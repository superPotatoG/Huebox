#include "Grid/HueBoard.h"

#include "Blocks/PulseBlock.h"
#include "Blocks/HueFixedBlocks.h"
#include "Rig/ReactiveRig.h"
#include "Game/ExhibitManager.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HueBoardPrivate
{
	constexpr float FloorThickness = 2.f;          // cm, floor top = actor Z + 2
	constexpr float LineWidth = 1.5f;              // cm
	constexpr float LineHeight = 0.6f;             // cm, sits on top of the floor
	constexpr float TitleHeight = 230.f;
	constexpr float TitleWorldSize = 40.f;
	constexpr int32 MaxGridDimension = 64;
	/** Level-placed blocks further than this above/below the floor are not adopted by the board. */
	constexpr float AdoptMaxHeight = 200.f;

	static const FLinearColor FloorColor(0.72f, 0.62f, 0.48f);
	static const FLinearColor FloorHighlightColor(0.85f, 0.78f, 0.6f);
	static const FLinearColor LineColor(0.25f, 0.2f, 0.15f);
	static const FLinearColor TitleCompletedColor(0.25f, 0.85f, 0.35f);

	/** Row-major order: south row first, west to east inside a row. */
	static bool CellLess(const FIntPoint& A, const FIntPoint& B)
	{
		return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
	}
}

AHueBoard::AHueBoard()
{
	// The exhibit manager drives TickObjectives; the board itself never ticks.
	PrimaryActorTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Floor = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Floor"));
	Floor->SetupAttachment(SceneRoot);
	Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Floor->SetGenerateOverlapEvents(false);

	GridLines = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GridLines"));
	GridLines->SetupAttachment(SceneRoot);
	GridLines->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	GridLines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GridLines->SetGenerateOverlapEvents(false);
	GridLines->SetCanEverAffectNavigation(false);
	GridLines->SetCastShadow(false);

	if (CubeMesh.Succeeded())
	{
		Floor->SetStaticMesh(CubeMesh.Object);
		GridLines->SetStaticMesh(CubeMesh.Object);
	}

	Title = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Title"));
	Title->SetupAttachment(SceneRoot);
	Title->SetRelativeRotation(FRotator(0.f, 90.f, 0.f)); // vertical, facing the viewer on the north side
	Title->SetHorizontalAlignment(EHTA_Center);
	Title->SetVerticalAlignment(EVRTA_TextCenter);
	Title->SetWorldSize(HueBoardPrivate::TitleWorldSize);
	Title->SetTextRenderColor(FColor::White);
	Title->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Title->SetGenerateOverlapEvents(false);
	Title->SetText(FText::FromString(BoardName));
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void AHueBoard::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	GridSize.X = FMath::Clamp(GridSize.X, 1, HueBoardPrivate::MaxGridDimension);
	GridSize.Y = FMath::Clamp(GridSize.Y, 1, HueBoardPrivate::MaxGridDimension);
	CellSize = FMath::Max(CellSize, 1.f);

	RebuildGridVisuals();
	RefreshTitle();
}

void AHueBoard::BeginPlay()
{
	Super::BeginPlay();

	if (!GetActorRotation().IsNearlyZero(0.01f) || !GetActorScale3D().Equals(FVector::OneVector, 0.001f))
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: boards are expected to be unrotated and unscaled (rig DOF locks and block ports are world-axis aligned)."),
			*GetName());
	}

	// Runtime colour instances. Both are UPROPERTYs (and referenced by the components' override materials).
	if (UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial())
	{
		FloorMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		Floor->SetMaterial(0, FloorMID);

		LineMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		LineMID->SetVectorParameterValue(TEXT("Color"), HueBoardPrivate::LineColor);
		GridLines->SetMaterial(0, LineMID);
	}

	RebuildGridVisuals();
	RefreshFloorColor();
	RefreshTitle();

	RegisterExistingBlocksInBounds();
	if (IsValid(Rig))
	{
		LinkReceiversToRig();
		Rig->ShowObjectiveMarkers(Objectives);
	}
	ResetObjectives();

	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		Manager->RegisterBoard(this);
	}
}

void AHueBoard::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Unbind from every block we listen to; the blocks may outlive this board during teardown.
	for (const TPair<FIntPoint, TObjectPtr<APulseBlock>>& Pair : Occupancy)
	{
		if (APulseBlock* Block = Pair.Value.Get())
		{
			Block->OnEndPlay.RemoveDynamic(this, &AHueBoard::HandleBlockEndPlay);
		}
	}
	Occupancy.Empty();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}

	Super::EndPlay(EndPlayReason);
}

// ---- Grid ---------------------------------------------------------------------------------------------------

void AHueBoard::SetGridSize(FIntPoint InSize)
{
	const FIntPoint NewSize(
		FMath::Clamp(InSize.X, 1, HueBoardPrivate::MaxGridDimension),
		FMath::Clamp(InSize.Y, 1, HueBoardPrivate::MaxGridDimension));
	if (NewSize == GridSize)
	{
		return;
	}
	GridSize = NewSize;

	// Blocks that no longer fit are released from the grid (they stay in the world, unregistered).
	TArray<APulseBlock*, TInlineAllocator<16>> Outside;
	for (const TPair<FIntPoint, TObjectPtr<APulseBlock>>& Pair : Occupancy)
	{
		if (!IsInside(Pair.Key) && IsValid(Pair.Value))
		{
			Outside.Add(Pair.Value.Get());
		}
	}
	for (APulseBlock* Block : Outside)
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: %s at (%d,%d) is outside the new %dx%d grid and was unregistered."),
			*GetName(), *Block->GetName(), Block->GetCell().X, Block->GetCell().Y, GridSize.X, GridSize.Y);
		UnregisterBlock(Block);
	}
	PruneInvalidOccupancy();

	RebuildGridVisuals();
}

bool AHueBoard::IsInside(FIntPoint Cell) const
{
	return Cell.X >= 0 && Cell.Y >= 0 && Cell.X < GridSize.X && Cell.Y < GridSize.Y;
}

FTransform AHueBoard::GetGridTransform() const
{
	return FTransform(GetActorQuat(), GetActorLocation());
}

FVector AHueBoard::CellToWorld(FIntPoint Cell) const
{
	// No clamping: the pulse visuals also interpolate towards cells just outside the board.
	const FVector Local((Cell.X + 0.5f) * CellSize, (Cell.Y + 0.5f) * CellSize, HueBoardPrivate::FloorThickness);
	return GetGridTransform().TransformPosition(Local);
}

bool AHueBoard::WorldToCell(const FVector& WorldLocation, FIntPoint& OutCell) const
{
	const FVector Local = GetGridTransform().InverseTransformPosition(WorldLocation);
	const double Size = FMath::Max(static_cast<double>(CellSize), 1.0);
	OutCell = FIntPoint(FMath::FloorToInt32(Local.X / Size), FMath::FloorToInt32(Local.Y / Size));
	return IsInside(OutCell);
}

FBox AHueBoard::GetWorldBounds(float Margin) const
{
	const FTransform GridTM = GetGridTransform();
	const double Width = GridSize.X * CellSize;
	const double Depth = GridSize.Y * CellSize;

	FBox Box(ForceInit);
	for (const double Z : { 0.0, static_cast<double>(HueBoardPrivate::FloorThickness) })
	{
		Box += GridTM.TransformPosition(FVector(0.0, 0.0, Z));
		Box += GridTM.TransformPosition(FVector(Width, 0.0, Z));
		Box += GridTM.TransformPosition(FVector(0.0, Depth, Z));
		Box += GridTM.TransformPosition(FVector(Width, Depth, Z));
	}
	const double Grow = FMath::Max(static_cast<double>(Margin), 0.0);
	return Box.ExpandBy(FVector(Grow, Grow, Grow));
}

// ---- Occupancy ----------------------------------------------------------------------------------------------

APulseBlock* AHueBoard::GetBlockAt(FIntPoint Cell) const
{
	const TObjectPtr<APulseBlock>* Found = Occupancy.Find(Cell);
	if (!Found)
	{
		return nullptr;
	}
	// A block destroyed behind our back is pending kill until the GC nulls the entry: treat it as empty.
	APulseBlock* Block = Found->Get();
	return IsValid(Block) ? Block : nullptr;
}

bool AHueBoard::IsCellFree(FIntPoint Cell) const
{
	return IsInside(Cell) && GetBlockAt(Cell) == nullptr;
}

bool AHueBoard::CanPlaceAt(FIntPoint Cell, FString* OutReason) const
{
	auto Fail = [OutReason](const TCHAR* Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};

	if (!IsInside(Cell))
	{
		return Fail(TEXT("Outside the construction area"));
	}
	if (!IsCellFree(Cell))
	{
		return Fail(TEXT("Cell occupied"));
	}
	// No manager (tests, bare levels) => editing is always allowed.
	const AExhibitManager* Manager = AExhibitManager::Get(this);
	if (Manager && !Manager->IsEditingAllowed())
	{
		return Fail(TEXT("Reset the run (R) before editing"));
	}

	if (OutReason)
	{
		OutReason->Reset();
	}
	return true;
}

APulseBlock* AHueBoard::SpawnBlock(TSubclassOf<APulseBlock> BlockClass, FIntPoint Cell, EHueDir Facing, bool bRemovable)
{
	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown)
	{
		return nullptr;
	}
	if (!BlockClass || BlockClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UE_LOG(LogHuebox, Error, TEXT("%s: SpawnBlock needs a concrete APulseBlock class (got %s)."),
			*GetName(), *GetNameSafe(BlockClass.Get()));
		return nullptr;
	}
	// Geometry only; the editing-state check belongs to CanPlaceAt (the builder and tests spawn during runs).
	if (!IsInside(Cell) || !IsCellFree(Cell))
	{
		UE_LOG(LogHuebox, Verbose, TEXT("%s: cannot spawn %s at (%d,%d): outside or occupied."),
			*GetName(), *BlockClass->GetName(), Cell.X, Cell.Y);
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.Owner = this; // the board owns its blocks; blocks only keep a weak back pointer
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	APulseBlock* Block = World->SpawnActor<APulseBlock>(BlockClass.Get(), CellToWorld(Cell), FRotator::ZeroRotator, Params);
	if (!IsValid(Block))
	{
		UE_LOG(LogHuebox, Error, TEXT("%s: failed to spawn %s at (%d,%d)."), *GetName(), *BlockClass->GetName(), Cell.X, Cell.Y);
		return nullptr;
	}

	Block->SetFacing(Facing);
	Block->SetRemovable(bRemovable);
	if (!RegisterBlock(Block, Cell))
	{
		Block->Destroy();
		return nullptr;
	}
	return Block;
}

bool AHueBoard::RegisterBlock(APulseBlock* Block, FIntPoint Cell)
{
	if (!IsValid(Block))
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: RegisterBlock called with an invalid block."), *GetName());
		return false;
	}
	if (!IsInside(Cell))
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: cannot register %s at (%d,%d): outside the %dx%d grid."),
			*GetName(), *Block->GetName(), Cell.X, Cell.Y, GridSize.X, GridSize.Y);
		return false;
	}

	APulseBlock* Existing = GetBlockAt(Cell);
	if (Existing && Existing != Block)
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: cannot register %s at (%d,%d): cell occupied by %s."),
			*GetName(), *Block->GetName(), Cell.X, Cell.Y, *Existing->GetName());
		return false;
	}

	// A block lives on exactly one board and one cell: release it from wherever it was registered before.
	if (Existing != Block)
	{
		AHueBoard* PreviousBoard = Block->GetBoard();
		if (IsValid(PreviousBoard) && PreviousBoard != this)
		{
			PreviousBoard->UnregisterBlock(Block);
		}
		UnregisterBlock(Block); // no-op unless the block moves between cells of this board
	}
	PruneInvalidOccupancy();

	Occupancy.Add(Cell, Block);
	Block->OnEndPlay.AddUniqueDynamic(this, &AHueBoard::HandleBlockEndPlay);

	Block->SetActorLocation(CellToWorld(Cell), false, nullptr, ETeleportType::TeleportPhysics);
	Block->InitializeOnBoard(this, Cell);
	return true;
}

void AHueBoard::UnregisterBlock(APulseBlock* Block)
{
	if (!Block)
	{
		return;
	}

	FIntPoint RegisteredCell;
	if (FindRegisteredCell(Block, RegisteredCell))
	{
		Occupancy.Remove(RegisteredCell);
	}
	// Harmless when not bound; the block's memory is valid here even if it is already pending kill.
	Block->OnEndPlay.RemoveDynamic(this, &AHueBoard::HandleBlockEndPlay);
}

bool AHueBoard::RemoveBlock(APulseBlock* Block)
{
	if (!IsValid(Block))
	{
		return false;
	}

	FIntPoint RegisteredCell;
	const bool bRegisteredHere = FindRegisteredCell(Block, RegisteredCell);
	if (!bRegisteredHere && Block->GetBoard() != this)
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: RemoveBlock(%s) ignored - the block belongs to another board."), *GetName(), *Block->GetName());
		return false;
	}

	// Unregister first (unbinds HandleBlockEndPlay) so the destruction below does not call back into us.
	UnregisterBlock(Block);
	Block->Destroy();
	return true;
}

void AHueBoard::RemoveAllRemovableBlocks()
{
	TArray<APulseBlock*, TInlineAllocator<64>> Blocks;
	GetBlocksSorted(Blocks);
	for (APulseBlock* Block : Blocks)
	{
		if (IsValid(Block) && Block->IsRemovable())
		{
			RemoveBlock(Block);
		}
	}
}

void AHueBoard::ForEachBlock(TFunctionRef<void(APulseBlock&)> Func) const
{
	// Iterate a snapshot: Func may remove or destroy blocks.
	TArray<APulseBlock*, TInlineAllocator<64>> Blocks;
	GetBlocksSorted(Blocks);
	for (APulseBlock* Block : Blocks)
	{
		if (IsValid(Block))
		{
			Func(*Block);
		}
	}
}

void AHueBoard::GetSources(TArray<AHueSourceBlock*>& OutSources) const
{
	OutSources.Reset();
	ForEachBlock([&OutSources](APulseBlock& Block)
	{
		if (AHueSourceBlock* Source = Cast<AHueSourceBlock>(&Block))
		{
			OutSources.Add(Source);
		}
	});
}

AHueReceiverBlock* AHueBoard::GetReceiver() const
{
	TArray<APulseBlock*, TInlineAllocator<64>> Blocks;
	GetBlocksSorted(Blocks);
	for (APulseBlock* Block : Blocks)
	{
		if (AHueReceiverBlock* Receiver = Cast<AHueReceiverBlock>(Block))
		{
			return Receiver;
		}
	}
	return nullptr;
}

int32 AHueBoard::GetHeldSignalCount() const
{
	int32 Held = 0;
	for (const TPair<FIntPoint, TObjectPtr<APulseBlock>>& Pair : Occupancy)
	{
		if (IsValid(Pair.Value))
		{
			Held += Pair.Value->GetHeldSignalCount();
		}
	}
	return Held;
}

void AHueBoard::ResetAllBlocks()
{
	ForEachBlock([](APulseBlock& Block)
	{
		Block.ResetBlock();
	});
}

void AHueBoard::RegisterExistingBlocksInBounds()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float FloorZ = static_cast<float>(CellToWorld(FIntPoint::ZeroValue).Z);
	for (TActorIterator<APulseBlock> It(World); It; ++It)
	{
		APulseBlock* Block = *It;
		// Blocks already owned by a board (spawned by one, or adopted earlier) are left alone.
		if (!IsValid(Block) || Block->GetBoard() != nullptr)
		{
			continue;
		}

		const FVector Location = Block->GetActorLocation();
		FIntPoint Cell;
		if (!WorldToCell(Location, Cell) || FMath::Abs(static_cast<float>(Location.Z) - FloorZ) > HueBoardPrivate::AdoptMaxHeight)
		{
			continue;
		}
		if (!RegisterBlock(Block, Cell))
		{
			UE_LOG(LogHuebox, Warning, TEXT("%s: level-placed %s could not be registered at (%d,%d)."),
				*GetName(), *Block->GetName(), Cell.X, Cell.Y);
		}
	}
}

void AHueBoard::HandleBlockEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason)
{
	if (APulseBlock* Block = Cast<APulseBlock>(Actor))
	{
		UnregisterBlock(Block);
	}
}

void AHueBoard::GetBlocksSorted(TArray<APulseBlock*, TInlineAllocator<64>>& OutBlocks) const
{
	struct FEntry
	{
		FIntPoint Cell;
		APulseBlock* Block;
	};

	// Local, call-scoped snapshot: the blocks stay referenced by Occupancy (UPROPERTY) meanwhile.
	TArray<FEntry, TInlineAllocator<64>> Entries;
	for (const TPair<FIntPoint, TObjectPtr<APulseBlock>>& Pair : Occupancy)
	{
		if (IsValid(Pair.Value))
		{
			Entries.Add({ Pair.Key, Pair.Value.Get() });
		}
	}
	Entries.Sort([](const FEntry& A, const FEntry& B) { return HueBoardPrivate::CellLess(A.Cell, B.Cell); });

	OutBlocks.Reset();
	for (const FEntry& Entry : Entries)
	{
		OutBlocks.Add(Entry.Block);
	}
}

bool AHueBoard::FindRegisteredCell(const APulseBlock* Block, FIntPoint& OutCell) const
{
	if (!Block)
	{
		return false;
	}
	// Fast path: the block's own cell; fall back to a value search (stale or moved entries).
	if (const TObjectPtr<APulseBlock>* Found = Occupancy.Find(Block->GetCell()); Found && Found->Get() == Block)
	{
		OutCell = Block->GetCell();
		return true;
	}
	for (const TPair<FIntPoint, TObjectPtr<APulseBlock>>& Pair : Occupancy)
	{
		if (Pair.Value.Get() == Block)
		{
			OutCell = Pair.Key;
			return true;
		}
	}
	return false;
}

void AHueBoard::PruneInvalidOccupancy()
{
	for (auto It = Occupancy.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Value()))
		{
			It.RemoveCurrent();
		}
	}
}

// ---- Mechanism & objectives ---------------------------------------------------------------------------------

void AHueBoard::SetRig(AReactiveRig* InRig)
{
	Rig = InRig;
	if (IsValid(Rig))
	{
		LinkReceiversToRig();
		Rig->ShowObjectiveMarkers(Objectives);
	}
}

void AHueBoard::LinkReceiversToRig()
{
	if (!IsValid(Rig))
	{
		return;
	}
	AReactiveRig* BoardRig = Rig.Get();
	ForEachBlock([BoardRig](APulseBlock& Block)
	{
		AHueReceiverBlock* Receiver = Cast<AHueReceiverBlock>(&Block);
		if (Receiver && !IsValid(Receiver->GetRig()))
		{
			Receiver->SetRig(BoardRig);
		}
	});
}

void AHueBoard::SetObjectives(const TArray<FHueObjective>& InObjectives)
{
	Objectives = InObjectives;
	ResetObjectives();
	if (IsValid(Rig))
	{
		Rig->ShowObjectiveMarkers(Objectives);
	}
}

void AHueBoard::ResetObjectives()
{
	CurrentObjective = 0;
	HoldTimer = 0.f;
}

void AHueBoard::TickObjectives(float DeltaSeconds)
{
	if (Objectives.Num() == 0 || AreObjectivesComplete())
	{
		return;
	}

	FHueRigSample Sample;
	if (IsValid(Rig))
	{
		Sample = Rig->MakeSample();
	}
	// The receiver is the authority for "what colour does the mechanism hold".
	if (const AHueReceiverBlock* Receiver = GetReceiver())
	{
		Sample.ReceiverColor = Receiver->GetCurrentColor();
	}

	const int32 Before = CurrentObjective;
	EvaluateObjectives(Sample, DeltaSeconds);

	for (int32 Index = Before; Index < CurrentObjective && Objectives.IsValidIndex(Index); ++Index)
	{
		const FString Message = FString::Printf(TEXT("Objective %d/%d done: %s"), Index + 1, Objectives.Num(), *Objectives[Index].Description);
		if (AExhibitManager* Manager = AExhibitManager::Get(this))
		{
			Manager->PushFeedback(EHueFeedbackKind::Success, Message, this);
		}
		else
		{
			UE_LOG(LogHuebox, Log, TEXT("%s: %s"), *BoardName, *Message);
		}
	}
}

bool AHueBoard::EvaluateObjectives(const FHueRigSample& Sample, float DeltaSeconds)
{
	if (Objectives.Num() == 0)
	{
		return false;
	}
	if (CurrentObjective >= Objectives.Num())
	{
		return true;
	}

	// Ordered objectives: only the current one is evaluated, so at most one completes per call.
	const FHueObjective& Objective = Objectives[CurrentObjective];
	const float Dt = FMath::Max(DeltaSeconds, 0.f);
	bool bDone = false;

	switch (Objective.Type)
	{
	case EHueObjectiveType::ReceiverColor:
		bDone = Sample.ReceiverColor == Objective.Color;
		break;

	case EHueObjectiveType::LeaveStart:
	case EHueObjectiveType::PassFarMarker:
		bDone = Sample.TrackFraction >= Objective.TrackFraction;
		break;

	case EHueObjectiveType::ReturnBelow:
		bDone = Sample.TrackFraction <= Objective.TrackFraction;
		break;

	case EHueObjectiveType::StopInZone:
	{
		const bool bInZone = Sample.TrackFraction >= Objective.ZoneMin && Sample.TrackFraction <= Objective.ZoneMax;
		const bool bSlowEnough = FMath::Abs(Sample.SpeedFraction) <= Objective.MaxSpeedFraction;
		if (bInZone && bSlowEnough)
		{
			HoldTimer += Dt;
			// Small tolerance: 120 steps of 1/120 s must count as one full second.
			bDone = HoldTimer + KINDA_SMALL_NUMBER >= Objective.HoldSeconds;
		}
		else
		{
			HoldTimer = 0.f; // leaving the zone or moving too fast restarts the hold
		}
		break;
	}

	default:
		break;
	}

	if (bDone)
	{
		++CurrentObjective;
		HoldTimer = 0.f;
	}
	return AreObjectivesComplete();
}

// ---- Presentation -------------------------------------------------------------------------------------------

void AHueBoard::SetTexts(const FString& InName, const FString& InBrief)
{
	BoardName = InName;
	Brief = InBrief;
	RefreshTitle();
}

void AHueBoard::SetHighlighted(bool bInHighlighted)
{
	bHighlighted = bInHighlighted;
	RefreshFloorColor();
	RefreshTitle();
}

void AHueBoard::SetCompleted(bool bInCompleted)
{
	bCompleted = bInCompleted;
	RefreshTitle();
}

void AHueBoard::RebuildGridVisuals()
{
	using namespace HueBoardPrivate;

	const int32 Columns = FMath::Max(1, GridSize.X);
	const int32 Rows = FMath::Max(1, GridSize.Y);
	const float Cell = FMath::Max(CellSize, 1.f);
	const float Width = Columns * Cell;
	const float Depth = Rows * Cell;

	if (Floor)
	{
		Floor->SetRelativeLocation(FVector(Width * 0.5f, Depth * 0.5f, FloorThickness * 0.5f));
		Floor->SetRelativeScale3D(FVector(Width / 100.f, Depth / 100.f, FloorThickness / 100.f));
	}

	if (GridLines)
	{
		// One instance per line (Rows + 1 east-west lines, Columns + 1 north-south lines) - a single draw call.
		GridLines->ClearInstances();
		TArray<FTransform> Lines;
		Lines.Reserve(Rows + Columns + 2);
		const float LineZ = FloorThickness + LineHeight * 0.5f;
		for (int32 Row = 0; Row <= Rows; ++Row)
		{
			Lines.Emplace(FRotator::ZeroRotator, FVector(Width * 0.5f, Row * Cell, LineZ),
				FVector((Width + LineWidth) / 100.f, LineWidth / 100.f, LineHeight / 100.f));
		}
		for (int32 Column = 0; Column <= Columns; ++Column)
		{
			Lines.Emplace(FRotator::ZeroRotator, FVector(Column * Cell, Depth * 0.5f, LineZ),
				FVector(LineWidth / 100.f, (Depth + LineWidth) / 100.f, LineHeight / 100.f));
		}
		GridLines->AddInstances(Lines, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
	}

	if (Title)
	{
		// Centred over the far (south, y = 0) edge as seen by the viewer standing north of the board.
		Title->SetRelativeLocation(FVector(Width * 0.5f, 0.f, TitleHeight));
	}
}

void AHueBoard::RefreshTitle()
{
	if (!Title)
	{
		return;
	}

	const FString Text = bCompleted ? FString::Printf(TEXT("%s  (done)"), *BoardName) : BoardName;
	Title->SetText(FText::FromString(Text));

	FLinearColor Color = FLinearColor::White;
	if (bCompleted)
	{
		Color = HueBoardPrivate::TitleCompletedColor;
	}
	else if (bHighlighted)
	{
		Color = HueColor::ToLinear(EHueColor::A); // amber
	}
	Title->SetTextRenderColor(Color.ToFColor(true));
}

void AHueBoard::RefreshFloorColor()
{
	if (FloorMID)
	{
		FloorMID->SetVectorParameterValue(TEXT("Color"),
			bHighlighted ? HueBoardPrivate::FloorHighlightColor : HueBoardPrivate::FloorColor);
	}
}
