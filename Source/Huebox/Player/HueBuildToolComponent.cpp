#include "Player/HueBuildToolComponent.h"

#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Game/ExhibitManager.h"
#include "Game/HueboxGameMode.h"
#include "GameFramework/Actor.h"
#include "Grid/HueBoard.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Player/HueInteractable.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"

namespace HueBuildToolTuning
{
	/** Ghost = a flat tile like the 2D module props (cube 0.8 x 0.8 x 0.06), centre 4 cm above the board top. */
	static const FVector PreviewTileScale(0.8f, 0.8f, 0.06f);
	static constexpr float PreviewTileHeight = 4.f;

	/**
	 * Facing arrow: the cone squashed into a flat wedge lying on the tile edge, pointing along the preview facing.
	 * With MakeFromZX(Facing, Up) the cone axis (local Z) is the facing and local X is world up (the thin axis).
	 */
	static const FVector PreviewArrowScale(0.06f, 0.22f, 0.22f);
	static constexpr float PreviewArrowOffset = 32.f;
	static constexpr float PreviewArrowHeight = 6.f;

	static const FLinearColor PreviewValid(0.2f, 0.9f, 0.3f);
	static const FLinearColor PreviewInvalid(0.9f, 0.2f, 0.2f);
	/** Dark like the icons on the white module tiles; readable on both ghost colours. */
	static const FLinearColor PreviewArrowColor(0.08f, 0.08f, 0.08f);
}

namespace HueBuildToolText
{
	static const TCHAR* SelectModule = TEXT("Select a module (1-6 / wheel)");
	static const TCHAR* ResetBeforeEditing = TEXT("Reset the run (R) before editing");
	static const TCHAR* InventoryFull = TEXT("Inventory full");
	static const TCHAR* FixedUnits = TEXT("Fixed units cannot be removed");
	static const TCHAR* CannotPlace = TEXT("Cannot place here");
}

namespace HueBuildToolPrivate
{
	/** Vector parameter of BasicShapeMaterial. */
	FName ColorParam()
	{
		static const FName Name(TEXT("Color"));
		return Name;
	}

	/**
	 * Runtime preview part on the tool's owner. Outer = the owner (an actor owns and destroys its components); the
	 * tool's UPROPERTY is the strong reference. Absolute transforms keep it on the targeted cell instead of
	 * following the pawn / camera it is attached to.
	 */
	UStaticMeshComponent* CreatePreviewPart(AActor& Owner, UStaticMesh* Mesh, UMaterialInterface* Material, const FVector& Scale)
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(&Owner, NAME_None, RF_Transient);
		Part->SetUsingAbsoluteLocation(true);
		Part->SetUsingAbsoluteRotation(true);
		Part->SetUsingAbsoluteScale(true);
		Part->SetRelativeScale3D(Scale);
		Part->SetMobility(EComponentMobility::Movable);
		if (Mesh)
		{
			Part->SetStaticMesh(Mesh);
		}
		if (Material)
		{
			Part->SetMaterial(0, Material);
		}
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetCastShadow(false);
		Part->SetVisibility(false);
		if (USceneComponent* Root = Owner.GetRootComponent())
		{
			Part->SetupAttachment(Root);
		}
		Part->RegisterComponent();
		return Part;
	}
}

UHueBuildToolComponent::UHueBuildToolComponent()
{
	// Highlight + ghost follow the target every frame (a few component updates, no queries).
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void UHueBuildToolComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// The owner pushes the target in its own Tick; ticking after it keeps the ghost on this frame's target.
	AddTickPrerequisiteActor(Owner);

	UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial();
	if (!PreviewTile)
	{
		PreviewTile = HueBuildToolPrivate::CreatePreviewPart(*Owner, APulseBlock::GetCubeMesh(), BaseMaterial, HueBuildToolTuning::PreviewTileScale);
	}
	if (!PreviewArrow)
	{
		PreviewArrow = HueBuildToolPrivate::CreatePreviewPart(*Owner, APulseBlock::GetConeMesh(), BaseMaterial, HueBuildToolTuning::PreviewArrowScale);
	}

	if (BaseMaterial)
	{
		if (PreviewTile && !PreviewMID)
		{
			// Recoloured green / red; the Transient UPROPERTY keeps it referenced.
			PreviewMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			PreviewTile->SetMaterial(0, PreviewMID);
		}
		if (PreviewArrow)
		{
			// Constant colour. Outer = the arrow component; its OverrideMaterials UPROPERTY is the GC reference.
			if (UMaterialInstanceDynamic* ArrowMID = UMaterialInstanceDynamic::Create(BaseMaterial, PreviewArrow.Get()))
			{
				ArrowMID->SetVectorParameterValue(HueBuildToolPrivate::ColorParam(), HueBuildToolTuning::PreviewArrowColor);
				PreviewArrow->SetMaterial(0, ArrowMID);
			}
		}
	}
	PreviewColorState = -1;
}

void UHueBuildToolComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Only touch the block when it outlives us; on level teardown every actor is going away anyway.
	if (EndPlayReason == EEndPlayReason::Destroyed || EndPlayReason == EEndPlayReason::RemovedFromWorld)
	{
		ClearHighlight();
	}
	HighlightedBlock.Reset();
	Target = FHueInteractionTarget();
	Inventory.Reset();

	// The preview parts belong to the owner, which may outlive this tool: destroy them explicitly and drop the
	// strong references (DestroyComponent also unregisters and detaches them).
	for (UStaticMeshComponent* Part : { PreviewTile.Get(), PreviewArrow.Get() })
	{
		if (IsValid(Part))
		{
			Part->DestroyComponent();
		}
	}
	PreviewTile = nullptr;
	PreviewArrow = nullptr;
	PreviewMID = nullptr;
	PreviewColorState = -1;

	Super::EndPlay(EndPlayReason);
}

void UHueBuildToolComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// The selection or the targeted block can change between owner updates (collected, destroyed, run started).
	UpdateHighlight();
	UpdatePreview();
}

// ---- Inventory source ---------------------------------------------------------------------------------------

void UHueBuildToolComponent::SetInventory(UHueInventoryComponent* InInventory)
{
	Inventory = InInventory;
	// Cell reasons depend on the selected module of the active inventory.
	RefreshTarget();
}

UHueInventoryComponent* UHueBuildToolComponent::GetInventory() const
{
	return Inventory.Get();
}

// ---- Targeting ----------------------------------------------------------------------------------------------

void UHueBuildToolComponent::SetTargetFromHit(const FHitResult& Hit)
{
	if (!Hit.bBlockingHit)
	{
		ClearTarget();
		return;
	}

	FHueInteractionTarget NewTarget;
	AActor* HitActor = Hit.GetActor();

	if (APulseBlock* Block = Cast<APulseBlock>(HitActor))
	{
		NewTarget.Kind = EHueTargetKind::Block;
		NewTarget.Block = Block;
		NewTarget.Board = Block->GetBoard();
		NewTarget.Cell = Block->GetCell();
	}
	else if (AHueModulePickup* Pickup = Cast<AHueModulePickup>(HitActor))
	{
		NewTarget.Kind = EHueTargetKind::Pickup;
		NewTarget.Pickup = Pickup;
	}
	else if (AHueBoard* Board = Cast<AHueBoard>(HitActor))
	{
		// Aiming at the floor beside a block's body still targets that block (BuildCellTarget).
		FIntPoint Cell;
		if (Board->WorldToCell(Hit.ImpactPoint, Cell))
		{
			BuildCellTarget(Board, Cell, NewTarget);
		}
	}

	ApplyTarget(MoveTemp(NewTarget));
}

void UHueBuildToolComponent::SetTargetCell(AHueBoard* Board, FIntPoint Cell)
{
	FHueInteractionTarget NewTarget;
	BuildCellTarget(Board, Cell, NewTarget);
	ApplyTarget(MoveTemp(NewTarget));
}

void UHueBuildToolComponent::ClearTarget()
{
	ApplyTarget(FHueInteractionTarget());
}

void UHueBuildToolComponent::RefreshTarget()
{
	FHueInteractionTarget NewTarget;

	switch (Target.Kind)
	{
	case EHueTargetKind::Block:
	case EHueTargetKind::Cell:
		if (AHueBoard* Board = Target.Board.Get())
		{
			// Same cell, current occupancy: a collected block becomes an empty cell, a placed one a block.
			BuildCellTarget(Board, Target.Cell, NewTarget);
		}
		else if (Target.Kind == EHueTargetKind::Block && Target.Block.IsValid())
		{
			// A block that is not registered on any board (level-placed stray): keep it while it exists.
			NewTarget = Target;
		}
		break;

	case EHueTargetKind::Pickup:
		if (Target.Pickup.IsValid())
		{
			NewTarget = Target;
		}
		break;

	case EHueTargetKind::None:
	default:
		break;
	}

	ApplyTarget(MoveTemp(NewTarget));
	UpdatePreview();
}

void UHueBuildToolComponent::BuildCellTarget(AHueBoard* Board, FIntPoint Cell, FHueInteractionTarget& Out) const
{
	Out = FHueInteractionTarget();
	if (!IsValid(Board) || !Board->IsInside(Cell))
	{
		return;
	}

	Out.Board = Board;
	Out.Cell = Cell;

	if (APulseBlock* Occupant = Board->GetBlockAt(Cell))
	{
		Out.Kind = EHueTargetKind::Block;
		Out.Block = Occupant;
		return;
	}

	Out.Kind = EHueTargetKind::Cell;

	const UHueInventoryComponent* ActiveInventory = Inventory.Get();
	const EHueModuleType Selected = ActiveInventory ? ActiveInventory->GetSelectedType() : EHueModuleType::None;
	if (Selected == EHueModuleType::None)
	{
		Out.Reason = HueBuildToolText::SelectModule;
	}
	else if (!Board->CanPlaceAt(Cell, &Out.Reason))
	{
		if (Out.Reason.IsEmpty())
		{
			Out.Reason = HueBuildToolText::CannotPlace;
		}
	}
	else if (CellBlockedQuery.IsBound() && CellBlockedQuery.Execute(Board, Cell))
	{
		Out.Reason = CellBlockedReason;
	}
	else
	{
		Out.bCanPlace = true;
	}
}

void UHueBuildToolComponent::ApplyTarget(FHueInteractionTarget&& NewTarget)
{
	Target = MoveTemp(NewTarget);
	UpdateHighlight();
}

void UHueBuildToolComponent::UpdateHighlight()
{
	// Highlight follows the block target. Both sides are weak: either block may be gone by now.
	APulseBlock* NewHighlight = (Target.Kind == EHueTargetKind::Block) ? Target.Block.Get() : nullptr;
	APulseBlock* OldHighlight = HighlightedBlock.Get();
	if (NewHighlight == OldHighlight)
	{
		return;
	}

	if (OldHighlight)
	{
		OldHighlight->SetHighlighted(false);
	}
	if (NewHighlight)
	{
		NewHighlight->SetHighlighted(true);
	}
	HighlightedBlock = NewHighlight;
}

void UHueBuildToolComponent::ClearHighlight()
{
	if (APulseBlock* Block = HighlightedBlock.Get())
	{
		Block->SetHighlighted(false);
	}
	HighlightedBlock.Reset();
}

void UHueBuildToolComponent::UpdatePreview()
{
	if (!PreviewTile || !PreviewArrow)
	{
		return;
	}

	const UHueInventoryComponent* ActiveInventory = Inventory.Get();
	const EHueModuleType Selected = ActiveInventory ? ActiveInventory->GetSelectedType() : EHueModuleType::None;
	const AHueBoard* Board = Target.Board.Get();
	const bool bShow = bShowPreview && Target.Kind == EHueTargetKind::Cell && Board != nullptr && Selected != EHueModuleType::None;

	PreviewTile->SetVisibility(bShow);
	PreviewArrow->SetVisibility(bShow);
	if (!bShow)
	{
		return;
	}

	// Boards are unrotated, so board-local directions are world directions.
	const FVector CellCenter = Board->CellToWorld(Target.Cell);
	const FVector Facing = HueDir::ToVector(PreviewFacing);

	PreviewTile->SetWorldLocationAndRotation(CellCenter + FVector(0.f, 0.f, HueBuildToolTuning::PreviewTileHeight),
		FRotator(0.f, HueDir::ToYaw(PreviewFacing), 0.f));
	// Cone points along its local +Z; local X up keeps the squashed wedge lying flat (readable from above).
	PreviewArrow->SetWorldLocationAndRotation(
		CellCenter + Facing * HueBuildToolTuning::PreviewArrowOffset + FVector(0.f, 0.f, HueBuildToolTuning::PreviewArrowHeight),
		FRotationMatrix::MakeFromZX(Facing, FVector::UpVector).Rotator());

	const int8 NewColorState = Target.bCanPlace ? 1 : 0;
	if (PreviewMID && NewColorState != PreviewColorState)
	{
		PreviewMID->SetVectorParameterValue(HueBuildToolPrivate::ColorParam(),
			Target.bCanPlace ? HueBuildToolTuning::PreviewValid : HueBuildToolTuning::PreviewInvalid);
		PreviewColorState = NewColorState;
	}
}

// ---- Actions ------------------------------------------------------------------------------------------------

void UHueBuildToolComponent::PrimaryAction()
{
	// Pickups and blocks decide through IHueInteractable (Blueprint overrides work); both call back into this
	// tool via the interactor's FindComponentByClass.
	AActor* Interactor = GetOwner();

	switch (Target.Kind)
	{
	case EHueTargetKind::Pickup:
		if (AHueModulePickup* Pickup = Target.Pickup.Get())
		{
			IHueInteractable::Execute_Interact(Pickup, Interactor);
		}
		break;

	case EHueTargetKind::Block:
		if (APulseBlock* Block = Target.Block.Get())
		{
			IHueInteractable::Execute_Interact(Block, Interactor);
		}
		break;

	case EHueTargetKind::Cell:
		TryPlaceSelected();
		break;

	case EHueTargetKind::None:
	default:
		break;
	}

	// The world under the target just changed (block removed / placed, pickup gone): refresh immediately.
	RefreshTarget();
}

void UHueBuildToolComponent::SecondaryAction()
{
	if (Target.Kind != EHueTargetKind::Block)
	{
		return;
	}
	if (APulseBlock* Block = Target.Block.Get())
	{
		TryCollectBlock(Block);
	}
	RefreshTarget();
}

void UHueBuildToolComponent::RotateTargetOrPreview(int32 Steps)
{
	if (Target.Kind == EHueTargetKind::Block)
	{
		if (APulseBlock* Block = Target.Block.Get())
		{
			if (!Block->IsRemovable() || !Block->CanRotate())
			{
				ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("%s is fixed and cannot be rotated"), *Block->GetDisplayName()));
				return;
			}
			if (!IsEditingAllowed())
			{
				ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::ResetBeforeEditing);
				return;
			}
			Block->RotateBy(Steps);
			return;
		}
	}

	// Nothing rotatable targeted: rotate the placement preview instead.
	PreviewFacing = HueDir::Rotate(PreviewFacing, Steps);
}

void UHueBuildToolComponent::AdjustTargetDelay(int32 Steps)
{
	AHueDelayBlock* Delay = (Target.Kind == EHueTargetKind::Block) ? Cast<AHueDelayBlock>(Target.Block.Get()) : nullptr;
	if (!Delay)
	{
		ShowMessage(EHueFeedbackKind::Info, TEXT("Aim at a delay block to change its delay (Z / X)"));
		return;
	}
	if (!IsEditingAllowed())
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::ResetBeforeEditing);
		return;
	}

	const float Before = Delay->GetDelaySeconds();
	Delay->AdjustDelay(Steps);
	const float After = Delay->GetDelaySeconds();

	if (FMath::IsNearlyEqual(Before, After))
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Delay %.2f s (%s)"), After,
			After <= AHueDelayBlock::MinDelay + UE_KINDA_SMALL_NUMBER ? TEXT("minimum") : TEXT("maximum")));
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Delay %.2f s"), After));
	}
}

void UHueBuildToolComponent::CycleSelection(int32 Delta)
{
	if (UHueInventoryComponent* ActiveInventory = Inventory.Get())
	{
		ActiveInventory->CycleSelection(Delta);
	}
}

void UHueBuildToolComponent::SelectSlot(int32 Index)
{
	if (UHueInventoryComponent* ActiveInventory = Inventory.Get())
	{
		ActiveInventory->SelectSlot(Index);
	}
}

bool UHueBuildToolComponent::TryPlaceSelected()
{
	AHueBoard* Board = Target.Board.Get();
	UHueInventoryComponent* ActiveInventory = Inventory.Get();
	if (Target.Kind != EHueTargetKind::Cell || !Board || !ActiveInventory)
	{
		return false;
	}

	const EHueModuleType Type = ActiveInventory->GetSelectedType();
	if (Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Info, HueBuildToolText::SelectModule);
		return false;
	}

	// Re-validate at click time: the run state or occupancy may have changed since the last update.
	FString Reason;
	if (!Board->CanPlaceAt(Target.Cell, &Reason))
	{
		ShowMessage(EHueFeedbackKind::Warning, Reason.IsEmpty() ? FString(HueBuildToolText::CannotPlace) : Reason);
		return false;
	}
	if (CellBlockedQuery.IsBound() && CellBlockedQuery.Execute(Board, Target.Cell))
	{
		ShowMessage(EHueFeedbackKind::Warning, CellBlockedReason);
		return false;
	}

	const TSubclassOf<APulseBlock> BlockClass = AHueboxGameMode::ResolveModuleClass(this, Type);
	if (!BlockClass)
	{
		ShowMessage(EHueFeedbackKind::Error, FString::Printf(TEXT("No block class for %s"), HueModule::DisplayName(Type)));
		return false;
	}

	const int32 SlotIndex = ActiveInventory->GetSelectedIndex();
	APulseBlock* Block = Board->SpawnBlock(BlockClass, Target.Cell, PreviewFacing, /*bRemovable*/ true);
	if (!Block)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::CannotPlace);
		return false;
	}

	// Transaction: the inventory pays for the block that now exists; if it cannot, undo the placement.
	if (!ActiveInventory->RemoveFromSlot(SlotIndex, 1))
	{
		Board->RemoveBlock(Block);
		return false;
	}
	SelectNextFilledSlotIfEmpty();
	return true;
}

bool UHueBuildToolComponent::TryCollectBlock(APulseBlock* Block)
{
	if (!IsValid(Block))
	{
		return false;
	}

	const EHueModuleType Type = Block->GetModuleType();
	if (!Block->IsRemovable() || Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::FixedUnits);
		return false;
	}
	if (!IsEditingAllowed())
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::ResetBeforeEditing);
		return false;
	}
	UHueInventoryComponent* ActiveInventory = Inventory.Get();
	if (!ActiveInventory || !ActiveInventory->CanAdd(Type, 1))
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::InventoryFull);
		return false;
	}

	const FString Name = Block->GetDisplayName();

	// The block is about to be destroyed: forget it instead of unhighlighting it.
	if (HighlightedBlock.Get() == Block)
	{
		HighlightedBlock.Reset();
	}

	// The board owns the block: let it unregister and destroy it. A stray block without a board is destroyed directly.
	bool bRemoved = false;
	if (AHueBoard* Board = Block->GetBoard())
	{
		bRemoved = Board->RemoveBlock(Block);
	}
	else
	{
		bRemoved = Block->Destroy();
	}

	if (!bRemoved)
	{
		ShowMessage(EHueFeedbackKind::Warning, FString::Printf(TEXT("%s could not be removed"), *Name));
		return false;
	}

	// CanAdd above guarantees this fits.
	const int32 Added = ActiveInventory->AddItem(Type, 1);
	ensureMsgf(Added == 1, TEXT("Inventory refused a module after CanAdd succeeded"));

	SelectIfNothingSelected(Type);
	ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Collected %s"), *Name));
	return true;
}

int32 UHueBuildToolComponent::TryCollectPickup(AHueModulePickup* Pickup)
{
	UHueInventoryComponent* ActiveInventory = Inventory.Get();
	if (!IsValid(Pickup) || !ActiveInventory)
	{
		return 0;
	}

	const EHueModuleType Type = Pickup->GetModuleType();
	const int32 Count = Pickup->GetCount();

	if (Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Warning, TEXT("This pickup holds no module"));
		return 0;
	}
	if (Count <= 0)
	{
		Pickup->Destroy();
		return 0;
	}

	const int32 Added = ActiveInventory->AddItem(Type, Count);
	if (Added <= 0)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueBuildToolText::InventoryFull);
		return 0;
	}

	const int32 Left = Count - Added;
	if (Left > 0)
	{
		Pickup->SetContents(Type, Left);
	}
	else
	{
		Pickup->Destroy();
	}

	SelectIfNothingSelected(Type);

	if (Left > 0)
	{
		ShowMessage(EHueFeedbackKind::Warning, FString::Printf(TEXT("Collected %d x %s - inventory full, %d left"), Added, HueModule::DisplayName(Type), Left));
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Collected %d x %s"), Added, HueModule::DisplayName(Type)));
	}
	return Added;
}

// ---- Helpers ------------------------------------------------------------------------------------------------

void UHueBuildToolComponent::ShowMessage(EHueFeedbackKind Kind, const FString& Message) const
{
	if (Message.IsEmpty())
	{
		return;
	}

	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		// The manager logs and keeps the line for the HUD feedback list.
		Manager->PushFeedback(Kind, Message);
	}
	else
	{
		UE_LOG(LogHuebox, Log, TEXT("[Player] %s"), *Message);
	}
}

bool UHueBuildToolComponent::IsEditingAllowed() const
{
	const AExhibitManager* Manager = AExhibitManager::Get(this);
	return Manager == nullptr || Manager->IsEditingAllowed();
}

void UHueBuildToolComponent::SelectIfNothingSelected(EHueModuleType Type)
{
	UHueInventoryComponent* ActiveInventory = Inventory.Get();
	if (!ActiveInventory || ActiveInventory->GetSelectedType() != EHueModuleType::None)
	{
		return;
	}

	const TArray<FHueInventorySlot>& Slots = ActiveInventory->GetSlots();
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (!Slots[Index].IsEmpty() && Slots[Index].Type == Type)
		{
			ActiveInventory->SelectSlot(Index);
			return;
		}
	}
}

void UHueBuildToolComponent::SelectNextFilledSlotIfEmpty()
{
	UHueInventoryComponent* ActiveInventory = Inventory.Get();
	if (!ActiveInventory || ActiveInventory->GetSelectedType() != EHueModuleType::None)
	{
		return;
	}

	const TArray<FHueInventorySlot>& Slots = ActiveInventory->GetSlots();
	const int32 NumSlots = Slots.Num();
	const int32 Start = ActiveInventory->GetSelectedIndex();
	for (int32 Offset = 1; Offset < NumSlots; ++Offset)
	{
		const int32 Index = (Start + Offset) % NumSlots;
		if (!Slots[Index].IsEmpty())
		{
			ActiveInventory->SelectSlot(Index);
			return;
		}
	}
}
