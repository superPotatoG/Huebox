#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HueInputConfig.generated.h"

class UInputAction;
class UInputMappingContext;

/**
 * Every input action of the game plus the default key mapping context, created in C++ at runtime
 * (NewObject) so the project needs no input assets. The character owns this object through a UPROPERTY,
 * which is what keeps all the actions / modifiers alive for the garbage collector.
 *
 * Default bindings:
 *   W A S D ............ Move              Mouse ........... Look
 *   Left mouse ......... Interact (collect pickup / collect block / place selected module)
 *   Q / E .............. Rotate target block or placement preview -90 / +90 degrees
 *   Z / X .............. Delay of targeted delay block -0.25 s / +0.25 s
 *   Mouse wheel, 1..6 .. Select hotbar slot
 *   G .................. Drop one of the selected module on the floor
 *   Space .............. Start / pause / resume the run of the board you stand at
 *   R .................. Stop and reset the attempt (layout is kept)
 *   H .................. Toggle help overlay
 *   Esc / P ............ Pause menu (Esc also ends Play-In-Editor; use P there)
 */
UCLASS()
class HUEBOX_API UHueInputConfig : public UObject
{
	GENERATED_BODY()

public:
	/** Creates all actions and the mapping context (idempotent). */
	void BuildDefaults();

	/** Sandbox / first-person context (AHueboxCharacter). */
	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient) TObjectPtr<UInputAction> Move;          // Axis2D
	UPROPERTY(Transient) TObjectPtr<UInputAction> Look;          // Axis2D
	UPROPERTY(Transient) TObjectPtr<UInputAction> Interact;      // Bool
	UPROPERTY(Transient) TObjectPtr<UInputAction> RotateLeft;    // Bool (Q)
	UPROPERTY(Transient) TObjectPtr<UInputAction> RotateRight;   // Bool (E)
	UPROPERTY(Transient) TObjectPtr<UInputAction> DelayDown;     // Bool (Z)
	UPROPERTY(Transient) TObjectPtr<UInputAction> DelayUp;       // Bool (X)
	UPROPERTY(Transient) TObjectPtr<UInputAction> HotbarScroll;  // Axis1D (wheel)
	UPROPERTY(Transient) TObjectPtr<UInputAction> HotbarSlot;    // Axis1D, keys 1..6 scaled to 1..6
	UPROPERTY(Transient) TObjectPtr<UInputAction> Drop;          // Bool (G)
	UPROPERTY(Transient) TObjectPtr<UInputAction> RunToggle;     // Bool (Space)
	UPROPERTY(Transient) TObjectPtr<UInputAction> ResetRun;      // Bool (R)
	UPROPERTY(Transient) TObjectPtr<UInputAction> ToggleHelp;    // Bool (H)
	UPROPERTY(Transient) TObjectPtr<UInputAction> PauseMenu;     // Bool (Esc / P), triggers while paused

	// ---- Instruction mode (AHueBuilderPawn) ----------------------------------------------------------------
	UPROPERTY(Transient) TObjectPtr<UInputAction> CursorMove;    // Axis2D: W/S/A/D + arrow keys (X = right, Y = up on screen)
	UPROPERTY(Transient) TObjectPtr<UInputAction> Secondary;     // Bool (RMB): collect the block under the cursor
	UPROPERTY(Transient) TObjectPtr<UInputAction> NextBoard;     // Bool (Tab, ])
	UPROPERTY(Transient) TObjectPtr<UInputAction> PrevBoard;     // Bool ([)

	/**
	 * Build view: CursorMove (WASD + arrows), Interact (LMB), Secondary (RMB), RotateLeft/Right (Q/E),
	 * DelayDown/Up (Z/X), HotbarScroll (wheel), HotbarSlot (1-6), RunToggle (Space), ResetRun (R),
	 * NextBoard (Tab, ]), PrevBoard ([), ToggleHelp (H), PauseMenu (Esc, P).
	 */
	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> BuildContext;

	/** Watch view: RunToggle (Space), ResetRun (R), NextBoard (Tab, ]), ToggleHelp (H), PauseMenu (Esc, P). */
	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> WatchContext;

	bool IsBuilt() const { return MappingContext != nullptr; }
};
