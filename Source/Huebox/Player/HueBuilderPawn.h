#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Core/HueTypes.h"
#include "HueBuilderPawn.generated.h"

class UCameraComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UHueBuildToolComponent;
class UHueInputConfig;
class AHueBoard;
class AHueStageCamera;
class AExhibitManager;
class APlayerController;
class UInputMappingContext;
struct FInputActionValue;

/**
 * Instruction-mode player (GDD workbench view, two pictures per exercise):
 *   Build view - camera locked straight above the focused board (screen right = +X, screen up = -Y); a cell cursor
 *                limited to the grid, driven by mouse hover or WASD / arrows; LMB place / collect, RMB collect,
 *                Q/E rotate, Z/X delay, wheel / 1-6 tray slot, Tab / ] next board, [ previous board.
 *   Watch view - Space starts the run: the view blends to AHueStageCamera following the performer from the side;
 *                the HUD draws a minimap of the grid with live pulses. Space pauses / resumes; R resets and returns
 *                to the Build view; after Complete / Ended, Tab goes on to the next board.
 * The view follows AExhibitManager::OnStateChanged (Build -> Build view, every other state -> Watch view), and the
 * matching input mapping context (UHueInputConfig::BuildContext / WatchContext) is swapped in with it.
 * The pawn's own location stays above the focused board, so manager queries "board near the player" just work.
 */
UCLASS()
class HUEBOX_API AHueBuilderPawn : public APawn
{
	GENERATED_BODY()

public:
	AHueBuilderPawn();

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;

	// ---- Boards ---------------------------------------------------------------------------------------------
	/** Moves the build view to Board (camera glides unless bSnap) and makes its tray the active inventory. */
	void FocusBoard(AHueBoard* Board, bool bSnap = false);
	/** Index into AExhibitManager::GetBoards() (progression order). */
	void FocusBoardIndex(int32 Index, bool bSnap = false);
	/** Delta = +1 next / -1 previous, wrapping. Refused (with a message) while a run is Running / Paused. */
	void FocusNextBoard(int32 Delta);
	AHueBoard* GetFocusedBoard() const { return FocusedBoard.Get(); }

	// ---- State for the HUD ----------------------------------------------------------------------------------
	FIntPoint GetCursorCell() const { return CursorCell; }
	bool HasCursor() const;
	EHueViewMode GetViewMode() const { return ViewMode; }
	UHueBuildToolComponent* GetBuildTool() const { return BuildTool; }
	AHueStageCamera* GetStageCamera() const { return StageCamera; }
	bool IsHelpVisible() const { return bShowHelp; }
	bool IsPauseMenuOpen() const { return bPauseMenuOpen; }

	// ---- Pure math (unit-tested) ------------------------------------------------------------------------------
	/** Camera height above the board so that an AreaSize (cm, X by Y) rectangle fits a perspective view looking
	 *  straight down with the given horizontal FOV (degrees) and aspect ratio (width / height). */
	static float ComputeTopDownHeight(const FVector2D& AreaSize, float HorizontalFovDegrees, float AspectRatio);
	/** Cell clamped into [0, GridSize - 1] on both axes. */
	static FIntPoint ClampCursor(FIntPoint Cell, FIntPoint GridSize);

	// ---- Tuning ---------------------------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float CameraFov = 55.f;

	/** Extra space (cm) around the grid on every side; the stage just behind the grid peeks in at the top. */
	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float CameraMargin = 260.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float CameraInterpSpeed = 6.f;

	/** Seconds of the camera blend between the Build and Watch views. */
	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float ViewBlendTime = 0.7f;

	/** Held WASD / arrows: first repeat after KeyRepeatDelay, then every KeyRepeatInterval seconds. */
	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float KeyRepeatDelay = 0.35f;

	UPROPERTY(EditAnywhere, Category = "Hue|Build View")
	float KeyRepeatInterval = 0.11f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Input handlers ---------------------------------------------------------------------------------------
	void HandleCursorMove(const FInputActionValue& Value);
	void HandleCursorMoveReleased(const FInputActionValue& Value);
	void HandleInteract();
	void HandleSecondary();
	void HandleRotateLeft();
	void HandleRotateRight();
	void HandleDelayDown();
	void HandleDelayUp();
	void HandleHotbarScroll(const FInputActionValue& Value);
	void HandleHotbarSlot(const FInputActionValue& Value);
	void HandleRunToggle();
	void HandleReset();
	void HandleNextBoard();
	void HandlePrevBoard();
	void HandleToggleHelp();
	void HandlePauseMenu();

	/** Bound to AExhibitManager::OnStateChanged (dynamic delegate, unbound in EndPlay). */
	UFUNCTION()
	void HandleExhibitStateChanged(EHueRunState NewState);

	void EnsureInputConfig();
	/** Binds to the manager once it exists and focuses the first board once boards exist (both may appear after
	 *  this pawn's BeginPlay, so Tick retries). */
	void TryLateInitialize();
	void EnterViewMode(EHueViewMode NewMode, bool bInstant);
	/** Puts the mapping context of the current view on the local player's input stack (removes the other). */
	void ApplyMappingContext();
	void RemoveMappingContexts();
	void UpdateCameraGoal(bool bSnap);
	void UpdateCursorFromMouse();
	void MoveCursor(FIntPoint Delta);
	void UpdateCursorVisual();
	/** Pushes the cursor cell into the build tool as its target (Build view) or clears it (Watch view). */
	void PushToolTarget();

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UHueBuildToolComponent> BuildTool;

	/** Square outline under the cursor cell (absolute transform, no collision). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> CursorFrame;

	/** The four thin bars of the hollow outline, attached to CursorFrame (which carries no mesh itself), so the
	 *  block on the cell stays visible inside the frame. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TArray<TObjectPtr<UStaticMeshComponent>> CursorEdges;

	/** Amber flat material shared by the cursor bars (created in BeginPlay). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CursorMID;

	/** Cell size the bars are laid out for; re-laid out when a board with another cell size is focused. */
	float CursorLayoutCellSize = 0.f;

	/** Runtime-created input actions + contexts; the UPROPERTY keeps them alive. */
	UPROPERTY(Transient)
	TObjectPtr<UHueInputConfig> InputConfig;

	/** Side camera of the Watch view, spawned in BeginPlay and destroyed in EndPlay (owned by this pawn). */
	UPROPERTY(Transient)
	TObjectPtr<AHueStageCamera> StageCamera;

	TWeakObjectPtr<AHueBoard> FocusedBoard;
	TWeakObjectPtr<AExhibitManager> BoundManager;
	TWeakObjectPtr<APlayerController> MappedController;
	/** Context currently on the input stack (weak: owned by InputConfig). */
	TWeakObjectPtr<UInputMappingContext> ActiveContext;

	FIntPoint CursorCell = FIntPoint(0, 0);
	/** Mouse moves take over the cursor; keyboard moves keep it until the mouse moves again. */
	FVector2D LastMousePosition = FVector2D(-1.f, -1.f);
	FIntPoint HeldCursorDirection = FIntPoint::ZeroValue;
	float HeldCursorTime = 0.f;
	float NextCursorRepeat = 0.f;

	FVector CameraGoal = FVector::ZeroVector;
	EHueViewMode ViewMode = EHueViewMode::Build;
	bool bShowHelp = true;
	bool bPauseMenuOpen = false;
};
