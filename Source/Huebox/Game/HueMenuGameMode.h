#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "HueMenuGameMode.generated.h"

class AReactiveRig;
class AHueStageCamera;
class AStaticMeshActor;

/**
 * Title screen (map Lvl_MainMenu). No pawn: the player controller shows the main menu over a small stage where the
 * dancer performs on her own, in a loop - pushed away (R), pulled back (C), braked (W) - with the reason above her
 * head each time, so the title already shows what the colours do.
 */
UCLASS()
class HUEBOX_API AHueMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AHueMenuGameMode();

	virtual void StartPlay() override;
	/** No pawn: the main menu instead (and the stage camera as the view, once it exists). */
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Next beat of the demo loop (reset, R, C, W, ...); re-arms its own timer. */
	void StepDemo();
	void ShowStage(APlayerController* PC) const;

	UPROPERTY(Transient)
	TObjectPtr<AReactiveRig> DemoRig;

	UPROPERTY(Transient)
	TObjectPtr<AHueStageCamera> DemoCamera;

	UPROPERTY(Transient)
	TObjectPtr<AStaticMeshActor> Ground;

	FTimerHandle DemoTimer;
	/** -HueShot visual check of the title screen. */
	FTimerHandle ShotTimer;
	int32 DemoStep = 0;
};
