#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "HueboxPlayerController.generated.h"

/** Game-only input mode, hidden cursor (crosshair aiming). Pause menu toggles the game pause. */
UCLASS()
class HUEBOX_API AHueboxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AHueboxPlayerController();

	/** Pauses / unpauses the game (input actions with bTriggerWhenPaused still fire). */
	void SetPauseMenuOpen(bool bOpen);

protected:
	virtual void BeginPlay() override;
};
