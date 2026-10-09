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
#include "Player/HueboxCharacter.h"
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

	constexpr float LabelWorldSize = 22.f;
	constexpr float FlashSeconds = 0.6f;
	/** Distance of a port marker from the cell centre (cm). */
	constexpr float PortMarkerOffset = 42.f;
	/** How far the red flash "X" moves from the label centre towards the blocked face (cm). */
	constexpr float FlashCrossOffset = 22.f;

	const FLinearColor FlashBodyColor(0.85f, 0.08f, 0.06f);
	const FLinearColor InputMarkerColor(0.1f, 0.1f, 0.12f);
	const FLinearColor OutputMarkerColor(0.92f, 0.92f, 0.88f);
	const FLinearColor BothMarkerColor(0.7f, 0.7f, 0.72f);

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
	// Lies on the top face (faces +Z) and reads along +X for the viewer standing north of the board.
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
		Body = BaseColor + (FLinearColor::White - BaseColor) * 0.35f;
		Body.A = 1.f;
	}
	SetBodyColor(Body);
	ApplyLabel();
}

// ---- IHueInteractable -------------------------------------------------------------------------------------

FString APulseBlock::GetInteractPrompt_Implementation(AHueboxCharacter* Interactor) const
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

bool APulseBlock::Interact_Implementation(AHueboxCharacter* Interactor)
{
	// The character owns the rules (fixed units, editing state, inventory capacity).
	return IsValid(Interactor) && Interactor->TryCollectBlock(this);
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

	// Runtime-only presentation: never create components in editor / preview worlds.
	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !SceneRoot)
	{
		return;
	}

	TArray<FHuePort> Ports;
	GetPorts(Ports);

	const float MarkerZ = BodyScale.Z * 50.f;
	for (const FHuePort& Port : Ports)
	{
		// Markers hang off the unrotated SceneRoot, so the board-local face direction is used directly.
		const FVector Dir = HueDir::ToVector(Port.Face);

		UStaticMesh* Mesh = nullptr;
		FVector Scale = FVector(0.16f);
		FRotator Rotation = FRotator::ZeroRotator;
		FLinearColor Color = HueBlockPrivate::InputMarkerColor;
		switch (Port.Kind)
		{
		case EHuePortKind::Input:
			Mesh = GetCubeMesh();
			break;
		case EHuePortKind::Output:
			Mesh = GetConeMesh();
			Scale = FVector(0.2f);
			Rotation = FRotationMatrix::MakeFromZ(Dir).Rotator(); // cone tip points out of the face
			Color = HueBlockPrivate::OutputMarkerColor;
			break;
		case EHuePortKind::Both:
			Mesh = GetCylinderMesh();
			Scale = FVector(0.16f, 0.16f, 0.12f);
			Rotation = FRotationMatrix::MakeFromZ(Dir).Rotator(); // short stub along the face normal
			Color = HueBlockPrivate::BothMarkerColor;
			break;
		}
		if (!Mesh)
		{
			continue;
		}

		// Owned by this actor; the UPROPERTY array keeps it reachable until it is destroyed on the next rebuild.
		UStaticMeshComponent* Marker = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
		Marker->SetStaticMesh(Mesh);
		Marker->SetupAttachment(SceneRoot);
		Marker->SetRelativeLocation(Dir * HueBlockPrivate::PortMarkerOffset + FVector(0.f, 0.f, MarkerZ));
		Marker->SetRelativeRotation(Rotation);
		Marker->SetRelativeScale3D(Scale);
		// Traceable (aiming at a marker targets this block) but never blocks movement or physics.
		Marker->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Marker->SetCollisionResponseToAllChannels(ECR_Ignore);
		Marker->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Marker->SetGenerateOverlapEvents(false);
		Marker->SetCastShadow(false);
		Marker->RegisterComponent();
		MakeColorMID(Marker, Color);
		PortMarkers.Add(Marker);
	}
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

	const FVector RestLocation(0.f, 0.f, BodyScale.Z * 100.f + 1.f);
	if (bFlashing)
	{
		Label->SetText(FText::FromString(TEXT("X")));
		Label->SetTextRenderColor(FColor(255, 40, 30));
		Label->SetRelativeLocation(RestLocation + HueDir::ToVector(FlashFace) * HueBlockPrivate::FlashCrossOffset);
	}
	else
	{
		Label->SetText(FText::FromString(LabelString));
		Label->SetTextRenderColor(LabelColor.ToFColor(true));
		Label->SetRelativeLocation(RestLocation);
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
		? FLinearColor(0.02f, 0.02f, 0.025f)
		: FLinearColor(0.95f, 0.95f, 0.95f);
}
