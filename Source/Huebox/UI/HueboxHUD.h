#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "HueboxHUD.generated.h"

class APawn;
class AExhibitManager;
class AHueBoard;
class UHueBuildToolComponent;

/**
 * Prototype HUD drawn with the Canvas (no widget assets needed; a UMG version can replace it later).
 * The layout follows the owning pawn (the build tool is found with FindComponentByClass, so both pawns share
 * the target / inventory code paths):
 *
 *   Sandbox (AHueboxCharacter, first person)
 *     - crosshair + interaction prompt, bottom hotbar with preview facing
 *     - top-left: board name, run state, run time, ordered objectives, hold progress
 *     - right: targeted block rule / state / ports / delay value
 *   Instruction, Build view (AHueBuilderPawn above a board)
 *     - top-left board panel, below it the board's tray (selected slot amber) and the preview facing
 *     - right: cursor target panel; bottom centre: prompt + key hint bar (no crosshair, the OS cursor is visible)
 *   Instruction, Watch view (side stage camera)
 *     - top-left board panel (state, time, objectives, performer), bottom-right minimap of the grid with
 *       letter-coded blocks and live pulses, key hint bar, end-of-run banner
 *
 * Every layout: recent feedback lines under the left column (fade out), help overlay (H, bindings of the current
 * mode) and pause overlay (Esc / P).
 */
UCLASS()
class HUEBOX_API AHueboxHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

protected:
	/** Which layout is drawn this frame (plain C++ enum, not reflected). */
	enum class EHudContext : uint8
	{
		/** No Huebox pawn (spectating, loading): board panel + feedback only. */
		None,
		Sandbox,
		Build,
		Watch
	};

	/** Layout for the owning pawn: its class, and for the builder pawn its current view. */
	static EHudContext ResolveContext(const APawn* Pawn);

	/** Board the panels describe: the running board while a run exists, else the focused / nearest board. */
	AHueBoard* ResolveBoard(const APawn* Pawn, const AExhibitManager* Manager) const;

	// Shared ------------------------------------------------------------------------------------------------
	void DrawBoardPanel(const APawn* Pawn, AExhibitManager* Manager, EHudContext Context);
	void DrawTargetPanel(const UHueBuildToolComponent* Tool, EHudContext Context);
	void DrawFeedback(AExhibitManager* Manager);
	void DrawHelp(EHudContext Context);
	void DrawPauseOverlay();

	// Sandbox -----------------------------------------------------------------------------------------------
	void DrawCrosshairAndPrompt(const UHueBuildToolComponent* Tool, APawn* Pawn);
	void DrawHotbar(const UHueBuildToolComponent* Tool);

	// Instruction -------------------------------------------------------------------------------------------
	/** Build view: vertical list of the focused board's tray under the board panel. */
	void DrawTrayPanel(const UHueBuildToolComponent* Tool);
	/** Build view: what LMB / RMB would do on the cursor cell, centred above the hint bar. */
	void DrawBuildPrompt(const UHueBuildToolComponent* Tool, APawn* Pawn);
	/** Key hints of the Build / Watch view, centred at the bottom (sets HintBarTop / HintBarRight). */
	void DrawHintBar(EHudContext Context);
	/** Watch view: top-down map of Board (screen right = +X, screen up = -Y) with blocks and live pulses. */
	void DrawMinimap(const AHueBoard* Board);
	/** Watch view: centred "complete" / "ended" banner with the next steps. */
	void DrawRunBanner(const AExhibitManager* Manager);

	/** Text with a dark backing box. Returns the height used. */
	float DrawLabel(const FString& Text, float X, float Y, const FLinearColor& Color, float Scale = 1.f, bool bBacking = true);

	/** Resolution scale (1 at 1080p, clamped), refreshed at the start of every DrawHUD. */
	float UIScale = 1.f;

	/** Bottom edge of the left column (board panel, tray) this frame; the feedback list starts below it. */
	float LeftColumnBottom = 0.f;

	/** Top / right edge of the bottom hint bar this frame (no bar: bottom margin / 0). Prompt, help and minimap
	 *  keep clear of it. */
	float HintBarTop = 0.f;
	float HintBarRight = 0.f;

	/** The run is Complete / Ended this frame: in the Watch view Space replays, so hint bar and help say so. */
	bool bRunFinished = false;
};
