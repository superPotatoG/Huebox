#include "Player/HueModulePickup.h"

#include "Blocks/PulseBlock.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Player/HueboxCharacter.h"
#include "UObject/ConstructorHelpers.h"

namespace HuePickupTuning
{
	static constexpr float MeshScale = 0.35f;
	static constexpr float LabelHeight = 45.f;
	static constexpr float LabelWorldSize = 18.f;
	static constexpr float BobAmplitude = 4.f;
	static constexpr float BobSpeed = 2.2f;
	static constexpr float SpinDegreesPerSecond = 45.f;
}

AHueModulePickup::AHueModulePickup()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(SceneRoot);
	Mesh->SetRelativeScale3D(FVector(HuePickupTuning::MeshScale));

	// Constructor-only asset lookup (ConstructorHelpers is illegal outside constructors).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CubeFinder.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		Mesh->SetMaterial(0, MaterialFinder.Object);
	}

	// Only the interaction trace (Visibility) may hit a pickup: the player walks through it, physics ignores it.
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Mesh->SetCollisionObjectType(ECC_WorldDynamic);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(SceneRoot);
	Label->SetRelativeLocation(FVector(0.f, 0.f, HuePickupTuning::LabelHeight));
	// Vertical text facing the viewer standing north of the boards (see orientation convention).
	Label->SetRelativeRotation(FRotator(0.f, 90.f, 0.f));
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetVerticalAlignment(EVRTA_TextCenter);
	Label->SetWorldSize(HuePickupTuning::LabelWorldSize);
	Label->SetTextRenderColor(FColor::White);
	Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AHueModulePickup::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Editor preview: label only. The MID is created in BeginPlay so no runtime material is saved into the level.
	RefreshVisuals();
}

void AHueModulePickup::BeginPlay()
{
	Super::BeginPlay();

	if (Mesh && !MID)
	{
		UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial();
		if (!BaseMaterial)
		{
			BaseMaterial = Mesh->GetMaterial(0);
		}
		if (BaseMaterial)
		{
			// Outer = this, and the Transient UPROPERTY keeps it referenced for the GC.
			MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			Mesh->SetMaterial(0, MID);
		}
	}

	// Desynchronise the bob of neighbouring pickups.
	BobTime = FMath::FRandRange(0.f, 2.f * UE_PI);
	RefreshVisuals();
}

void AHueModulePickup::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Purely cosmetic: only the mesh's relative transform moves, the actor (and its trace target) stays put.
	BobTime += DeltaSeconds;
	if (Mesh)
	{
		const float Z = FMath::Sin(BobTime * HuePickupTuning::BobSpeed) * HuePickupTuning::BobAmplitude;
		const float Yaw = FMath::Fmod(BobTime * HuePickupTuning::SpinDegreesPerSecond, 360.f);
		Mesh->SetRelativeLocationAndRotation(FVector(0.f, 0.f, Z), FRotator(0.f, Yaw, 0.f));
	}
}

void AHueModulePickup::SetContents(EHueModuleType InType, int32 InCount)
{
	Type = InType;
	Count = FMath::Max(0, InCount);
	RefreshVisuals();
}

void AHueModulePickup::RefreshVisuals()
{
	if (MID)
	{
		MID->SetVectorParameterValue(TEXT("Color"), HueModule::BodyColor(Type));
	}

	if (Label)
	{
		Label->SetText(FText::FromString(FString::Printf(TEXT("%s x%d"), HueModule::ShortName(Type), Count)));
	}
}

FString AHueModulePickup::GetInteractPrompt_Implementation(AHueboxCharacter* Interactor) const
{
	return FString::Printf(TEXT("LMB  Pick up %d x %s"), Count, HueModule::DisplayName(Type));
}

bool AHueModulePickup::Interact_Implementation(AHueboxCharacter* Interactor)
{
	// The character owns the inventory rules (stacking, "inventory full", destroying an emptied pickup).
	return IsValid(Interactor) && Interactor->TryCollectPickup(this) > 0;
}
