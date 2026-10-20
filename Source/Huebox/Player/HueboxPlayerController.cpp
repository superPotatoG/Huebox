#include "Player/HueboxPlayerController.h"

#include "Core/HueTypes.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Player/HueBuilderPawn.h"

AHueboxPlayerController::AHueboxPlayerController()
{
	// The cursor follows the possessed pawn (ApplyInputModeForPawn). Cells are picked by our own deprojection, not
	// by engine click / hover events.
	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
}

void AHueboxPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// The pawn is usually possessed before BeginPlay (OnPossess already applied the mode); harmless to repeat.
	ApplyInputModeForPawn(GetPawn());
}

void AHueboxPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	ApplyInputModeForPawn(InPawn);
}

void AHueboxPlayerController::ApplyInputModeForPawn(const APawn* InPawn)
{
	if (!IsLocalController())
	{
		return;
	}

	if (InPawn && InPawn->IsA<AHueBuilderPawn>())
	{
		// Instruction / top-down: the OS cursor hovers cells; clicks are captured only while held, so LMB / RMB still
		// reach the input actions and the cursor never vanishes or gets trapped in the window.
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);
		bShowMouseCursor = true;
	}
	else
	{
		// Sandbox / first person: the mouse looks around, aiming uses the screen-centre crosshair.
		FInputModeGameOnly InputMode;
		InputMode.SetConsumeCaptureMouseDown(true);
		SetInputMode(InputMode);
		bShowMouseCursor = false;
	}
}

void AHueboxPlayerController::SetPauseMenuOpen(bool bOpen)
{
	// Input actions flagged bTriggerWhenPaused (the pause key) keep working while the world is paused,
	// so the same key closes the menu again.
	if (!UGameplayStatics::SetGamePaused(this, bOpen))
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueboxPlayerController: could not %s the game"), bOpen ? TEXT("pause") : TEXT("unpause"));
	}
}
