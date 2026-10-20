#include "Blocks/PulseBlock.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#include "Game/ExhibitManager.h"
#include "Grid/HueBoard.h"
#include "Player/HueBuildToolComponent.h"
#include "Simulation/PulseSubsystem.h"

namespace HueBlockPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const ConePath = TEXT("/Engine/BasicShapes/Cone.Cone");
	const TCHAR* const SpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	const TCHAR* const CylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	const TCHAR* const MaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	/** Vector parameter of BasicShapeMaterial. */
	FName ColorParam()
	{
		static const FName Name(TEXT("Color"));
		return Name;
	}

	/** Nominal label size (cm); long labels shrink to fit between the port markers (FitLabelToTile). */
	constexpr float LabelWorldSize = 26.f;
	/** Label height above the top face (cm). */
	constexpr float LabelLift = 1.f;
	constexpr float FlashSeconds = 0.6f;
	/** How far the flash "X" moves from the label centre towards the blocked face (cm). */
	constexpr float FlashCrossOffset = 22.f;

	/** Port markers sit at the tile edge but never further than this fraction of the cell size from the centre. */
	constexpr float PortMarkerCellFraction = 0.40f;
	/** Cell size assumed while a block is not registered on a board yet (cm). */
	constexpr float DefaultCellSize = 100.f;
	/** Free band kept between the label and the port markers, measured from the marker centre (cm): half the
	 *  longest marker (8 cm) plus a visible gap, so text like "SRC B" never runs into an output arrow. */
	constexpr float LabelMarkerClearance = 14.f;

	/** Highlight: blend of the body towards a light amber (visible on white and on coloured tiles). */
	constexpr float HighlightBlend = 0.35f;
	const FLinearColor HighlightTint(1.0f, 0.82f, 0.4f);
	const FLinearColor FlashBodyColor(0.85f, 0.08f, 0.06f);
	/** The flash cross is light so it reads on the red tile. */
	const FColor FlashCrossColor(250, 250, 245);
	/** Dark ink for labels and port markers. */
	const FLinearColor TileInkColor(0.08f, 0.08f, 0.08f);
	const FLinearColor MarkerColor(0.06f, 0.06f, 0.07f);

	/** Marker sizes in units of the 100 cm engine shapes (all 2 cm thick except the 8 cm output cone). */
	const FVector InputMarkerScale(0.14f, 0.14f, 0.02f);	// square
	const FVector OutputMarkerScale(0.08f, 0.16f, 0.16f);	// cone, see RebuildPortMarkers: X = thickness, Y = width, Z = length
	const FVector BothMarkerScale(0.16f, 0.06f, 0.02f);		// bar, X = length along the face normal

	/**
	 * Weak cache + LoadObject fallback for shared engine assets used outside constructors. A weak pointer never
	 * roots the asset (nothing static survives UObject shutdown); the components using it hold the strong refs.
	 */
	template <typename T>
	T* LoadCached(TWeakObjectPtr<T>& Cache, const TCHAR* Path)
	{
		if (T* Existing = Cache.Get())
		{
			return Existing;
		}
		T* Loaded = LoadObject<T>(nullptr, Path);
		if (!Loaded)
		{
			UE_LOG(LogHuebox, Warning, TEXT("PulseBlock: could not load %s"), Path);
		}
		Cache = Loaded;
		return Loaded;
	}
}

APulseBlock::APulseBlock()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SceneRoot->SetMobility(EComponentMobility::Movable);
	SetRootComponent(SceneRoot);

	Pivot = CreateDefaultSubobject<USceneComponent>(TEXT("Pivot"));
	Pivot->SetupAttachment(SceneRoot);

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(Pivot);
	BodyMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	BodyMesh->SetGenerateOverlapEvents(false);

	// Epic's constructor idiom: the finders resolve once, while the CDO is built on the game thread, so later
	// instances (including actors constructed by the async loader) never load from a constructor. The finder is
	// an FGCObject whose destructor is safe after the exit purge (unlike a static TStrongObjectPtr).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(HueBlockPrivate::CubePath);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(HueBlockPrivate::MaterialPath);
	if (CubeFinder.Succeeded())
	{
		BodyMesh->SetStaticMesh(CubeFinder.Object);
	}
	if (MaterialFinder.Succeeded())
	{
		BodyMesh->SetMaterial(0, MaterialFinder.Object);
	}

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(SceneRoot);
	// Lies on the top face (faces +Z), reads along +X with the letter tops towards -Y: upright in the top-down
	// build view (screen right = +X, screen up = -Y) and for a sandbox viewer standing north of the board.
	Label->SetRelativeRotation(FRotationMatrix::MakeFromXY(FVector::UpVector, -FVector::ForwardVector).Rotator());
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetVerticalAlignment(EVRTA_TextCenter);
	Label->SetWorldSize(HueBlockPrivate::LabelWorldSize);
	Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Label->SetGenerateOverlapEvents(false);
	Label->SetCastShadow(false);

	ApplyBodyLayout();
}

// ---- Simulation interface ---------------------------------------------------------------------------------

EPulseResponse APulseBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	ReportBlocked(Pulse, Sim,
		FString::Printf(TEXT("no input on the %s face"), HueDir::ToString(HueDir::Opposite(Pulse.Direction))));
	return EPulseResponse::Consumed;
}

void APulseBlock::ResetBlock()
{
	LastInputText.Reset();
	LastOutputText.Reset();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlashTimer);
	}
	bFlashing = false;

	RefreshVisuals();
}

// ---- Description ------------------------------------------------------------------------------------------

FString APulseBlock::GetRuleText() const
{
	return TEXT("Blocks every pulse.");
}

FString APulseBlock::GetStateText() const
{
	if (LastInputText.IsEmpty() && LastOutputText.IsEmpty())
	{
		return TEXT("No pulse yet this run");
	}
	if (LastOutputText.IsEmpty())
	{
		return LastInputText;
	}
	if (LastInputText.IsEmpty())
	{
		return LastOutputText;
	}
	return LastInputText + TEXT(" | ") + LastOutputText;
}

FString APulseBlock::GetDisplayName() const
{
	if (!BlockDisplayName.IsEmpty())
	{
		return BlockDisplayName;
	}
	if (ModuleType != EHueModuleType::None)
	{
		return HueModule::DisplayName(ModuleType);
	}
	return TEXT("Block");
}

// ---- Grid placement ---------------------------------------------------------------------------------------

void APulseBlock::InitializeOnBoard(AHueBoard* Board, FIntPoint InCell)
{
	OwningBoard = Board;
	Cell = InCell;

	// Spawn order is BeginPlay -> SetFacing -> register, so the markers were laid out for the default cell size.
	// Redo them (and the label fit) only if this board's cell size actually moves the markers.
	if (HasActorBegunPlay() && !FMath::IsNearlyEqual(GetPortMarkerOffset(), PortMarkerOffsetInUse))
	{
		RebuildPortMarkers();
		ApplyLabel();
	}
}

void APulseBlock::SetFacing(EHueDir NewFacing)
{
	Facing = NewFacing;
	ApplyFacingRotation();
	RebuildPortMarkers();
}

void APulseBlock::RotateBy(int32 Steps)
{
	if (Steps % 4 == 0)
	{
		return;
	}
	SetFacing(HueDir::Rotate(Facing, Steps));
}

// ---- Presentation -----------------------------------------------------------------------------------------

void APulseBlock::SetHighlighted(bool bInHighlighted)
{
	if (bHighlighted == bInHighlighted)
	{
		return;
	}
	bHighlighted = bInHighlighted;
	RefreshVisuals();
}

void APulseBlock::FlashBlocked(EHueDir Face)
{
	FlashFace = Face;
	bFlashing = true;
	RefreshVisuals();

	// Method-pointer timer: the delegate is bound to this UObject, so it can never fire into a dead actor, and
	// EndPlay clears it explicitly. Re-flashing restarts the same handle.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(FlashTimer, this, &APulseBlock::EndFlash, HueBlockPrivate::FlashSeconds, false);
	}
}

void APulseBlock::EndFlash()
{
	bFlashing = false;
	RefreshVisuals();
}

void APulseBlock::RefreshVisuals()
{
	FLinearColor Body = BaseColor;
	if (bFlashing)
	{
		Body = HueBlockPrivate::FlashBodyColor;
	}
	else if (bHighlighted)
	{
		// Slight amber tint: lightening alone would be invisible on the white module tiles.
		Body = BaseColor + (HueBlockPrivate::HighlightTint - BaseColor) * HueBlockPrivate::HighlightBlend;
		Body.A = 1.f;
	}
	SetBodyColor(Body);
	ApplyLabel();
}

// ---- IHueInteractable -------------------------------------------------------------------------------------

FString APulseBlock::GetInteractPrompt_Implementation(AActor* Interactor) const
{
	if (!bRemovable)
	{
		return FString::Printf(TEXT("%s (fixed)"), *GetDisplayName());
	}
	// Match what LMB will actually do: collecting is refused outside Build.
	const AExhibitManager* Manager = AExhibitManager::Get(this);
	if (Manager && !Manager->IsEditingAllowed())
	{
		return FString::Printf(TEXT("%s  (R: reset to edit)"), *GetDisplayName());
	}
	return FString::Printf(TEXT("LMB  Collect %s"), *GetDisplayName());
}

bool APulseBlock::Interact_Implementation(AActor* Interactor)
{
	// The pawn's build tool owns the rules (fixed units, editing state, inventory capacity), whatever the pawn type.
	UHueBuildToolComponent* Tool = IsValid(Interactor) ? Interactor->FindComponentByClass<UHueBuildToolComponent>() : nullptr;
	return Tool && Tool->TryCollectBlock(this);
}

// ---- Shared placeholder assets ----------------------------------------------------------------------------

UStaticMesh* APulseBlock::GetCubeMesh()
{
	static TWeakObjectPtr<UStaticMesh> Cache;
	return HueBlockPrivate::LoadCached(Cache, HueBlockPrivate::CubePath);
}

UStaticMesh* APulseBlock::GetConeMesh()
{
	static TWeakObjectPtr<UStaticMesh> Cache;
	return HueBlockPrivate::LoadCached(Cache, HueBlockPrivate::ConePath);
}

UStaticMesh* APulseBlock::GetSphereMesh()
{
	static TWeakObjectPtr<UStaticMesh> Cache;
	return HueBlockPrivate::LoadCached(Cache, HueBlockPrivate::SpherePath);
}

UStaticMesh* APulseBlock::GetCylinderMesh()
{
	static TWeakObjectPtr<UStaticMesh> Cache;
	return HueBlockPrivate::LoadCached(Cache, HueBlockPrivate::CylinderPath);
}

UMaterialInterface* APulseBlock::GetBasicMaterial()
{
	static TWeakObjectPtr<UMaterialInterface> Cache;
	return HueBlockPrivate::LoadCached(Cache, HueBlockPrivate::MaterialPath);
}

// ---- Actor lifecycle --------------------------------------------------------------------------------------

void APulseBlock::BeginPlay()
{
	Super::BeginPlay();

	ApplyFacingRotation();

	if (IsValid(BodyMesh))
	{
		if (!BodyMesh->GetStaticMesh())
		{
			BodyMesh->SetStaticMesh(GetCubeMesh());
		}
		if (!BodyMID)
		{
			BodyMID = MakeColorMID(BodyMesh, BaseColor);
		}
	}

	RefreshVisuals();
	RebuildPortMarkers();
}

void APulseBlock::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	FlashTimer.Invalidate();
	bFlashing = false;

	// Destroyed by anything other than AHueBoard::RemoveBlock (which unregisters first): keep the board's
	// occupancy consistent so it never maps a cell to a dead block.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		if (AHueBoard* Board = OwningBoard.Get())
		{
			if (Board->GetBlockAt(Cell) == this)
			{
				Board->UnregisterBlock(this);
			}
		}
	}
	OwningBoard.Reset();

	Super::EndPlay(EndPlayReason);
}

void APulseBlock::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	ApplyBodyLayout();
	ApplyFacingRotation();
	// Labels update in the editor too; body MIDs are only created in game worlds (see SetBodyColor).
	RefreshVisuals();
}

void APulseBlock::ApplyFacingRotation()
{
	if (Pivot)
	{
		Pivot->SetRelativeRotation(FRotator(0.f, HueDir::ToYaw(Facing), 0.f));
	}
}

void APulseBlock::ApplyBodyLayout()
{
	if (BodyMesh)
	{
		BodyMesh->SetRelativeScale3D(BodyScale);
		BodyMesh->SetRelativeLocation(FVector(0.f, 0.f, BodyScale.Z * 50.f));
	}
	ApplyLabel();
}

void APulseBlock::RebuildPortMarkers()
{
	for (UStaticMeshComponent* Marker : PortMarkers)
	{
		if (IsValid(Marker))
		{
			Marker->DestroyComponent();
		}
	}
	PortMarkers.Reset();
	PortMarkerOffsetInUse = GetPortMarkerOffset();

	// Runtime-only presentation: never create components in editor / preview worlds.
	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !SceneRoot)
	{
		return;
	}

	TArray<FHuePort> Ports;
	GetPorts(Ports);

	const float TopZ = static_cast<float>(BodyScale.Z) * 100.f;
	for (const FHuePort& Port : Ports)
	{
		// Markers hang off the unrotated SceneRoot, so the board-local face direction is used directly.
		const FVector Dir = HueDir::ToVector(Port.Face);

		// Flat dark shapes that read from straight above.
		UStaticMesh* Mesh = nullptr;
		FVector Scale = HueBlockPrivate::InputMarkerScale;
		FRotator Rotation = FRotator::ZeroRotator;
		switch (Port.Kind)
		{
		case EHuePortKind::Input:
			Mesh = GetCubeMesh(); // small square
			break;
		case EHuePortKind::Output:
			// Cone lying on the face with its tip (local +Z) pointing out of the face and local X up, so the X
			// scale is the thickness: seen from above it is a triangle pointing outward.
			Mesh = GetConeMesh();
			Scale = HueBlockPrivate::OutputMarkerScale;
			Rotation = FRotationMatrix::MakeFromZX(Dir, FVector::UpVector).Rotator();
			break;
		case EHuePortKind::Both:
			// Bar along the face normal; the two bars of a turn outline its corner path.
			Mesh = GetCubeMesh();
			Scale = HueBlockPrivate::BothMarkerScale;
			Rotation = FRotator(0.f, HueDir::ToYaw(Port.Face), 0.f);
			break;
		}
		if (!Mesh)
		{
			continue;
		}

		// Place by the mesh bounds so the result does not depend on where the basic shape's pivot is: bounds
		// centre at the port edge (inside the cell footprint), bottom resting on the top face.
		const FBox Placed = Mesh->GetBoundingBox().TransformBy(FTransform(Rotation, FVector::ZeroVector, Scale));
		const FVector PlacedCenter = Placed.GetCenter();
		const FVector Location(
			Dir.X * PortMarkerOffsetInUse - PlacedCenter.X,
			Dir.Y * PortMarkerOffsetInUse - PlacedCenter.Y,
			TopZ - Placed.Min.Z);

		// Owned by this actor; the UPROPERTY array keeps it reachable until it is destroyed on the next rebuild.
		UStaticMeshComponent* Marker = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Marker->SetStaticMesh(Mesh);
		Marker->SetupAttachment(SceneRoot);
		Marker->SetRelativeLocation(Location);
		Marker->SetRelativeRotation(Rotation);
		Marker->SetRelativeScale3D(Scale);
		// Traceable (aiming at a marker targets this block) but never blocks movement or physics.
		Marker->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Marker->SetCollisionResponseToAllChannels(ECR_Ignore);
		Marker->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Marker->SetGenerateOverlapEvents(false);
		Marker->SetCastShadow(false);
		Marker->RegisterComponent();
		MakeColorMID(Marker, HueBlockPrivate::MarkerColor);
		PortMarkers.Add(Marker);
	}
}

float APulseBlock::GetPortMarkerOffset() const
{
	const AHueBoard* Board = OwningBoard.Get();
	const float CellSize = Board ? Board->GetCellSize() : HueBlockPrivate::DefaultCellSize;
	const float TileHalf = static_cast<float>(FMath::Min(BodyScale.X, BodyScale.Y)) * 50.f;
	// 100 cm cells: 40 cm for every tile size, i.e. on the edge of the 80 cm module tiles.
	return FMath::Min(TileHalf, CellSize * HueBlockPrivate::PortMarkerCellFraction);
}

// ---- Helpers ----------------------------------------------------------------------------------------------

UMaterialInstanceDynamic* APulseBlock::MakeColorMID(UPrimitiveComponent* Component, const FLinearColor& Color)
{
	if (!IsValid(Component))
	{
		return nullptr;
	}
	UMaterialInterface* BaseMaterial = GetBasicMaterial();
	if (!BaseMaterial)
	{
		BaseMaterial = Component->GetMaterial(0);
	}
	if (!BaseMaterial)
	{
		return nullptr;
	}

	// Outer = the component: the MID lives exactly as long as what it colours. SetMaterial stores it in the
	// component's OverrideMaterials UPROPERTY, which is the strong reference the GC follows.
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, Component);
	if (MID)
	{
		MID->SetVectorParameterValue(HueBlockPrivate::ColorParam(), Color);
		Component->SetMaterial(0, MID);
	}
	return MID;
}

void APulseBlock::SetBodyColor(const FLinearColor& Color)
{
	if (!IsValid(BodyMesh))
	{
		return;
	}
	if (BodyMID)
	{
		BodyMID->SetVectorParameterValue(HueBlockPrivate::ColorParam(), Color);
		return;
	}

	// Lazily create the MID in game worlds only (editor-placed blocks keep the plain base material, so no
	// transient instance ends up serialized into the level).
	const UWorld* World = GetWorld();
	if (World && World->IsGameWorld())
	{
		BodyMID = MakeColorMID(BodyMesh, Color);
	}
}

void APulseBlock::SetLabelText(const FString& Text, const FLinearColor& Color)
{
	LabelString = Text;
	LabelColor = Color;
	ApplyLabel();
}

void APulseBlock::ApplyLabel()
{
	if (!Label)
	{
		return;
	}

	const FVector RestLocation(0.f, 0.f, BodyScale.Z * 100.f + HueBlockPrivate::LabelLift);
	if (bFlashing)
	{
		// Light cross on the red tile, shifted towards the blocked face.
		Label->SetText(FText::FromString(TEXT("X")));
		Label->SetTextRenderColor(HueBlockPrivate::FlashCrossColor);
		Label->SetRelativeLocation(RestLocation + HueDir::ToVector(FlashFace) * HueBlockPrivate::FlashCrossOffset);
	}
	else
	{
		Label->SetText(FText::FromString(LabelString));
		// TextRenderColor reaches the text material as a raw vertex colour, so convert linearly: the 0.08 ink
		// then renders dark (an sRGB conversion would store 79/255, a mid gray).
		Label->SetTextRenderColor(LabelColor.ToFColor(/*bSRGB*/ false));
		Label->SetRelativeLocation(RestLocation);
	}
	FitLabelToTile();
}

void APulseBlock::FitLabelToTile()
{
	if (!Label)
	{
		return;
	}

	// Measure at the nominal size, then shrink to the free square between the edge markers (text bounds scale
	// linearly with WorldSize). Local text bounds: Y = width, Z = height. Empty text measures 0.
	Label->SetWorldSize(HueBlockPrivate::LabelWorldSize);
	const FVector TextSize = Label->GetTextLocalSize();
	const float Largest = static_cast<float>(FMath::Max(TextSize.Y, TextSize.Z));

	const float TileWidth = static_cast<float>(FMath::Min(BodyScale.X, BodyScale.Y)) * 100.f;
	const float MarkerFree = 2.f * (GetPortMarkerOffset() - HueBlockPrivate::LabelMarkerClearance);
	const float MaxExtent = FMath::Max(10.f, FMath::Min(MarkerFree, TileWidth - 4.f));
	if (Largest > MaxExtent)
	{
		Label->SetWorldSize(HueBlockPrivate::LabelWorldSize * MaxExtent / Largest);
	}
}

void APulseBlock::NoteInput(const FPulseData& Pulse, const UPulseSubsystem& Sim)
{
	LastInputText = FString::Printf(TEXT("in: %s from %s @%.2fs"),
		HueColor::Label(Pulse.Color),
		HueDir::ToString(HueDir::Opposite(Pulse.Direction)),
		HueTime::MsToSeconds(Sim.GetSimTimeMs()));
}

void APulseBlock::NoteOutput(const FString& Text)
{
	LastOutputText = Text;
}

void APulseBlock::ReportBlocked(const FPulseData& Pulse, UPulseSubsystem& Sim, const FString& Why)
{
	const EHueDir EntryFace = HueDir::Opposite(Pulse.Direction);
	LastInputText = FString::Printf(TEXT("blocked: %s from %s @%.2fs"),
		HueColor::Label(Pulse.Color), HueDir::ToString(EntryFace), HueTime::MsToSeconds(Sim.GetSimTimeMs()));

	Sim.Report(EHueFeedbackKind::Warning, FString::Printf(TEXT("%s: %s"), *DescribeBlock(), *Why), Cell);
	FlashBlocked(EntryFace);
}

FString APulseBlock::DescribeBlock() const
{
	return FString::Printf(TEXT("%s (%d,%d)"), *GetDisplayName(), Cell.X, Cell.Y);
}

int64 APulseBlock::GetHudSimTimeMs() const
{
	const UPulseSubsystem* Sim = UPulseSubsystem::Get(this);
	return Sim ? Sim->GetSimTimeMs() : 0;
}

FString APulseBlock::FormatOutput(EHueColor Color, EHueDir Dir, int64 TimeMs)
{
	return FString::Printf(TEXT("out: %s to %s @%.2fs"), HueColor::Label(Color), HueDir::ToString(Dir), HueTime::MsToSeconds(TimeMs));
}

FLinearColor APulseBlock::ContrastTextColor(const FLinearColor& Background)
{
	return Background.GetLuminance() > 0.25f
		? TileLabelColor()
		: FLinearColor(0.95f, 0.95f, 0.95f);
}

FLinearColor APulseBlock::TileLabelColor()
{
	return HueBlockPrivate::TileInkColor;
}
