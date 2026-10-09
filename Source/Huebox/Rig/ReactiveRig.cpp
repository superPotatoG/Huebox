#include "Rig/ReactiveRig.h"

#include "Blocks/PulseBlock.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UObject/ConstructorHelpers.h"

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HueRigPrivate
{
	// Rig-local layout (cm). Track runs along +X from the actor origin; the viewer stands on the +Y side.
	constexpr float TrackWidth = 30.f;
	constexpr float TrackHeight = 6.f;               // track top at Z 6
	constexpr float CarriageWidth = 50.f;
	constexpr float CarriageHeight = 30.f;
	constexpr float CarriageClearance = 1.f;         // gap between track top and carriage bottom
	constexpr float CarriageCenterZ = TrackHeight + CarriageHeight * 0.5f + CarriageClearance; // 22
	constexpr float CarriageMassKg = 10.f;
	constexpr float StopLength = 20.f;
	constexpr float StopWidth = 60.f;
	constexpr float StopHeight = 40.f;
	constexpr float AnchorSize = 60.f;
	constexpr float AnchorCenterX = -60.f;
	constexpr float StateLabelZ = 125.f;
	constexpr float FigureScale = 0.45f;

	// Objective markers.
	constexpr float MarkerSideOffset = 45.f;         // towards the viewer
	constexpr float PostThickness = 4.f;
	constexpr float PostDepth = 40.f;
	constexpr float PostHeight = 60.f;
	constexpr float ZonePlateZ = 7.f;
	constexpr float ZonePlateWidth = 90.f;
	constexpr float ZonePlateThickness = 2.f;

	static const FLinearColor TrackColor(0.4f, 0.42f, 0.45f);
	static const FLinearColor StopColor(0.15f, 0.15f, 0.15f);
	static const FLinearColor LeaveStartMarkerColor(0.55f, 0.55f, 0.55f);
	static const FLinearColor ZoneColor(0.2f, 0.7f, 0.3f);
	static const FColor LetterOnBodyColor(20, 20, 24);

	/** Basic-shape material instance tinted with Color. Outer = component, and the component's
	 *  OverrideMaterials (a UPROPERTY) is what keeps the instance alive. */
	static UMaterialInstanceDynamic* TintComponent(UPrimitiveComponent* Component, const FLinearColor& Color)
	{
		if (!IsValid(Component))
		{
			return nullptr;
		}
		UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial();
		if (!BaseMaterial)
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, Component);
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		Component->SetMaterial(0, MID);
		return MID;
	}

	static void SetupStaticPart(UStaticMeshComponent* Part, USceneComponent* Parent, UStaticMesh* Mesh, FName CollisionProfile)
	{
		Part->SetupAttachment(Parent);
		if (Mesh)
		{
			Part->SetStaticMesh(Mesh);
		}
		Part->SetCollisionProfileName(CollisionProfile);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
	}

	static void SetupText(UTextRenderComponent* Text, float WorldSize, const FColor& Color)
	{
		Text->SetRelativeRotation(FRotator(0.f, 90.f, 0.f)); // vertical, facing the viewer (+Y side)
		Text->SetHorizontalAlignment(EHTA_Center);
		Text->SetVerticalAlignment(EVRTA_TextCenter);
		Text->SetWorldSize(WorldSize);
		Text->SetTextRenderColor(Color);
		Text->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Text->SetGenerateOverlapEvents(false);
	}

	static const TCHAR* ResponseName(EHueColor Color)
	{
		switch (Color)
		{
		case EHueColor::A: return TEXT("repel");
		case EHueColor::B: return TEXT("attract");
		default:           return TEXT("brake");
		}
	}
}

AReactiveRig::AReactiveRig()
{
	using namespace HueRigPrivate;

	// Colour forces are added before the physics step of the same frame.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> FigureMesh(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	UStaticMesh* Cube = CubeMesh.Succeeded() ? CubeMesh.Object : nullptr;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	TrackMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TrackMesh"));
	SetupStaticPart(TrackMesh, SceneRoot, Cube, UCollisionProfile::NoCollision_ProfileName);

	NearStop = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NearStop"));
	SetupStaticPart(NearStop, SceneRoot, Cube, UCollisionProfile::BlockAll_ProfileName);

	FarStop = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FarStop"));
	SetupStaticPart(FarStop, SceneRoot, Cube, UCollisionProfile::BlockAll_ProfileName);

	AnchorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AnchorMesh"));
	SetupStaticPart(AnchorMesh, SceneRoot, Cube, UCollisionProfile::NoCollision_ProfileName);

	AnchorLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("AnchorLabel"));
	AnchorLabel->SetupAttachment(SceneRoot);
	SetupText(AnchorLabel, 40.f, LetterOnBodyColor);
	AnchorLabel->SetText(FText::FromString(HueColor::Label(EHueColor::A)));

	StateLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("StateLabel"));
	StateLabel->SetupAttachment(SceneRoot);
	SetupText(StateLabel, 24.f, FColor::White);

	// Carriage: the only simulated body. Chaos DOF lock keeps it on the track (X translation only).
	Carriage = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Carriage"));
	Carriage->SetupAttachment(SceneRoot);
	if (Cube)
	{
		Carriage->SetStaticMesh(Cube);
	}
	Carriage->SetMobility(EComponentMobility::Movable);
	Carriage->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	Carriage->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore); // the player walks through, never pushes it
	Carriage->SetGenerateOverlapEvents(false);
	Carriage->SetCanEverAffectNavigation(false);
	Carriage->SetRelativeScale3D(FVector(2.f * CarriageHalfLength / 100.f, CarriageWidth / 100.f, CarriageHeight / 100.f));
	Carriage->SetRelativeLocation(FVector(CarriageStartFraction * TrackLength, 0.f, CarriageCenterZ));

	FBodyInstance& Body = Carriage->BodyInstance;
	Body.bSimulatePhysics = false; // simulation starts OFF; a run turns it on
	Body.bEnableGravity = false;
	Body.SetMassOverride(CarriageMassKg, true);
	Body.LinearDamping = BaseDamping;
	Body.DOFMode = EDOFMode::SixDOF;
	Body.bLockXTranslation = false;
	Body.bLockYTranslation = true;
	Body.bLockZTranslation = true;
	Body.bLockXRotation = true;
	Body.bLockYRotation = true;
	Body.bLockZRotation = true;

	// Children of the scaled carriage use absolute scale so they are not stretched by the 80x50x30 box.
	// Their relative locations are still in the carriage's (unit cube, +-50) local space.
	CarriageLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("CarriageLabel"));
	CarriageLabel->SetupAttachment(Carriage);
	CarriageLabel->SetUsingAbsoluteScale(true);
	CarriageLabel->SetRelativeLocation(FVector(0.f, 50.f + 1.f / (CarriageWidth / 100.f), 0.f)); // 1 cm in front of the +Y face
	SetupText(CarriageLabel, 22.f, LetterOnBodyColor);
	CarriageLabel->SetText(FText::FromString(HueColor::Label(EHueColor::N)));

	Figure = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Figure"));
	Figure->SetupAttachment(Carriage);
	if (FigureMesh.Succeeded())
	{
		Figure->SetSkeletalMeshAsset(FigureMesh.Object);
	}
	Figure->SetUsingAbsoluteScale(true);
	Figure->SetRelativeScale3D(FVector(FigureScale));
	Figure->SetRelativeLocation(FVector(0.f, 0.f, 50.f)); // feet on the carriage top
	Figure->SetRelativeRotation(FRotator::ZeroRotator);   // mannequin forward is mesh +Y => looks at the viewer
	Figure->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Figure->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Figure->SetGenerateOverlapEvents(false);
	Figure->SetCanEverAffectNavigation(false);

	UpdateColorVisuals();
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void AReactiveRig::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	LayoutComponents();
}

void AReactiveRig::BeginPlay()
{
	Super::BeginPlay();

	using namespace HueRigPrivate;

	if (!GetActorRotation().IsNearlyZero(0.01f) || !GetActorScale3D().Equals(FVector::OneVector, 0.001f))
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: the rig must be unrotated and unscaled - the Chaos DOF locks are world-axis aligned."), *GetName());
	}

	LayoutComponents();

	// Inelastic, frictionless contacts with the end stops. Created at runtime; the UPROPERTY keeps it alive
	// (the bodies' PhysMaterialOverride references it as well). Properties are set before first use, which
	// is when Chaos creates the material handle from them.
	LowBounceMaterial = NewObject<UPhysicalMaterial>(this, MakeUniqueObjectName(this, UPhysicalMaterial::StaticClass(), TEXT("LowBounceMaterial")));
	LowBounceMaterial->Friction = 0.f;
	LowBounceMaterial->StaticFriction = 0.f;
	LowBounceMaterial->Restitution = 0.f;
	LowBounceMaterial->bOverrideFrictionCombineMode = true;
	LowBounceMaterial->FrictionCombineMode = EFrictionCombineMode::Min;
	LowBounceMaterial->bOverrideRestitutionCombineMode = true;
	LowBounceMaterial->RestitutionCombineMode = EFrictionCombineMode::Min;

	for (UStaticMeshComponent* Part : { Carriage.Get(), NearStop.Get(), FarStop.Get() })
	{
		if (IsValid(Part))
		{
			Part->SetPhysMaterialOverride(LowBounceMaterial);
		}
	}

	if (IsValid(Carriage))
	{
		Carriage->SetMassOverrideInKg(NAME_None, CarriageMassKg, true);
		CarriageMID = TintComponent(Carriage, HueColor::ToLinear(CurrentColor));
	}
	TintComponent(TrackMesh, TrackColor);
	TintComponent(NearStop, StopColor);
	TintComponent(FarStop, StopColor);
	TintComponent(AnchorMesh, HueColor::ToLinear(EHueColor::A));

	ResetRig();
}

void AReactiveRig::LayoutComponents()
{
	using namespace HueRigPrivate;

	const float Length = FMath::Max(TrackLength, 2.f * CarriageHalfLength + 1.f);
	const float HalfLength = FMath::Max(CarriageHalfLength, 1.f);

	if (TrackMesh)
	{
		TrackMesh->SetRelativeLocation(FVector(Length * 0.5f, 0.f, TrackHeight * 0.5f));
		TrackMesh->SetRelativeScale3D(FVector(Length / 100.f, TrackWidth / 100.f, TrackHeight / 100.f));
	}

	// The stops' inner faces sit exactly at X = 0 and X = TrackLength, which is what limits the carriage
	// centre to [CarriageHalfLength, TrackLength - CarriageHalfLength] (GetMin/MaxCarriageCenter).
	const FVector StopScale(StopLength / 100.f, StopWidth / 100.f, StopHeight / 100.f);
	if (NearStop)
	{
		NearStop->SetRelativeLocation(FVector(-StopLength * 0.5f, 0.f, StopHeight * 0.5f));
		NearStop->SetRelativeScale3D(StopScale);
	}
	if (FarStop)
	{
		FarStop->SetRelativeLocation(FVector(Length + StopLength * 0.5f, 0.f, StopHeight * 0.5f));
		FarStop->SetRelativeScale3D(StopScale);
	}

	if (AnchorMesh)
	{
		AnchorMesh->SetRelativeLocation(FVector(AnchorCenterX, 0.f, AnchorSize * 0.5f));
		AnchorMesh->SetRelativeScale3D(FVector(AnchorSize / 100.f));
	}
	if (AnchorLabel)
	{
		AnchorLabel->SetRelativeLocation(FVector(AnchorCenterX, AnchorSize * 0.5f + 1.f, AnchorSize * 0.5f));
	}
	if (StateLabel)
	{
		StateLabel->SetRelativeLocation(FVector(AnchorCenterX, 0.f, StateLabelZ));
	}

	if (Carriage)
	{
		// Only X changes here: the DOF constraint locked Y/Z at the location the body was created at.
		Carriage->SetRelativeScale3D(FVector(2.f * HalfLength / 100.f, CarriageWidth / 100.f, CarriageHeight / 100.f));
		if (!Carriage->IsSimulatingPhysics())
		{
			PlaceCarriageAtStart();
		}
	}
}

// ---- Motion -------------------------------------------------------------------------------------------------

void AReactiveRig::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bMotionActive || bMotionPaused || !IsValid(Carriage) || !Carriage->IsSimulatingPhysics())
	{
		return;
	}

	const FVector Direction = GetTrackDirection();

	// Fixed colour pair: A repels (away from the anchor), B attracts, N applies no colour force (damping brakes).
	// Acceleration change => the force is independent of the carriage mass, matching PredictMotion.
	if (CurrentColor == EHueColor::A)
	{
		Carriage->AddForce(Direction * ColorAcceleration, NAME_None, /*bAccelChange*/ true);
	}
	else if (CurrentColor == EHueColor::B)
	{
		Carriage->AddForce(-Direction * ColorAcceleration, NAME_None, /*bAccelChange*/ true);
	}

	// Speed cap along the track (Y/Z are locked, so the clamped velocity is purely axial).
	const float Speed = static_cast<float>(FVector::DotProduct(Carriage->GetPhysicsLinearVelocity(), Direction));
	if (MaxSpeed > 0.f && FMath::Abs(Speed) > MaxSpeed)
	{
		Carriage->SetPhysicsLinearVelocity(Direction * (FMath::Sign(Speed) * MaxSpeed));
	}

	AttemptMaxFraction = FMath::Max(AttemptMaxFraction, GetCarriageFraction());
}

void AReactiveRig::ApplyColor(EHueColor NewColor)
{
	CurrentColor = NewColor;
	if (IsValid(Carriage))
	{
		// Chaos applies v *= max(0, 1 - damping * dt) per step; N raises it for a gradual stop.
		Carriage->SetLinearDamping(NewColor == EHueColor::N ? BrakeDamping : BaseDamping);
	}
	UpdateColorVisuals();
}

void AReactiveRig::SetMotionActive(bool bActive)
{
	bMotionActive = bActive;
	bMotionPaused = false;
	PausedVelocity = FVector::ZeroVector;

	if (!IsValid(Carriage))
	{
		return;
	}

	if (bActive)
	{
		// Note: enabling simulation detaches the carriage from SceneRoot (KeepWorldTransform); ResetRig re-attaches.
		Carriage->SetSimulatePhysics(true);
		Carriage->SetPhysicsLinearVelocity(FVector::ZeroVector);
		Carriage->SetLinearDamping(CurrentColor == EHueColor::N ? BrakeDamping : BaseDamping);
		Carriage->WakeRigidBody();
	}
	else
	{
		if (Carriage->IsSimulatingPhysics())
		{
			Carriage->SetPhysicsLinearVelocity(FVector::ZeroVector);
		}
		Carriage->SetSimulatePhysics(false);
	}
}

void AReactiveRig::SetMotionPaused(bool bPaused)
{
	if (bPaused == bMotionPaused)
	{
		return;
	}
	if (!bMotionActive || !IsValid(Carriage))
	{
		// Nothing is moving; just remember the flag (SetMotionActive / ResetRig clear it).
		bMotionPaused = bPaused;
		PausedVelocity = FVector::ZeroVector;
		return;
	}

	bMotionPaused = bPaused;
	if (bPaused)
	{
		// Tick samples the extreme before this frame's physics step; include the step that just ran so the
		// frozen attempt never reports a max below its final position.
		AttemptMaxFraction = FMath::Max(AttemptMaxFraction, GetCarriageFraction());
		// Freeze: keep the velocity so resume continues the exact same motion.
		PausedVelocity = Carriage->IsSimulatingPhysics() ? Carriage->GetPhysicsLinearVelocity() : FVector::ZeroVector;
		Carriage->SetSimulatePhysics(false);
	}
	else
	{
		Carriage->SetSimulatePhysics(true);
		Carriage->SetPhysicsLinearVelocity(PausedVelocity);
		Carriage->WakeRigidBody();
		PausedVelocity = FVector::ZeroVector;
	}
}

void AReactiveRig::FreezeMotion()
{
	// Same freeze as a pause (also captures the final extreme), but an ended attempt never resumes.
	SetMotionPaused(true);
	PausedVelocity = FVector::ZeroVector;
}

void AReactiveRig::ResetRig()
{
	if (IsValid(Carriage))
	{
		if (Carriage->IsSimulatingPhysics())
		{
			Carriage->SetPhysicsLinearVelocity(FVector::ZeroVector);
			Carriage->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		}
		Carriage->SetSimulatePhysics(false);
		PlaceCarriageAtStart();
	}

	bMotionActive = false;
	bMotionPaused = false;
	PausedVelocity = FVector::ZeroVector;
	ApplyColor(EHueColor::N);
	AttemptMaxFraction = TrackLength > KINDA_SMALL_NUMBER ? static_cast<float>(GetLocalStart().X) / TrackLength : 0.f;
}

FVector AReactiveRig::GetLocalStart() const
{
	const float MinCenter = GetMinCarriageCenter();
	const float MaxCenter = FMath::Max(MinCenter, GetMaxCarriageCenter());
	const float StartX = FMath::Clamp(CarriageStartFraction * TrackLength, MinCenter, MaxCenter);
	return FVector(StartX, 0.f, HueRigPrivate::CarriageCenterZ);
}

FVector AReactiveRig::GetStartLocation() const
{
	return GetActorTransform().TransformPosition(GetLocalStart());
}

void AReactiveRig::PlaceCarriageAtStart()
{
	if (!IsValid(Carriage) || !IsValid(SceneRoot) || Carriage->IsSimulatingPhysics())
	{
		return;
	}
	if (Carriage->GetAttachParent() != SceneRoot)
	{
		// SetSimulatePhysics(true) detached it; restore the hierarchy so the rig layout stays authoritative.
		Carriage->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepWorldTransform);
	}
	Carriage->SetWorldLocationAndRotation(GetStartLocation(), GetActorQuat(), false, nullptr, ETeleportType::TeleportPhysics);
}

// ---- Queries ------------------------------------------------------------------------------------------------

FVector AReactiveRig::GetTrackDirection() const
{
	// == world +X for the required unrotated rig (the DOF lock frame).
	return GetActorForwardVector();
}

float AReactiveRig::GetCarriageFraction() const
{
	if (!IsValid(Carriage) || TrackLength <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(Carriage->GetComponentLocation());
	return FMath::Clamp(static_cast<float>(Local.X) / TrackLength, 0.f, 1.f);
}

float AReactiveRig::GetCarriageSpeed() const
{
	if (!IsValid(Carriage))
	{
		return 0.f;
	}
	if (bMotionPaused)
	{
		return static_cast<float>(FVector::DotProduct(PausedVelocity, GetTrackDirection()));
	}
	if (!Carriage->IsSimulatingPhysics())
	{
		return 0.f;
	}
	return static_cast<float>(FVector::DotProduct(Carriage->GetPhysicsLinearVelocity(), GetTrackDirection()));
}

FHueRigSample AReactiveRig::MakeSample() const
{
	FHueRigSample Sample;
	Sample.TrackFraction = GetCarriageFraction();
	Sample.SpeedFraction = TrackLength > KINDA_SMALL_NUMBER ? GetCarriageSpeed() / TrackLength : 0.f;
	Sample.ReceiverColor = CurrentColor;
	return Sample;
}

// ---- Prediction ---------------------------------------------------------------------------------------------

FHueMotionPrediction AReactiveRig::PredictMotion(const TArray<FHueColorEvent>& Events, float DurationSeconds, float StepSeconds,
	TFunctionRef<void(const FHueRigSample& Sample, float StepSeconds)> OnStep) const
{
	FHueMotionPrediction Result;
	if (TrackLength <= KINDA_SMALL_NUMBER)
	{
		return Result;
	}

	const float Dt = StepSeconds > KINDA_SMALL_NUMBER ? StepSeconds : 1.f / 120.f;
	const float MinCenter = GetMinCarriageCenter();
	const float MaxCenter = FMath::Max(MinCenter, GetMaxCarriageCenter());

	// Own sorted copy (stable: same-time events keep their order, the last one wins).
	TArray<FHueColorEvent> Sorted = Events;
	Sorted.StableSort([](const FHueColorEvent& A, const FHueColorEvent& B) { return A.TimeSeconds < B.TimeSeconds; });

	float X = static_cast<float>(GetLocalStart().X);
	float V = 0.f;
	EHueColor Color = EHueColor::N;
	int32 NextEvent = 0;
	Result.MaxFraction = X / TrackLength;

	const int32 NumSteps = FMath::Max(0, FMath::CeilToInt32(static_cast<double>(DurationSeconds) / Dt - 1e-4));
	for (int32 Step = 0; Step < NumSteps; ++Step)
	{
		// Colour active during [t, t + dt): every event with time <= t (double time: no drift over 2400 steps).
		const double T = static_cast<double>(Step) * Dt;
		while (NextEvent < Sorted.Num() && Sorted[NextEvent].TimeSeconds <= T + 1e-6)
		{
			Color = Sorted[NextEvent].Color;
			++NextEvent;
		}

		// Semi-implicit Euler in the order Chaos integrates: velocity (force, then damping), clamp, position.
		const float Acceleration = Color == EHueColor::A ? ColorAcceleration : (Color == EHueColor::B ? -ColorAcceleration : 0.f);
		const float Damping = Color == EHueColor::N ? BrakeDamping : BaseDamping;
		V += Acceleration * Dt;
		V *= FMath::Max(0.f, 1.f - Damping * Dt);
		if (MaxSpeed > 0.f)
		{
			V = FMath::Clamp(V, -MaxSpeed, MaxSpeed);
		}
		X += V * Dt;

		// Inelastic end stops (restitution 0): the carriage stops dead against them.
		if (X < MinCenter)
		{
			X = MinCenter;
			V = FMath::Max(V, 0.f);
		}
		else if (X > MaxCenter)
		{
			X = MaxCenter;
			V = FMath::Min(V, 0.f);
		}

		const float Fraction = X / TrackLength;
		Result.MaxFraction = FMath::Max(Result.MaxFraction, Fraction);

		FHueRigSample Sample;
		Sample.TrackFraction = Fraction;
		Sample.SpeedFraction = V / TrackLength;
		Sample.ReceiverColor = Color;
		OnStep(Sample, Dt);
	}

	Result.FinalFraction = X / TrackLength;
	Result.FinalSpeed = V; // cm/s, + = away from the anchor (same unit as GetCarriageSpeed)
	return Result;
}

FHueMotionPrediction AReactiveRig::PredictMotion(const TArray<FHueColorEvent>& Events, float DurationSeconds, float StepSeconds) const
{
	return PredictMotion(Events, DurationSeconds, StepSeconds, [](const FHueRigSample&, float) {});
}

// ---- Presentation -------------------------------------------------------------------------------------------

void AReactiveRig::UpdateColorVisuals()
{
	const FLinearColor Color = HueColor::ToLinear(CurrentColor);
	const TCHAR* Letter = HueColor::Label(CurrentColor);

	if (CarriageMID)
	{
		CarriageMID->SetVectorParameterValue(TEXT("Color"), Color);
	}
	if (StateLabel)
	{
		StateLabel->SetText(FText::FromString(FString::Printf(TEXT("Carriage: %s (%s)"), Letter, HueRigPrivate::ResponseName(CurrentColor))));
		StateLabel->SetTextRenderColor(Color.ToFColor(true));
	}
	if (CarriageLabel)
	{
		CarriageLabel->SetText(FText::FromString(Letter));
	}
}

void AReactiveRig::ShowObjectiveMarkers(const TArray<FHueObjective>& Objectives)
{
	using namespace HueRigPrivate;

	ClearObjectiveMarkers();
	if (!GetWorld() || !APulseBlock::GetCubeMesh())
	{
		return;
	}

	for (const FHueObjective& Objective : Objectives)
	{
		switch (Objective.Type)
		{
		case EHueObjectiveType::LeaveStart:
		case EHueObjectiveType::PassFarMarker:
		case EHueObjectiveType::ReturnBelow:
		{
			const FLinearColor Color =
				Objective.Type == EHueObjectiveType::PassFarMarker ? HueColor::ToLinear(EHueColor::A) :
				Objective.Type == EHueObjectiveType::ReturnBelow ? HueColor::ToLinear(EHueColor::B) :
				LeaveStartMarkerColor;
			const float Fraction = FMath::Clamp(Objective.TrackFraction, 0.f, 1.f);
			const float X = Fraction * TrackLength;
			AddMarkerMesh(FVector(X, MarkerSideOffset, PostHeight * 0.5f),
				FVector(PostThickness / 100.f, PostDepth / 100.f, PostHeight / 100.f), Color);
			AddMarkerLabel(FString::Printf(TEXT("%d%%"), FMath::RoundToInt32(Fraction * 100.f)),
				FVector(X, MarkerSideOffset, PostHeight + 14.f), Color);
			break;
		}
		case EHueObjectiveType::StopInZone:
		{
			const float ZoneMin = FMath::Clamp(FMath::Min(Objective.ZoneMin, Objective.ZoneMax), 0.f, 1.f);
			const float ZoneMax = FMath::Clamp(FMath::Max(Objective.ZoneMin, Objective.ZoneMax), 0.f, 1.f);
			const float Length = FMath::Max((ZoneMax - ZoneMin) * TrackLength, 1.f);
			const float CenterX = (ZoneMin + ZoneMax) * 0.5f * TrackLength;
			AddMarkerMesh(FVector(CenterX, 0.f, ZonePlateZ),
				FVector(Length / 100.f, ZonePlateWidth / 100.f, ZonePlateThickness / 100.f), ZoneColor);
			// Floats above the mannequin's head so it never covers the carriage's colour letter when stopped here.
			AddMarkerLabel(TEXT("STOP ZONE"), FVector(CenterX, ZonePlateWidth * 0.5f + 2.f, 135.f), ZoneColor);
			break;
		}
		case EHueObjectiveType::ReceiverColor:
		default:
			break; // no marker on the track
		}
	}
}

void AReactiveRig::ClearObjectiveMarkers()
{
	for (UStaticMeshComponent* Marker : MarkerComponents)
	{
		if (IsValid(Marker))
		{
			Marker->DestroyComponent();
		}
	}
	MarkerComponents.Reset();

	for (UTextRenderComponent* Label : MarkerLabels)
	{
		if (IsValid(Label))
		{
			Label->DestroyComponent();
		}
	}
	MarkerLabels.Reset();
}

UStaticMeshComponent* AReactiveRig::AddMarkerMesh(const FVector& RelativeLocation, const FVector& RelativeScale, const FLinearColor& Color)
{
	// Runtime component: outer = this actor, attached + registered, owned through the MarkerComponents UPROPERTY.
	UStaticMeshComponent* Marker = NewObject<UStaticMeshComponent>(this);
	Marker->SetStaticMesh(APulseBlock::GetCubeMesh());
	Marker->SetMobility(EComponentMobility::Movable);
	Marker->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Marker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Marker->SetGenerateOverlapEvents(false);
	Marker->SetCanEverAffectNavigation(false);
	Marker->SetCastShadow(false);
	Marker->SetupAttachment(SceneRoot);
	Marker->SetRelativeLocation(RelativeLocation);
	Marker->SetRelativeScale3D(RelativeScale);
	Marker->RegisterComponent();
	HueRigPrivate::TintComponent(Marker, Color);
	MarkerComponents.Add(Marker);
	return Marker;
}

void AReactiveRig::AddMarkerLabel(const FString& Text, const FVector& RelativeLocation, const FLinearColor& Color)
{
	UTextRenderComponent* Label = NewObject<UTextRenderComponent>(this);
	Label->SetupAttachment(SceneRoot);
	HueRigPrivate::SetupText(Label, 18.f, Color.ToFColor(true));
	Label->SetRelativeLocation(RelativeLocation);
	Label->SetText(FText::FromString(Text));
	Label->RegisterComponent();
	MarkerLabels.Add(Label);
}
