#include "Game/HueboxGameMode.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"

#include "Blocks/HueModuleBlocks.h"
#include "Game/ExhibitManager.h"
#include "Game/HueTutorialBuilder.h"
#include "Player/HueboxCharacter.h"
#include "Player/HueboxPlayerController.h"
#include "UI/HueboxHUD.h"

namespace HueGameModePrivate
{
	/** Native placeholder class for each inventory module type (None -> no class). */
	TSubclassOf<APulseBlock> NativeModuleClass(EHueModuleType Type)
	{
		switch (Type)
		{
		case EHueModuleType::Turn:       return AHueTurnBlock::StaticClass();
		case EHueModuleType::ColorShift: return AHueColorShiftBlock::StaticClass();
		case EHueModuleType::Delay:      return AHueDelayBlock::StaticClass();
		case EHueModuleType::Mixer:      return AHueMixerBlock::StaticClass();
		case EHueModuleType::None:
		default:                         return TSubclassOf<APulseBlock>();
		}
	}
}

AHueboxGameMode::AHueboxGameMode()
{
	DefaultPawnClass = AHueboxCharacter::StaticClass();
	PlayerControllerClass = AHueboxPlayerController::StaticClass();
	HUDClass = AHueboxHUD::StaticClass();
	TutorialBuilderClass = AHueTutorialBuilder::StaticClass();

	for (const EHueModuleType Type : { EHueModuleType::Turn, EHueModuleType::ColorShift, EHueModuleType::Delay, EHueModuleType::Mixer })
	{
		ModuleClasses.Add(Type, HueGameModePrivate::NativeModuleClass(Type));
	}
}

void AHueboxGameMode::StartPlay()
{
	// Runs before Super::StartPlay dispatches BeginPlay, so the manager and builder spawned here begin play with
	// the rest of the level (and boards spawned by the builder find the manager through GetExhibitManager()).
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		if (!IsValid(ExhibitManager))
		{
			// A manager placed in the level wins; it is only cached here.
			for (TActorIterator<AExhibitManager> It(World); It; ++It)
			{
				if (IsValid(*It))
				{
					ExhibitManager = *It;
					break;
				}
			}
		}
		if (!IsValid(ExhibitManager))
		{
			ExhibitManager = World->SpawnActor<AExhibitManager>(AExhibitManager::StaticClass(), FTransform::Identity, Params);
			if (!ExhibitManager)
			{
				UE_LOG(LogHuebox, Error, TEXT("HueboxGameMode: failed to spawn the exhibit manager"));
			}
		}

		TutorialBuilder = AHueTutorialBuilder::FindInWorld(this);
		if (!IsValid(TutorialBuilder) && bAutoSpawnTutorial)
		{
			UClass* BuilderClass = TutorialBuilderClass ? TutorialBuilderClass.Get() : AHueTutorialBuilder::StaticClass();
			TutorialBuilder = World->SpawnActor<AHueTutorialBuilder>(BuilderClass, FTransform::Identity, Params);
			if (TutorialBuilder)
			{
				UE_LOG(LogHuebox, Log, TEXT("HueboxGameMode: level has no tutorial builder - spawned %s"), *BuilderClass->GetName());
			}
			else
			{
				UE_LOG(LogHuebox, Error, TEXT("HueboxGameMode: failed to spawn the tutorial builder"));
			}
		}
	}

	Super::StartPlay();
}

AActor* AHueboxGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Any designer-placed start (including PIE "Play from here") -> default selection.
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		const APlayerStart* Candidate = *It;
		if (IsValid(Candidate) && Candidate != FallbackStartActor.Get())
		{
			return Super::ChoosePlayerStart_Implementation(Player);
		}
	}

	// Empty level: one fallback start in front of the first exercise, spawned once and reused (e.g. on restart).
	if (!IsValid(FallbackStartActor))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FallbackStartActor = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FallbackSpawn, Params);
		if (FallbackStartActor)
		{
			UE_LOG(LogHuebox, Log, TEXT("HueboxGameMode: no PlayerStart in the level - using fallback at %s"),
				*FallbackSpawn.GetLocation().ToString());
		}
	}

	return IsValid(FallbackStartActor) ? FallbackStartActor.Get() : Super::ChoosePlayerStart_Implementation(Player);
}

TSubclassOf<APulseBlock> AHueboxGameMode::GetModuleClass(EHueModuleType Type) const
{
	const TSubclassOf<APulseBlock> Native = HueGameModePrivate::NativeModuleClass(Type);

	if (const TSubclassOf<APulseBlock>* Found = ModuleClasses.Find(Type))
	{
		UClass* Class = Found->Get();
		// A replacement must be a concrete subclass of the native rule class (casts in gameplay code rely on it).
		if (Class && Native && Class->IsChildOf(Native.Get()) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return Class;
		}
		if (Class)
		{
			UE_LOG(LogHuebox, Warning, TEXT("HueboxGameMode: module class %s for %s is not a concrete %s - using the native class"),
				*Class->GetName(), HueModule::DisplayName(Type), Native ? *Native->GetName() : TEXT("module"));
		}
	}
	return Native;
}

TSubclassOf<APulseBlock> AHueboxGameMode::ResolveModuleClass(const UObject* WorldContext, EHueModuleType Type)
{
	const UWorld* World = (WorldContext && GEngine)
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;

	if (World)
	{
		if (const AHueboxGameMode* GameMode = Cast<AHueboxGameMode>(World->GetAuthGameMode()))
		{
			return GameMode->GetModuleClass(Type);
		}
	}
	// Tests / other game modes: the native placeholder classes.
	return HueGameModePrivate::NativeModuleClass(Type);
}
