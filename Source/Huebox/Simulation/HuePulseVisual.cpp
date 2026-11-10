#include "Simulation/HuePulseVisual.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace HuePulseVisualPrivate
{
	/** The engine sphere is 100 cm across; a pulse is a ~18 cm bead of light. */
	static constexpr float SphereScale = 0.18f;
	/** Letter floats just above the bead. */
	static constexpr float LetterHeight = 24.f;
	static constexpr float LetterWorldSize = 20.f;
	/** Unlit emissive above 1 glows (bloom). */
	static constexpr float BeadGlow = 3.0f;
	/** Trail: beads this far apart along the leg (fraction of a cell), shrinking and fading. */
	static constexpr double TrailSpacing = 0.12;
	static constexpr float TrailScaleStart = 0.14f;
	static constexpr float TrailScaleEnd = 0.06f;
	static constexpr float TrailGlowStart = 1.8f;
	static constexpr float TrailGlowEnd = 0.5f;
	/** Vector parameter of both materials (plain TCHAR*: no FName static-init order issue). */
	static constexpr const TCHAR* ColorParameterName = TEXT("Color");

	void SetupBead(UStaticMeshComponent* Bead)
	{
		Bead->SetMobility(EComponentMobility::Movable);
		Bead->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Bead->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Bead->SetGenerateOverlapEvents(false);
		Bead->SetCastShadow(false);
		Bead->CanCharacterStepUpOn = ECB_No;
	}
}

AHuePulseVisual::AHuePulseVisual()
{
	using namespace HuePulseVisualPrivate;

	// Purely cosmetic and moved by the subsystem: no tick, no damage; every component below is NoCollision so
	// the player's interaction trace never hits a pulse.
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;
	SetCanBeDamaged(false);
	// Parked (hidden) until the pool activates it.
	SetHidden(true);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SceneRoot->SetMobility(EComponentMobility::Movable);
	RootComponent = SceneRoot;

	// ConstructorHelpers are only legal inside a constructor; the statics resolve the assets once.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	// Unlit glow (the project material); the lit basic material is the fallback when it is missing.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GlowMaterial(TEXT("/Game/Huebox/Materials/M_HueUnlit.M_HueUnlit"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	UMaterialInterface* Material = GlowMaterial.Succeeded() ? GlowMaterial.Object : (BasicMaterial.Succeeded() ? BasicMaterial.Object : nullptr);

	Sphere = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sphere"));
	Sphere->SetupAttachment(SceneRoot);
	Sphere->SetRelativeScale3D(FVector(SphereScale));
	SetupBead(Sphere);
	if (SphereMesh.Succeeded())
	{
		Sphere->SetStaticMesh(SphereMesh.Object);
	}
	if (Material)
	{
		Sphere->SetMaterial(0, Material);
	}

	for (int32 Index = 0; Index < NumTrailBeads; ++Index)
	{
		UStaticMeshComponent* Bead = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Trail%d"), Index));
		Bead->SetupAttachment(SceneRoot);
		// Placed in world space along the leg, independent of the actor's own location.
		Bead->SetUsingAbsoluteLocation(true);
		const float T = NumTrailBeads > 1 ? static_cast<float>(Index) / (NumTrailBeads - 1) : 0.f;
		Bead->SetRelativeScale3D(FVector(FMath::Lerp(TrailScaleStart, TrailScaleEnd, T)));
		SetupBead(Bead);
		if (SphereMesh.Succeeded())
		{
			Bead->SetStaticMesh(SphereMesh.Object);
		}
		if (Material)
		{
			Bead->SetMaterial(0, Material);
		}
		Bead->SetVisibility(false);
		Trail.Add(Bead);
	}

	Letter = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Letter"));
	Letter->SetupAttachment(SceneRoot);
	Letter->SetMobility(EComponentMobility::Movable);
	Letter->SetRelativeLocation(FVector(0.f, 0.f, LetterHeight));
	// Vertical text facing the viewer standing north of the boards (see orientation convention).
	Letter->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
	Letter->SetHorizontalAlignment(EHTA_Center);
	Letter->SetVerticalAlignment(EVRTA_TextCenter);
	Letter->SetWorldSize(LetterWorldSize);
	Letter->SetText(FText::FromString(HueColor::Label(EHueColor::B)));
	Letter->SetTextRenderColor(HueColor::ToLinear(EHueColor::B).ToFColor(true));
	Letter->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Letter->SetGenerateOverlapEvents(false);
	Letter->SetCastShadow(false);
}

void AHuePulseVisual::BeginPlay()
{
	Super::BeginPlay();
	EnsureMID();
}

UMaterialInstanceDynamic* AHuePulseVisual::EnsureMID()
{
	using namespace HuePulseVisualPrivate;
	if (MID || !Sphere)
	{
		return MID.Get();
	}

	UMaterialInterface* BaseMaterial = Sphere->GetMaterial(0);
	if (!BaseMaterial)
	{
		return nullptr;
	}

	// Outer = this actor; the UPROPERTY keeps the instances alive exactly as long as the visual.
	MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (MID)
	{
		Sphere->SetMaterial(0, MID);
	}
	TrailMIDs.Reset();
	for (UStaticMeshComponent* Bead : Trail)
	{
		UMaterialInstanceDynamic* BeadMID = Bead ? UMaterialInstanceDynamic::Create(BaseMaterial, this) : nullptr;
		if (BeadMID)
		{
			Bead->SetMaterial(0, BeadMID);
		}
		TrailMIDs.Add(BeadMID);
	}
	if (bHasShownColor)
	{
		bHasShownColor = false; // re-apply to the new instances
		SetColor(ShownColor);
	}
	return MID.Get();
}

void AHuePulseVisual::Activate(EHueColor Color, const FVector& Location)
{
	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	for (UStaticMeshComponent* Bead : Trail)
	{
		if (Bead)
		{
			Bead->SetVisibility(false);
		}
	}
	SetColor(Color);
	SetActorHiddenInGame(false);
}

void AHuePulseVisual::Deactivate()
{
	SetActorHiddenInGame(true);
}

void AHuePulseVisual::SetTravel(const FVector& From, const FVector& To, double Alpha)
{
	using namespace HuePulseVisualPrivate;
	SetActorLocation(FMath::Lerp(From, To, Alpha));
	for (int32 Index = 0; Index < Trail.Num(); ++Index)
	{
		UStaticMeshComponent* Bead = Trail[Index];
		if (!Bead)
		{
			continue;
		}
		// Only along the current leg: right after a turn the trail starts again from the block.
		const double BeadAlpha = Alpha - TrailSpacing * (Index + 1);
		if (BeadAlpha < 0.0)
		{
			Bead->SetVisibility(false);
			continue;
		}
		Bead->SetWorldLocation(FMath::Lerp(From, To, BeadAlpha));
		Bead->SetVisibility(true);
	}
}

void AHuePulseVisual::SetColor(EHueColor Color)
{
	using namespace HuePulseVisualPrivate;
	if (bHasShownColor && ShownColor == Color)
	{
		return;
	}
	ShownColor = Color;
	bHasShownColor = true;

	const FLinearColor Linear = HueColor::ToLinear(Color);
	if (UMaterialInstanceDynamic* Material = EnsureMID())
	{
		Material->SetVectorParameterValue(ColorParameterName, Linear * BeadGlow);
	}
	for (int32 Index = 0; Index < TrailMIDs.Num(); ++Index)
	{
		if (UMaterialInstanceDynamic* BeadMID = TrailMIDs[Index])
		{
			const float T = TrailMIDs.Num() > 1 ? static_cast<float>(Index) / (TrailMIDs.Num() - 1) : 0.f;
			BeadMID->SetVectorParameterValue(ColorParameterName, Linear * FMath::Lerp(TrailGlowStart, TrailGlowEnd, T));
		}
	}
	if (Letter)
	{
		Letter->SetText(FText::FromString(HueColor::Label(Color)));
		Letter->SetTextRenderColor(Linear.ToFColor(true));
	}
}
