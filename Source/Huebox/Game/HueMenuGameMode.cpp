#include "Game/HueMenuGameMode.h"

#include "Blocks/PulseBlock.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Game/HueStageCamera.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Player/HueboxPlayerController.h"
#include "Rig/ReactiveRig.h"
#include "TimerManager.h"
#include "UI/HueboxHUD.h"

namespace HueMenuPrivate
{
	constexpr float TrackLength = 1000.f;

	/** The demo loop: what reaches the dancer at each beat and how long until the next one (seconds). R at the start
	 *  pushes her almost to the far end, C pulls her back, W stops her left of the middle; then the stage resets. */
	struct FDemoBeat
	{
		bool bReset = false;
		bool bColor = false;
		EHueColor Color = EHueColor::N;
		float NextIn = 1.f;
	};
	const FDemoBeat Beats[] =
	{
		{ true,  false, EHueColor::N, 0.8f },
		{ false, true,  EHueColor::A, 3.0f },
		{ false, true,  EHueColor::B, 3.4f },
		{ false, true,  EHueColor::N, 3.4f },
	};
}

AHueMenuGameMode::AHueMenuGameMode()
{
	PlayerControllerClass = AHueboxPlayerController::StaticClass();
	HUDClass = AHueboxHUD::StaticClass();
	// No pawn on the title screen (HandleStartingNewPlayer never restarts the player).
	DefaultPawnClass = nullptr;
}

void AHueMenuGameMode::StartPlay()
{
	// Before Super: the actors spawned here begin play with the rest of the level.
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		// A floor under the stage (like the exercise table's slab, only smaller).
		const FTransform GroundTransform(FRotator::ZeroRotator, FVector(500.f, 0.f, -10.f), FVector(30.f, 30.f, 0.2f));
		Ground = World->SpawnActorDeferred<AStaticMeshActor>(AStaticMeshActor::StaticClass(), GroundTransform, this, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Ground)
		{
			Ground->SetMobility(EComponentMobility::Movable);
			if (UStaticMeshComponent* Mesh = Ground->GetStaticMeshComponent())
			{
				Mesh->SetStaticMesh(APulseBlock::GetCubeMesh());
				Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
				if (UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial())
				{
					// Outer = component; its material slot (a UPROPERTY) keeps the MID alive.
					UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, Mesh);
					MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.32f, 0.32f, 0.32f));
					Mesh->SetMaterial(0, MID);
				}
			}
			Ground->FinishSpawning(GroundTransform);
		}

		// The rig (deferred: its construction lays out the track from TrackLength).
		const FTransform RigTransform(FVector::ZeroVector);
		DemoRig = World->SpawnActorDeferred<AReactiveRig>(AReactiveRig::StaticClass(), RigTransform, this, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (DemoRig)
		{
			DemoRig->TrackLength = HueMenuPrivate::TrackLength;
			DemoRig->FinishSpawning(RigTransform);
		}

		DemoCamera = World->SpawnActor<AHueStageCamera>(AHueStageCamera::StaticClass(), FTransform::Identity, Params);
		if (DemoCamera && DemoRig)
		{
			DemoCamera->SetRig(DemoRig, /*bSnap*/ true);
		}
	}

	Super::StartPlay();

	// Players usually logged in before StartPlay (HandleStartingNewPlayer had no camera to show yet).
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ShowStage(It->Get());
	}

	DemoStep = 0;
	StepDemo();

	// Visual check, as in the levels (AExhibitManager): -HueShot=<file.png> [-HueShotRun=<s>] saves the title screen
	// after <s> seconds of the demo (default 4) and quits. Use with -RenderOffscreen.
	FString ShotPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("HueShot="), ShotPath))
	{
		float Seconds = 4.f;
		FParse::Value(FCommandLine::Get(), TEXT("HueShotRun="), Seconds);
		GetWorldTimerManager().SetTimer(ShotTimer, FTimerDelegate::CreateWeakLambda(this, [ShotPath]()
		{
			FScreenshotRequest::RequestScreenshot(ShotPath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}), 1.5f);
		}), FMath::Max(Seconds, 0.5f), false);
	}
}

void AHueMenuGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// Deliberately no Super: it would restart the player, i.e. spawn a pawn.
	if (AHueboxPlayerController* PC = Cast<AHueboxPlayerController>(NewPlayer))
	{
		PC->OpenMainMenu();
	}
	ShowStage(NewPlayer);
}

void AHueMenuGameMode::ShowStage(APlayerController* PC) const
{
	if (PC && DemoCamera)
	{
		PC->SetViewTarget(DemoCamera);
	}
}

void AHueMenuGameMode::StepDemo()
{
	if (!IsValid(DemoRig))
	{
		return;
	}

	const HueMenuPrivate::FDemoBeat& Beat = HueMenuPrivate::Beats[DemoStep % UE_ARRAY_COUNT(HueMenuPrivate::Beats)];
	if (Beat.bReset)
	{
		DemoRig->ResetRig();
		DemoRig->SetMotionActive(true);
	}
	if (Beat.bColor)
	{
		DemoRig->ApplyColor(Beat.Color);
	}
	++DemoStep;

	// A UObject method, cleared in EndPlay: nothing can call back into a torn-down game mode.
	GetWorldTimerManager().SetTimer(DemoTimer, this, &AHueMenuGameMode::StepDemo, Beat.NextIn, /*bLoop*/ false);
}

void AHueMenuGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(DemoTimer);
	GetWorldTimerManager().ClearTimer(ShotTimer);
	Super::EndPlay(EndPlayReason);
}
