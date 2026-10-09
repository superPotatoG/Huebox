#include "Player/HueboxPlayerController.h"

#include "Core/HueTypes.h"
#include "Kismet/GameplayStatics.h"

AHueboxPlayerController::AHueboxPlayerController()
{
	// Aiming uses the screen-centre crosshair, never the OS cursor.
	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
}

void AHueboxPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
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
