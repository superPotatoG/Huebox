#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "HueboxHUD.generated.h"

class AHueboxCharacter;
class AExhibitManager;
class AHueBoard;

/**
 * Prototype HUD drawn with the Canvas (no widget assets needed; a UMG version can replace it later):
 *  - crosshair + interaction prompt
 *  - top-left: board name, run state, run time, ordered objectives (done / current / pending), hold progress
 *  - bottom: hotbar with module type + count, selected slot highlighted, preview facing
 *  - right: targeted block rule / state / latest input-output (mixer waits & failure cause, delay value)
 *  - left: recent feedback lines (fade out)
 *  - help overlay (H) and pause overlay (Esc / P)
 */
UCLASS()
class HUEBOX_API AHueboxHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

protected:
	void DrawCrosshairAndPrompt(AHueboxCharacter* Character);
	void DrawBoardPanel(AHueboxCharacter* Character, AExhibitManager* Manager);
	void DrawHotbar(AHueboxCharacter* Character);
	void DrawTargetPanel(AHueboxCharacter* Character);
	void DrawFeedback(AExhibitManager* Manager);
	void DrawHelp();
	void DrawPauseOverlay();

	/** Text with a dark backing box. Returns the height used. */
	float DrawLabel(const FString& Text, float X, float Y, const FLinearColor& Color, float Scale = 1.f, bool bBacking = true);

	/** Resolution scale (1 at 1080p, clamped), refreshed at the start of every DrawHUD. */
	float UIScale = 1.f;

	/** Bottom edge of the board panel this frame; the feedback list starts below it. */
	float LeftColumnBottom = 0.f;
};
