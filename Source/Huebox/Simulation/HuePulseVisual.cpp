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
	/** The engine sphere is 100 cm across; a pulse is a ~22 cm ball. */
	static constexpr float SphereScale = 0.22f;
	/** Letter floats just above the ball. */
	static constexpr float LetterHeight = 24.f;
	static constexpr float LetterWorldSize = 20.f;
	/** Vector parameter of /Engine/BasicShapes/BasicShapeMaterial (plain TCHAR*: no FName static-init order issue). */
	static constexpr const TCHAR* ColorParameterName = TEXT("Color");
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

	Sphere = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sphere"));
	Sphere->SetupAttachment(SceneRoot);
	Sphere->SetMobility(EComponentMobility::Movable);
	Sphere->SetRelativeScale3D(FVector(SphereScale));
	Sphere->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sphere->SetGenerateOverlapEvents(false);
	Sphere->SetCastShadow(false);
	Sphere->CanCharacterStepUpOn = ECB_No;

	// ConstructorHelpers are only legal inside a constructor; the statics resolve the assets once.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		Sphere->SetStaticMesh(SphereMesh.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BasicMaterial.Succeeded())
	{
		Sphere->SetMaterial(0, BasicMaterial.Object);
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
	if (MID || !Sphere)
	{
		return MID.Get();
	}

	UMaterialInterface* BaseMaterial = Sphere->GetMaterial(0);
	if (!BaseMaterial)
	{
		return nullptr;
	}

	// Outer = this actor; the UPROPERTY keeps the instance alive exactly as long as the visual.
	MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (MID)
	{
		Sphere->SetMaterial(0, MID);
		if (bHasShownColor)
		{
			MID->SetVectorParameterValue(HuePulseVisualPrivate::ColorParameterName, HueColor::ToLinear(ShownColor));
		}
	}
	return MID.Get();
}

void AHuePulseVisual::Activate(EHueColor Color, const FVector& Location)
{
	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	SetColor(Color);
	SetActorHiddenInGame(false);
}

void AHuePulseVisual::Deactivate()
{
	SetActorHiddenInGame(true);
}

void AHuePulseVisual::SetColor(EHueColor Color)
{
	if (bHasShownColor && ShownColor == Color)
	{
		return;
	}
	ShownColor = Color;
	bHasShownColor = true;

	const FLinearColor Linear = HueColor::ToLinear(Color);
	if (UMaterialInstanceDynamic* Material = EnsureMID())
	{
		Material->SetVectorParameterValue(HuePulseVisualPrivate::ColorParameterName, Linear);
	}
	if (Letter)
	{
		Letter->SetText(FText::FromString(HueColor::Label(Color)));
		Letter->SetTextRenderColor(Linear.ToFColor(true));
	}
}
