#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "HueBoard.generated.h"

class APulseBlock;
class AHueSourceBlock;
class AHueReceiverBlock;
class AReactiveRig;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;

/**
 * One construction area (a tutorial exercise or the final exhibition box): a rectangular grid of cells with
 * the blocks registered on it, the mechanism it drives, its ordered objectives and its tuning values.
 *
 * Grid convention: the actor location is the south-west corner of cell (0,0); cell (x,y) spans
 * [x*CellSize, (x+1)*CellSize) along local X (East) and the same along local Y (North). Boards are expected
 * to be placed without rotation (the rig track and DOF locks are world-axis aligned).
 */
UCLASS()
class HUEBOX_API AHueBoard : public AActor
{
	GENERATED_BODY()

public:
	AHueBoard();

	// ---- Grid ---------------------------------------------------------------------------------------------
	FIntPoint GetGridSize() const { return GridSize; }
	float GetCellSize() const { return CellSize; }
	void SetGridSize(FIntPoint InSize);
	bool IsInside(FIntPoint Cell) const;
	/** World position of the centre of a cell, at the top surface of the board. */
	FVector CellToWorld(FIntPoint Cell) const;
	/** Returns false if the location is outside the grid. */
	bool WorldToCell(const FVector& WorldLocation, FIntPoint& OutCell) const;
	/** Board bounds (XY) grown by Margin, used to find the board the player stands at. */
	FBox GetWorldBounds(float Margin = 0.f) const;

	// ---- Occupancy ----------------------------------------------------------------------------------------
	APulseBlock* GetBlockAt(FIntPoint Cell) const;
	bool IsCellFree(FIntPoint Cell) const;
	/** Placement validation (inside, free, editing allowed). OutReason gets a short player-facing text. */
	bool CanPlaceAt(FIntPoint Cell, FString* OutReason = nullptr) const;
	/** Spawns BlockClass at Cell and registers it. Returns null (and does nothing) if the cell is invalid. */
	APulseBlock* SpawnBlock(TSubclassOf<APulseBlock> BlockClass, FIntPoint Cell, EHueDir Facing, bool bRemovable);
	/** Registers an existing block at Cell (snaps it to the cell centre). */
	bool RegisterBlock(APulseBlock* Block, FIntPoint Cell);
	void UnregisterBlock(APulseBlock* Block);
	/** Unregisters and destroys a block. */
	bool RemoveBlock(APulseBlock* Block);
	/** Removes every removable block (used by the self-test before installing a baseline solution). */
	void RemoveAllRemovableBlocks();
	void ForEachBlock(TFunctionRef<void(APulseBlock&)> Func) const;
	void GetSources(TArray<AHueSourceBlock*>& OutSources) const;
	AHueReceiverBlock* GetReceiver() const;
	int32 GetHeldSignalCount() const;
	void ResetAllBlocks();

	// ---- Mechanism & objectives -----------------------------------------------------------------------------
	AReactiveRig* GetRig() const { return Rig; }
	void SetRig(AReactiveRig* InRig);

	const TArray<FHueObjective>& GetObjectives() const { return Objectives; }
	void SetObjectives(const TArray<FHueObjective>& InObjectives);
	void ResetObjectives();
	/** Samples the rig and advances objective progress. Call every frame while the run is active. */
	void TickObjectives(float DeltaSeconds);
	/** Pure objective step (unit-testable): only the current objective can complete; returns true when all done. */
	bool EvaluateObjectives(const FHueRigSample& Sample, float DeltaSeconds);
	bool AreObjectivesComplete() const { return Objectives.Num() > 0 && CurrentObjective >= Objectives.Num(); }
	int32 GetCurrentObjectiveIndex() const { return CurrentObjective; }
	float GetHoldProgress() const { return HoldTimer; }

	// ---- Presentation / progression -------------------------------------------------------------------------
	const FString& GetBoardName() const { return BoardName; }
	const FString& GetBrief() const { return Brief; }
	void SetTexts(const FString& InName, const FString& InBrief);
	void SetHighlighted(bool bInHighlighted);
	bool IsHighlighted() const { return bHighlighted; }
	void SetCompleted(bool bInCompleted);
	bool IsCompleted() const { return bCompleted; }

	// ---- Tuning (GDD section 2 / 3) -----------------------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "Hue|Tuning", meta = (ClampMin = "0.5"))
	float CellsPerSecond = 4.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Tuning", meta = (ClampMin = "0"))
	float MixWindowSeconds = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Hue|Tuning", meta = (ClampMin = "1"))
	int32 MaxActiveSignals = 16;

	UPROPERTY(EditAnywhere, Category = "Hue|Tuning", meta = (ClampMin = "1"))
	int32 MaxOpsPerSignal = 64;

	UPROPERTY(EditAnywhere, Category = "Hue|Tuning", meta = (ClampMin = "1"))
	float RunTimeLimitSeconds = 20.f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;

	/** Registers APulseBlock actors that were placed in the level inside this board's bounds. */
	void RegisterExistingBlocksInBounds();
	void RebuildGridVisuals();
	void RefreshTitle();

	UPROPERTY(EditAnywhere, Category = "Hue|Grid")
	FIntPoint GridSize = FIntPoint(6, 6);

	UPROPERTY(EditAnywhere, Category = "Hue|Grid")
	float CellSize = 100.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Board")
	FString BoardName = TEXT("Board");

	UPROPERTY(EditAnywhere, Category = "Hue|Board", meta = (MultiLine = "true"))
	FString Brief;

	/** Required reference to the mechanism of this board. */
	UPROPERTY(EditAnywhere, Category = "Hue|Board")
	TObjectPtr<AReactiveRig> Rig;

	UPROPERTY(EditAnywhere, Category = "Hue|Board")
	TArray<FHueObjective> Objectives;

	/** Cell -> block. UPROPERTY so the GC sees the references (and nulls them if a block is force-destroyed). */
	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<APulseBlock>> Occupancy;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	/** Thin slab under the grid; also the trace target for "place on empty cell". */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> Floor;

	/** Grid lines (one instance per line) - a single draw call. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UInstancedStaticMeshComponent> GridLines;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> Title;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FloorMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LineMID;

	int32 CurrentObjective = 0;
	float HoldTimer = 0.f;
	bool bHighlighted = false;
	bool bCompleted = false;

private:
	/**
	 * Keeps Occupancy honest when a block leaves play without going through RemoveBlock (destroyed elsewhere,
	 * streamed out). Bound per block in RegisterBlock, removed in UnregisterBlock and EndPlay.
	 */
	UFUNCTION()
	void HandleBlockEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);

	/** Rotation + translation of the grid (cell math is in cm and ignores actor scale). */
	FTransform GetGridTransform() const;
	/** Valid registered blocks in a deterministic row-major order (y, then x): stable source order => stable runs. */
	void GetBlocksSorted(TArray<APulseBlock*, TInlineAllocator<64>>& OutBlocks) const;
	/** Cell under which Block is registered on this board (searches by value). */
	bool FindRegisteredCell(const APulseBlock* Block, FIntPoint& OutCell) const;
	/** Drops entries whose block is gone (GC-nulled or pending kill). */
	void PruneInvalidOccupancy();
	/** Fills the rig reference of receivers that have none (designer-placed boards). */
	void LinkReceiversToRig();
	void RefreshFloorColor();
};
