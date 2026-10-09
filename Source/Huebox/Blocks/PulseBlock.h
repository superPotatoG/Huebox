#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "Player/HueInteractable.h"
#include "PulseBlock.generated.h"

class UPulseSubsystem;
class AHueBoard;
class UStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UMaterialInterface;

/** How a face of a block behaves, used for placeholder port markers and HUD text. */
UENUM()
enum class EHuePortKind : uint8
{
	Input,
	Output,
	/** Bidirectional port (turn block corner). */
	Both
};

USTRUCT()
struct FHuePort
{
	GENERATED_BODY()

	/** Board-local face of the cell (the face a pulse enters through / leaves through). */
	UPROPERTY()
	EHueDir Face = EHueDir::East;

	UPROPERTY()
	EHuePortKind Kind = EHuePortKind::Input;
};

/**
 * Common base for everything that occupies a grid cell: modules, sources, receivers, barriers.
 *
 * The pulse simulation only ever talks to this base class (ReceivePulse / OnScheduledEvent / OnTimeGroupEnd /
 * ResetBlock). Subclasses override the behaviour; adding a new module type never touches the caller.
 *
 * Port convention: a pulse travelling in direction D enters a cell through face HueDir::Opposite(D) and leaves
 * through face D.
 */
UCLASS(Abstract)
class HUEBOX_API APulseBlock : public AActor, public IHueInteractable
{
	GENERATED_BODY()

public:
	APulseBlock();

	// ---- Simulation interface (called by UPulseSubsystem only) -------------------------------------------------

	/**
	 * A pulse entered this block's cell. Pulse.Direction is its travel direction on entry; the block may modify
	 * Pulse.Color / Pulse.Direction and return Continue, or return Consumed. Base implementation: blocks entry.
	 * The subsystem has already spent one of Pulse.RemainingOps for this operation.
	 */
	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim);

	/** A block event scheduled with UPulseSubsystem::ScheduleBlockEvent fired. */
	virtual void OnScheduledEvent(int32 EventTag, UPulseSubsystem& Sim) {}

	/** Called once after all events of one logical time step were processed, for blocks touched in that step. */
	virtual void OnTimeGroupEnd(UPulseSubsystem& Sim) {}

	/** Restore run-time state (buffers, waiting state, colours). Never changes Cell/Facing (layout survives reset). */
	virtual void ResetBlock();

	/** Signals currently stored inside this block (delay buffer, mixer waiting input). Counts toward the 16 cap. */
	virtual int32 GetHeldSignalCount() const { return 0; }

	// ---- Description (HUD) -----------------------------------------------------------------------------------

	/** One short rule, e.g. "Swaps A and B; N passes unchanged." */
	virtual FString GetRuleText() const;
	/** Current state + latest input/output, e.g. "Holding A, releases in 0.75 s". */
	virtual FString GetStateText() const;
	virtual FString GetDisplayName() const;

	/** Faces and their roles for the current Facing. */
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const {}

	// ---- Grid placement --------------------------------------------------------------------------------------

	/** Called by AHueBoard when the block is registered at a cell. */
	void InitializeOnBoard(AHueBoard* Board, FIntPoint InCell);
	AHueBoard* GetBoard() const { return OwningBoard.Get(); }
	FIntPoint GetCell() const { return Cell; }
	EHueDir GetFacing() const { return Facing; }
	/** Sets facing and refreshes rotation + port markers. */
	void SetFacing(EHueDir NewFacing);
	/** Quarter-turn rotation (Q / E). */
	void RotateBy(int32 Steps);
	virtual bool CanRotate() const { return bRemovable; }

	bool IsRemovable() const { return bRemovable; }
	void SetRemovable(bool bInRemovable) { bRemovable = bInRemovable; }
	EHueModuleType GetModuleType() const { return ModuleType; }

	// ---- Presentation ----------------------------------------------------------------------------------------

	void SetHighlighted(bool bInHighlighted);
	/** Briefly tints the block red and shows a cross on Face (blocked entry, occupied delay, mixer failure...). */
	void FlashBlocked(EHueDir Face);
	/** Re-applies colours / labels from current state. Subclasses extend. */
	virtual void RefreshVisuals();

	// ---- IHueInteractable ----------------------------------------------------------------------------------
	virtual FString GetInteractPrompt_Implementation(AHueboxCharacter* Interactor) const override;
	virtual bool Interact_Implementation(AHueboxCharacter* Interactor) override;

	/** Placeholder shape assets (engine basic shapes), shared by subclasses. */
	static UStaticMesh* GetCubeMesh();
	static UStaticMesh* GetConeMesh();
	static UStaticMesh* GetSphereMesh();
	static UStaticMesh* GetCylinderMesh();
	static UMaterialInterface* GetBasicMaterial();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;

	/** Rebuilds the small marker components that show input/output faces. */
	void RebuildPortMarkers();

	/** Colour of the body when idle (subclasses set it in their constructor). */
	UPROPERTY(EditAnywhere, Category = "Hue|Visual")
	FLinearColor BaseColor = FLinearColor(0.6f, 0.6f, 0.6f);

	/** Body scale in metres-ish units of the 100 cm engine cube. */
	UPROPERTY(EditAnywhere, Category = "Hue|Visual")
	FVector BodyScale = FVector(0.7f, 0.7f, 0.5f);

	UPROPERTY(VisibleAnywhere, Category = "Hue|Grid")
	FIntPoint Cell = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, Category = "Hue|Grid")
	EHueDir Facing = EHueDir::East;

	/** Fixed sources, receivers and barriers are not removable. */
	UPROPERTY(EditAnywhere, Category = "Hue|Grid")
	bool bRemovable = true;

	/** Inventory item returned when the player collects this block (None for fixed units). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Grid")
	EHueModuleType ModuleType = EHueModuleType::None;

	/** Non-owning back reference: the board owns the block, not the other way round. */
	TWeakObjectPtr<AHueBoard> OwningBoard;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Rotates with Facing; body, arrows and port markers hang below it. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> Pivot;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	/** Text on the top face (does not rotate with Facing so it stays readable). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> Label;

	/** Runtime material instance for the body. Created in BeginPlay; UPROPERTY keeps it alive for the GC. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMID;

	/** Port marker components created at runtime (NewObject + RegisterComponent); destroyed on rebuild. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PortMarkers;

	/** Last input / output strings for the HUD ("in: B from West @ 1.25 s"). */
	FString LastInputText;
	FString LastOutputText;

	bool bHighlighted = false;
	bool bFlashing = false;
	FTimerHandle FlashTimer;

	/** Helper for subclasses: set a material colour on a component via a per-component MID. */
	UMaterialInstanceDynamic* MakeColorMID(UPrimitiveComponent* Component, const FLinearColor& Color);
	void SetBodyColor(const FLinearColor& Color);
	void SetLabelText(const FString& Text, const FLinearColor& Color = FLinearColor::White);

	/** Standard log of an input for HUD state text. */
	void NoteInput(const FPulseData& Pulse, const UPulseSubsystem& Sim);
	void NoteOutput(const FString& Text);

	/** Report "blocked" feedback for a pulse that hit a face without a port. */
	void ReportBlocked(const FPulseData& Pulse, UPulseSubsystem& Sim, const FString& Why);

	/** Player-facing name ("Turn", "Colour shift", "Source", ...). Set by every subclass constructor. */
	FString BlockDisplayName;

	/** Label the subclass wants on the top face. Shown unless a blocked flash temporarily replaces it. */
	FString LabelString;
	FLinearColor LabelColor = FLinearColor::White;

	/** Applies BodyScale to the body mesh and the label height (constructors / OnConstruction). */
	void ApplyBodyLayout();

	/** "Turn (3,2)" - prefix of every feedback line this block raises. */
	FString DescribeBlock() const;

	/** Current logical time of the pulse simulation, for HUD text only (0 without a subsystem). */
	int64 GetHudSimTimeMs() const;

	/** "out: A to North @1.25s" */
	static FString FormatOutput(EHueColor Color, EHueDir Dir, int64 TimeMs);

	/** Readable label colour on top of Background: dark text on light bodies, white on dark ones. */
	static FLinearColor ContrastTextColor(const FLinearColor& Background);

private:
	void EndFlash();
	void ApplyFacingRotation();
	/** Pushes LabelString / LabelColor (or the red flash cross) to the text component. */
	void ApplyLabel();

	/** Face the current blocked flash refers to; the red "X" is drawn towards it. */
	EHueDir FlashFace = EHueDir::East;
};
