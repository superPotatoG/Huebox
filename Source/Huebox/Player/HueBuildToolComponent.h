#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/HueTypes.h"
#include "HueBuildToolComponent.generated.h"

class APulseBlock;
class AHueBoard;
class AHueModulePickup;
class UHueInventoryComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
struct FHitResult;

UENUM()
enum class EHueTargetKind : uint8
{
	None,
	Block,
	Pickup,
	Cell
};

/** What the player currently points at (crosshair or build cursor). All object references are weak. */
struct FHueInteractionTarget
{
	EHueTargetKind Kind = EHueTargetKind::None;
	TWeakObjectPtr<APulseBlock> Block;
	TWeakObjectPtr<AHueModulePickup> Pickup;
	TWeakObjectPtr<AHueBoard> Board;
	FIntPoint Cell = FIntPoint(INDEX_NONE, INDEX_NONE);
	/** For Cell targets: can the selected module be placed here? */
	bool bCanPlace = false;
	/** Player-facing reason when bCanPlace is false. */
	FString Reason;
};

/** Owner veto for placing on a cell (e.g. the first-person pawn stands on it). Return true = blocked. */
DECLARE_DELEGATE_RetVal_TwoParams(bool, FHueCellBlockedQuery, const AHueBoard* /*Board*/, FIntPoint /*Cell*/);

/**
 * The maker's construction tool, shared by every player pawn:
 *   - AHueBuilderPawn (Instruction mode): target set from the top-down cursor, inventory = focused board's tray;
 *   - AHueboxCharacter (Sandbox mode): target set from the camera trace, inventory = the character's own.
 * Owns the placement rules (editing lock, CanPlaceAt, inventory transactions), the placement ghost + facing arrow,
 * block highlighting and the player messages. Pawns only translate input into these calls.
 */
UCLASS(ClassGroup = (Huebox), meta = (BlueprintSpawnableComponent))
class HUEBOX_API UHueBuildToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHueBuildToolComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ---- Inventory source ---------------------------------------------------------------------------------
	/** Which inventory pays for placements / receives collected blocks (weak: the tool never owns it). */
	void SetInventory(UHueInventoryComponent* InInventory);
	UHueInventoryComponent* GetInventory() const;

	// ---- Targeting (owners call one of these every tick) --------------------------------------------------
	/** From a line trace: block / pickup / board floor cell. A miss = ClearTarget. */
	void SetTargetFromHit(const FHitResult& Hit);
	/** From a grid cursor: the block on the cell if any, otherwise the empty cell. */
	void SetTargetCell(AHueBoard* Board, FIntPoint Cell);
	void ClearTarget();
	/** Re-evaluates the current target (cell occupancy, editing lock, selection) without moving it. */
	void RefreshTarget();
	const FHueInteractionTarget& GetTarget() const { return Target; }

	// ---- Actions --------------------------------------------------------------------------------------------
	/** LMB: pickup -> collect it; removable block -> collect it; empty cell -> place the selected module. */
	void PrimaryAction();
	/** RMB: collect the targeted block (nothing else). */
	void SecondaryAction();
	/** Q / E: rotate the targeted removable block, otherwise the placement preview. */
	void RotateTargetOrPreview(int32 Steps);
	/** Z / X: +/- Steps * 0.25 s on the targeted delay block. */
	void AdjustTargetDelay(int32 Steps);
	/** Wheel / 1..6 on the active inventory. */
	void CycleSelection(int32 Delta);
	void SelectSlot(int32 Index);

	bool TryPlaceSelected();
	bool TryCollectBlock(APulseBlock* Block);
	/** Returns the number of modules taken. */
	int32 TryCollectPickup(AHueModulePickup* Pickup);

	EHueDir GetPreviewFacing() const { return PreviewFacing; }
	void SetPreviewFacing(EHueDir InFacing) { PreviewFacing = InFacing; }

	/** Forwarded to the exhibit feedback log (or the output log if there is no manager). */
	void ShowMessage(EHueFeedbackKind Kind, const FString& Message) const;
	/** Build-state check through the exhibit manager (no manager => editing allowed). */
	bool IsEditingAllowed() const;

	/** Optional owner veto for placement (see FHueCellBlockedQuery) and the reason shown when it vetoes. */
	FHueCellBlockedQuery CellBlockedQuery;
	FString CellBlockedReason = TEXT("Step off this cell to place here");

	/** Show the ghost tile + facing arrow on empty cell targets. */
	UPROPERTY(EditAnywhere, Category = "Hue|Build")
	bool bShowPreview = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Fills Out for Board/Cell (block target if occupied, else cell target with bCanPlace / Reason). */
	void BuildCellTarget(AHueBoard* Board, FIntPoint Cell, FHueInteractionTarget& Out) const;
	void ApplyTarget(FHueInteractionTarget&& NewTarget);
	void UpdateHighlight();
	void UpdatePreview();
	void ClearHighlight();
	void SelectIfNothingSelected(EHueModuleType Type);
	void SelectNextFilledSlotIfEmpty();

	/** Runtime components created on the owner in BeginPlay (absolute transforms, no collision). */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> PreviewTile;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> PreviewArrow;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PreviewMID;

	TWeakObjectPtr<UHueInventoryComponent> Inventory;
	FHueInteractionTarget Target;
	TWeakObjectPtr<APulseBlock> HighlightedBlock;
	EHueDir PreviewFacing = EHueDir::East;
	/** Last colour state written to PreviewMID (-1 unknown, 0 invalid, 1 valid). */
	int8 PreviewColorState = -1;
};
