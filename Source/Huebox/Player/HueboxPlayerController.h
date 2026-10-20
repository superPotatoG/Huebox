#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "HueboxPlayerController.generated.h"

/**
 * Input mode follows the possessed pawn:
 *   AHueBuilderPawn (Instruction) - OS cursor visible, game-and-UI input (hover picks cells, clicks stay in game);
 *   AHueboxCharacter (Sandbox)    - game-only input, hidden cursor (screen-centre crosshair aiming).
 * Pause menu toggles the game pause.
 */
UCLASS()
class HUEBOX_API AHueboxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AHueboxPlayerController();

	/** Pauses / unpauses the game (input actions with bTriggerWhenPaused still fire). */
	void SetPauseMenuOpen(bool bOpen);

	/** Applies the cursor / input mode for InPawn (called on BeginPlay and on every possession). */
	void ApplyInputModeForPawn(const APawn* InPawn);

protected:
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
};
