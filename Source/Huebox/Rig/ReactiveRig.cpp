#include "Rig/ReactiveRig.h"

#include "Blocks/PulseBlock.h"
#include "Core/HueboxSettings.h"
#include "Rig/HuePerformerSkin.h"
#include "Rig/HueShadowPuppet.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
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
#include "ProceduralMeshComponent.h"
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
	constexpr float StateLabelZ = 175.f;
	constexpr float FigureScale = 0.45f;

	// Skin parts (rig-local, viewer on +Y, so "behind" the performer is -Y).
	constexpr float BackdropY = -60.f;
	constexpr float BackdropThickness = 2.f;
	constexpr float BackdropNearOverhang = 200.f;    // screen extends this far behind the anchor end
	constexpr float BackdropFarOverhang = 120.f;
	constexpr float SpotlightY = BackdropY + BackdropThickness * 0.5f + 1.f;
	constexpr float PerformerLetterZ = 60.f;         // world height of the colour letter in front of a standing figure
	constexpr float MovingSpeedThreshold = 15.f;     // cm/s: below this the performer idles
	const TCHAR* const FlatMaterialPath = TEXT("/Game/Huebox/Materials/M_HueUnlit.M_HueUnlit");

	/** Scale of the carriage box; children use absolute scale but their relative location is in box space. */
	FVector CarriageScale(float HalfLength)
	{
		return FVector(2.f * FMath::Max(HalfLength, 1.f) / 100.f, CarriageWidth / 100.f, CarriageHeight / 100.f);
	}

	// Objective markers.
	constexpr float MarkerSideOffset = 45.f;         // towards the viewer
	constexpr float PostThickness = 4.f;
	constexpr float PostDepth = 40.f;
	constexpr float PostHeight = 60.f;
	constexpr float ZonePlateZ = 7.f;
	constexpr float ZonePlateWidth = 90.f;
	constexpr float ZonePlateThickness = 2.f;

	/** Key moves: how long the key letter shows when the skin has no key-action animation. */
	constexpr float KeyActionLetterSeconds = 1.0f;
	/** A key move's hit lands this long after the move starts; a broken board's panel disappears then. */
	constexpr float KeyActionHitSeconds = 0.25f;
	/** Strike-zone spotlight / plate glow (multiplies the key colour; the unlit material blooms above 1). */
	constexpr float StrikeZoneDimGlow = 3.5f;
	constexpr float StrikeZoneLitGlow = 10.f;
	/** Stage board panel (cm): thin along the track, as wide as the track area, taller than the dancer. */
	constexpr float BoardThickness = 8.f;
	constexpr float BoardDepth = 90.f;
	constexpr float BoardHeight = 170.f;
	constexpr float BoardGlow = 2.5f;
	constexpr float BoardLetterSize = 48.f;
	/** How far a figure riding without the carriage box reaches ahead of its centre (cm). */
	constexpr float PerformerFrontOffset = 15.f;
	/** A landed strike shatters the glass when the performer's centre is this close to the contact line (cm); from
	 *  further away it only cracks, and bursts when she gets there. */
	constexpr float ShatterReach = 60.f;
	/** Glow of cracked glass and of the flying pieces. */
	constexpr float CrackedGlow = 5.f;
	/** Shards: a ShardColumns (across the track) x ShardRows grid of pieces, thrown forward and falling. */
	constexpr int32 ShardColumns = 4;
	constexpr int32 ShardRows = 7;
	constexpr float ShardSpeedMin = 150.f;
	constexpr float ShardSpeedMax = 380.f;
	constexpr float ShardGravity = 980.f;
	constexpr float ShardLifeSeconds = 1.6f;
	constexpr float ShardFadeSeconds = 0.5f;
	/** Marks where key moves happened: short posts on the viewer side of the track. */
	constexpr float MoveMarkHeight = 30.f;
	constexpr float MoveMarkGlow = 2.f;
	/** Reason above her head: how long it stays, its height above the track and its letter size (cm). */
	constexpr float ReasonSeconds = 3.f;
	constexpr float ReasonZ = 215.f;
	constexpr float ReasonSize = 16.f;
	constexpr float ReasonY = 30.f;
	/** Shadow puppets: colour letters float this far above their heads, the state line above the partner's letter. */
	constexpr float PuppetLetterAboveHead = 14.f;
	constexpr float PuppetStateLabelZ = 210.f;
	/** The lattice window (generated cm): half its width, and it stands this far in front of the dancer's plane. */
	constexpr float PaneHalfWidth = 19.5f;
	constexpr float PaneInFront = 1.5f;
	/** Glow of a standing pane (x the puppet glow) and of a cracked one. */
	constexpr float PaneGlow = 0.75f;
	constexpr float PaneCrackedGlow = 1.6f;
	static const FLinearColor PaneWood(0.34f, 0.18f, 0.08f);
	/** Her openings in white light (x the skin's glow): cooler and brighter than the plain N colour. */
	static const FLinearColor WhiteOpeningGlow(1.15f, 1.25f, 1.45f);

	// Theatre (skins with bShowTheatre), rig-local cm.
	const TCHAR* const ScreenMaterialPath = TEXT("/Game/Huebox/Materials/M_HueScreen.M_HueScreen");
	const TCHAR* const PoolMaterialPath = TEXT("/Game/Huebox/Materials/M_HueGlowPool.M_HueGlowPool");
	const TCHAR* const PlaneMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
	constexpr float PillarWidth = 26.f;
	constexpr float PillarFrontY = -32.f;
	constexpr float LintelHeight = 52.f;
	constexpr float LintelFrontY = -28.f;
	/** The sill: the stage edge the puppets stand on; it hides the rods and carries the zone captions. */
	constexpr float SillFrontY = 18.f;
	constexpr float SillBottomZ = -26.f;
	/** The low front rail of the stage (栏板): it hides where the rods go down, and carries the zone captions. */
	constexpr float RailTopZ = 26.f;
	constexpr float RailDepth = 5.f;
	constexpr float SillCaptionZ = 13.f;
	constexpr float SillCaptionSize = 15.f;
	constexpr float CurtainWidth = 70.f;
	constexpr int32 CurtainFolds = 5;
	constexpr float ValanceHeight = 26.f;
	constexpr float FriezeSpacing = 100.f;
	/** The dark hall behind the screen: how far it reaches past the screen and how high. */
	constexpr float HallMargin = 1000.f;
	constexpr float HallTop = 1000.f;
	static const FLinearColor HallColor(0.008f, 0.006f, 0.005f);
	static const FLinearColor FriezeGlow(1.5f, 1.05f, 0.45f);
	static const FLinearColor FriezeLeather(0.9f, 0.85f, 0.8f);
	static const FColor SillCaptionColor(240, 214, 160);
	/** Additive pools of light: strength relative to the opaque spotlight colours. */
	constexpr float PoolStrength = 0.32f;

	template <typename T>
	T* LoadCachedAsset(TWeakObjectPtr<T>& Cache, const TCHAR* Path)
	{
		if (!Cache.IsValid())
		{
			Cache = LoadObject<T>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		return Cache.Get();
	}

	/** Two short lines for the stage (the feedback log has the long form, HueColor::MotionMeaning). The partner sits at
	 *  the left end, so ">>" is away from her and "<<" back towards her. */
	FString StageReason(EHueColor Color)
	{
		switch (Color)
		{
		case EHueColor::A: return TEXT("R  like her partner\npushed away  >>");
		case EHueColor::B: return TEXT("C  not like her partner\n<<  pulled back");
		case EHueColor::N: return TEXT("W = R + C\npush and pull cancel: brakes");
		default:           return FString::Printf(TEXT("%s  a key\nher move"), HueColor::Label(Color));
		}
	}

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
		case EHueColor::A: return TEXT("pushed away");
		case EHueColor::B: return TEXT("pulled back");
		default:           return TEXT("brakes");
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

	// Under the rig root (not the physics body): Tick moves it along with her, upright whatever the body does.
	ReasonLabel = CreateDefaultSubobject<UTextRenderComponent>(TEXT("ReasonLabel"));
	ReasonLabel->SetupAttachment(SceneRoot);
	SetupText(ReasonLabel, ReasonSize, LetterOnBodyColor);
	ReasonLabel->SetText(FText::GetEmpty());

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

	ApplySkin();
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

	UpdateStageBoards(DeltaSeconds);
	UpdateShards(DeltaSeconds);
	UpdatePerformer(DeltaSeconds);
	UpdateReason(DeltaSeconds);

	if (!bMotionActive || bMotionPaused || !IsValid(Carriage) || !Carriage->IsSimulatingPhysics())
	{
		return;
	}

	// A board reached during the last physics step stops the performer before any force is added this frame.
	CheckStageBoardContact();
	if (bCrashed)
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
	// Only colours delivered during a run explain themselves (the reset back to N stays quiet).
	if (bMotionActive)
	{
		ShowReason(NewColor);
		if (Puppet)
		{
			Puppet->Flash();
		}
		if (PartnerPuppet)
		{
			// Her partner answers: same colour as him - away with her; not his colour - come back; white - a nod.
			PartnerPuppet->PlayGesture(NewColor == EHueColor::A ? EHuePuppetGesture::Push
				: NewColor == EHueColor::B ? EHuePuppetGesture::Beckon : EHuePuppetGesture::Nod);
		}
	}
}

void AReactiveRig::ShowReason(EHueColor Color)
{
	if (!ReasonLabel)
	{
		return;
	}
	ReasonLabel->SetText(FText::FromString(HueRigPrivate::StageReason(Color)));
	ReasonTimeLeft = HueRigPrivate::ReasonSeconds;
	UpdateReason(0.f);
}

void AReactiveRig::UpdateReason(float DeltaSeconds)
{
	if (!ReasonLabel || ReasonTimeLeft <= 0.f)
	{
		return;
	}
	ReasonTimeLeft -= DeltaSeconds;
	if (ReasonTimeLeft <= 0.f)
	{
		ReasonTimeLeft = 0.f;
		ReasonLabel->SetText(FText::GetEmpty());
		return;
	}
	ReasonLabel->SetRelativeLocation(FVector(GetCarriageFraction() * TrackLength, HueRigPrivate::ReasonY, HueRigPrivate::ReasonZ));
}

void AReactiveRig::PerformKeyAction(EHueColor KeyColor)
{
	using namespace HueRigPrivate;

	const float Fraction = GetCarriageFraction();
	bKeyActionPending = true;
	PendingKeyColor = KeyColor;
	PendingKeyFraction = Fraction;

	// Where the move happened stays marked on the stage, so the next attempt can be tuned by eye.
	if (StageBoards.Num() > 0 && TrackLength > KINDA_SMALL_NUMBER)
	{
		AddMarkerMesh(FVector(Fraction * TrackLength, MarkerSideOffset, MoveMarkHeight * 0.5f),
			FVector(PostThickness / 100.f, PostThickness / 100.f, MoveMarkHeight / 100.f), HueColor::ToLinear(KeyColor) * MoveMarkGlow,
			nullptr, /*bFlat*/ true, FRotator::ZeroRotator, &MoveMarks);
	}
	if (bCrashed)
	{
		return; // already down: no move, nothing breaks
	}

	// The lock this key opens: an unbroken board of its colour, when the performer is within reach (strike zone).
	// The panel disappears when the hit lands, a moment later; the way is open from now on.
	for (int32 Index = 0; Index < StageBoards.Num(); ++Index)
	{
		FStageBoard& Board = StageBoards[Index];
		if (!Board.bBroken && Board.Color == KeyColor && Fraction >= Board.StrikeMin && Fraction <= Board.StrikeMax)
		{
			Board.bBroken = true;
			Board.HitTimeLeft = KeyActionHitSeconds;
			if (StageBoardGlowMIDs.IsValidIndex(Index) && StageBoardGlowMIDs[Index])
			{
				StageBoardGlowMIDs[Index]->SetVectorParameterValue(TEXT("Color"), StrikeZoneColor(KeyColor, /*bLit*/ true));
			}
		}
	}

	// Presentation: the skin's move plays once (the physics body keeps moving underneath), the key letter shows
	// on the performer meanwhile.
	KeyActionShownColor = KeyColor;
	KeyActionTimeLeft = HueRigPrivate::KeyActionLetterSeconds;
	if (Puppet)
	{
		// The water sleeve is thrown; her openings show the key's colour while she does it.
		KeyActionTimeLeft = FMath::Max(Puppet->PlayKeyMove(), 0.1f);
		if (ActiveSkin)
		{
			Puppet->SetGlowColor(HueColor::ToLinear(KeyColor) * ActiveSkin->PuppetGlow);
		}
	}
	else if (KeyActionAnim && IsValid(Figure) && Figure->GetSkeletalMeshAsset())
	{
		Figure->SetPlayRate(1.f);
		Figure->PlayAnimation(KeyActionAnim, /*bLooping*/ false);
		bPlayingMoveAnim = false;
		KeyActionTimeLeft = FMath::Max(KeyActionAnim->GetPlayLength(), 0.1f);
	}

	const FLinearColor Color = HueColor::ToLinear(KeyColor);
	if (CarriageLabel)
	{
		CarriageLabel->SetText(FText::FromString(HueColor::Label(KeyColor)));
		CarriageLabel->SetTextRenderColor(Color.ToFColor(true));
	}
	ShowReason(KeyColor);
	if (StateLabel)
	{
		StateLabel->SetText(FText::FromString(FString::Printf(TEXT("%s: %s move"), *GetPerformerName(), HueColor::Label(KeyColor))));
		StateLabel->SetTextRenderColor(Color.ToFColor(true));
	}
}

FLinearColor AReactiveRig::StrikeZoneColor(EHueColor KeyColor, bool bLit) const
{
	// HDR emissive on the unlit material: dim enough to read as "waiting" against the glowing screen, bright once lit.
	// The theatre's pools of light are added to the screen, so they need less.
	const bool bPool = ActiveSkin && ActiveSkin->bShowBackdrop && ActiveSkin->bShowTheatre;
	return HueColor::ToLinear(KeyColor) * (bLit ? HueRigPrivate::StrikeZoneLitGlow : HueRigPrivate::StrikeZoneDimGlow)
		* (bPool ? HueRigPrivate::PoolStrength : 1.f);
}

void AReactiveRig::RestoreStageBoards()
{
	ClearShards();
	for (int32 Index = 0; Index < StageBoards.Num(); ++Index)
	{
		FStageBoard& Board = StageBoards[Index];
		Board.bBroken = false;
		Board.HitTimeLeft = 0.f;
		Board.bCracked = false;
		Board.bShattered = false;
		if (UStaticMeshComponent* Panel = StageBoardMeshes.IsValidIndex(Index) ? StageBoardMeshes[Index].Get() : nullptr)
		{
			Panel->SetVisibility(true);
			if (UMaterialInstanceDynamic* PanelMID = Cast<UMaterialInstanceDynamic>(Panel->GetMaterial(0)))
			{
				PanelMID->SetVectorParameterValue(TEXT("Color"), HueColor::ToLinear(Board.Color) * HueRigPrivate::BoardGlow);
			}
		}
		if (UHueShadowPuppetComponent* Pane = StageBoardPuppets.IsValidIndex(Index) ? StageBoardPuppets[Index].Get() : nullptr)
		{
			Pane->SetVisibility(true, /*bPropagateToChildren*/ true);
			if (ActiveSkin)
			{
				Pane->SetGlowColor(HueColor::ToLinear(Board.Color) * ActiveSkin->PuppetGlow * HueRigPrivate::PaneGlow);
			}
		}
		if (StageBoardLabels.IsValidIndex(Index) && StageBoardLabels[Index])
		{
			StageBoardLabels[Index]->SetVisibility(true);
		}
		if (StageBoardGlowMIDs.IsValidIndex(Index) && StageBoardGlowMIDs[Index])
		{
			StageBoardGlowMIDs[Index]->SetVectorParameterValue(TEXT("Color"), StrikeZoneColor(Board.Color, /*bLit*/ false));
		}
	}
}

void AReactiveRig::UpdateStageBoards(float DeltaSeconds)
{
	if (StageBoards.Num() == 0 || TrackLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const float Fraction = GetCarriageFraction();
	const float Reach = HueRigPrivate::ShatterReach / TrackLength;
	for (int32 Index = 0; Index < StageBoards.Num(); ++Index)
	{
		FStageBoard& Board = StageBoards[Index];
		if (!Board.bBroken || Board.bShattered)
		{
			continue;
		}
		Board.HitTimeLeft = FMath::Max(0.f, Board.HitTimeLeft - FMath::Max(DeltaSeconds, 0.f));
		const bool bHitLanded = Board.HitTimeLeft <= 0.f;
		const bool bWithinReach = Fraction >= Board.ContactFraction - Reach;

		// Shatter once the hit has landed within reach - or as she gets to the glass (a strike from further away
		// cracked it; reaching the contact line always bursts it, so she never runs into a broken board).
		if ((bHitLanded && bWithinReach) || Fraction >= Board.ContactFraction || (Board.bCracked && bWithinReach))
		{
			ShatterStageBoard(Index);
		}
		else if (bHitLanded && !Board.bCracked)
		{
			Board.bCracked = true;
			UStaticMeshComponent* Panel = StageBoardMeshes.IsValidIndex(Index) ? StageBoardMeshes[Index].Get() : nullptr;
			if (UMaterialInstanceDynamic* PanelMID = Panel ? Cast<UMaterialInstanceDynamic>(Panel->GetMaterial(0)) : nullptr)
			{
				PanelMID->SetVectorParameterValue(TEXT("Color"), HueColor::ToLinear(Board.Color) * HueRigPrivate::CrackedGlow);
			}
			UHueShadowPuppetComponent* Pane = StageBoardPuppets.IsValidIndex(Index) ? StageBoardPuppets[Index].Get() : nullptr;
			if (Pane && ActiveSkin)
			{
				Pane->SetGlowColor(HueColor::ToLinear(Board.Color) * ActiveSkin->PuppetGlow * HueRigPrivate::PaneCrackedGlow);
				Pane->Flash();
			}
		}
	}
}

void AReactiveRig::ShatterStageBoard(int32 Index)
{
	using namespace HueRigPrivate;

	FStageBoard& Board = StageBoards[Index];
	Board.bShattered = true;
	UStaticMeshComponent* Panel = StageBoardMeshes.IsValidIndex(Index) ? StageBoardMeshes[Index].Get() : nullptr;
	if (StageBoardLabels.IsValidIndex(Index) && StageBoardLabels[Index])
	{
		StageBoardLabels[Index]->SetVisibility(false);
	}
	UHueShadowPuppetComponent* Pane = StageBoardPuppets.IsValidIndex(Index) ? StageBoardPuppets[Index].Get() : nullptr;
	if (IsValid(Pane) && ActiveSkin)
	{
		// The lattice breaks into flat pieces of wood and lit openings that tumble in the screen's plane.
		Pane->SetVisibility(false, /*bPropagateToChildren*/ true);
		const float S = ActiveSkin->PuppetScale;
		const float Width = 2.f * PaneHalfWidth * S;
		const float Height = Pane->GetHeight();
		const FVector Base = Pane->GetRelativeLocation();
		const FLinearColor Lit = HueColor::ToLinear(Board.Color) * ActiveSkin->PuppetGlow * PaneCrackedGlow;
		const FLinearColor Wood = PaneWood * ActiveSkin->BackdropColor * ActiveSkin->PuppetTransmission;
		const float PieceWidth = Width / ShardColumns;
		const float PieceHeight = Height / ShardRows;
		FRandomStream Random(1234 + Index);
		for (int32 Row = 0; Row < ShardRows; ++Row)
		{
			for (int32 Column = 0; Column < ShardColumns; ++Column)
			{
				const FVector Offset((Column + 0.5f) * PieceWidth - Width * 0.5f, 0.2f * ((Row + Column) % 3), (Row + 0.5f) * PieceHeight);
				const float Size = Random.FRandRange(0.55f, 0.9f);
				const FVector Scale(PieceWidth * Size / 100.f, 0.01f, PieceHeight * Size / 100.f);
				AddMarkerMesh(Base + Offset, Scale, (Row + Column) % 3 == 0 ? Wood : Lit, nullptr, /*bFlat*/ true,
					FRotator(Random.FRandRange(-25.f, 25.f), 0.f, 0.f), &ShardMeshes);

				FShardMotion Motion;
				Motion.BaseScale = Scale;
				Motion.Velocity = FVector(Random.FRandRange(ShardSpeedMin, ShardSpeedMax) + Offset.X * 2.f, 0.f,
					Random.FRandRange(40.f, 220.f) + (Offset.Z - Height * 0.5f) * 0.8f);
				Motion.Spin = FRotator(Random.FRandRange(-540.f, 540.f), 0.f, 0.f);
				ShardMotions.Add(Motion);
			}
		}
		return;
	}
	if (!IsValid(Panel))
	{
		return;
	}
	Panel->SetVisibility(false);

	// The pane breaks into a grid of pieces (across the track x up), thrown forward - the way she is running - and
	// falling. A fixed seed per board: the same strike always shatters the same way.
	const FVector Center = Panel->GetRelativeLocation();
	const FLinearColor Color = HueColor::ToLinear(Board.Color) * CrackedGlow;
	const float PieceDepth = BoardDepth / ShardColumns;
	const float PieceHeight = BoardHeight / ShardRows;
	FRandomStream Random(1234 + Index);
	for (int32 Row = 0; Row < ShardRows; ++Row)
	{
		for (int32 Column = 0; Column < ShardColumns; ++Column)
		{
			const FVector Offset(0.f, (Column + 0.5f) * PieceDepth - BoardDepth * 0.5f, (Row + 0.5f) * PieceHeight - BoardHeight * 0.5f);
			const float Size = Random.FRandRange(0.55f, 0.9f);
			const FVector Scale(BoardThickness / 100.f, PieceDepth * Size / 100.f, PieceHeight * Size / 100.f);
			AddMarkerMesh(Center + Offset, Scale, Color, nullptr, /*bFlat*/ true,
				FRotator(Random.FRandRange(-20.f, 20.f), 0.f, Random.FRandRange(-20.f, 20.f)), &ShardMeshes);

			FShardMotion Motion;
			Motion.BaseScale = Scale;
			Motion.Velocity = FVector(Random.FRandRange(ShardSpeedMin, ShardSpeedMax), Offset.Y * Random.FRandRange(1.5f, 3.f),
				Random.FRandRange(40.f, 220.f) + Offset.Z * 0.8f);
			Motion.Spin = FRotator(Random.FRandRange(-540.f, 540.f), Random.FRandRange(-360.f, 360.f), Random.FRandRange(-540.f, 540.f));
			ShardMotions.Add(Motion);
		}
	}
}

void AReactiveRig::UpdateShards(float DeltaSeconds)
{
	using namespace HueRigPrivate;

	if (ShardMeshes.Num() == 0)
	{
		return;
	}
	const float Dt = FMath::Max(DeltaSeconds, 0.f);
	bool bAllDone = true;
	for (int32 Index = 0; Index < ShardMeshes.Num() && Index < ShardMotions.Num(); ++Index)
	{
		UStaticMeshComponent* Shard = ShardMeshes[Index];
		FShardMotion& Motion = ShardMotions[Index];
		if (!IsValid(Shard) || Motion.Age >= ShardLifeSeconds)
		{
			continue;
		}
		bAllDone = false;
		Motion.Age += Dt;

		// Ballistic flight; on the stage floor the pieces stop sliding and spinning.
		Motion.Velocity.Z -= ShardGravity * Dt;
		FVector Location = Shard->GetRelativeLocation() + Motion.Velocity * Dt;
		const float FloorZ = TrackHeight + Motion.BaseScale.X * 50.f;
		if (Location.Z <= FloorZ)
		{
			Location.Z = FloorZ;
			Motion.Velocity = FVector::ZeroVector;
			Motion.Spin = FRotator::ZeroRotator;
		}
		Shard->SetRelativeLocationAndRotation(Location, Shard->GetRelativeRotation() + Motion.Spin * Dt);

		// The last part of their life they shrink away.
		const float Fade = FMath::Clamp((ShardLifeSeconds - Motion.Age) / ShardFadeSeconds, 0.f, 1.f);
		Shard->SetRelativeScale3D(Motion.BaseScale * FMath::Max(Fade, 0.01f));
		if (Motion.Age >= ShardLifeSeconds)
		{
			Shard->SetVisibility(false);
		}
	}
	if (bAllDone)
	{
		ClearShards();
	}
}

void AReactiveRig::ClearShards()
{
	for (UStaticMeshComponent* Shard : ShardMeshes)
	{
		if (IsValid(Shard))
		{
			Shard->DestroyComponent();
		}
	}
	ShardMeshes.Reset();
	ShardMotions.Reset();
}

void AReactiveRig::CheckStageBoardContact()
{
	if (bCrashed || StageBoards.Num() == 0 || TrackLength <= KINDA_SMALL_NUMBER || !IsValid(Carriage))
	{
		return;
	}

	const float Fraction = GetCarriageFraction();
	const float StartFraction = static_cast<float>(GetLocalStart().X) / TrackLength;
	for (const FStageBoard& Board : StageBoards)
	{
		// Boards stand ahead of the start; an unbroken one stops the performer at its contact line.
		if (Board.bBroken || StartFraction >= Board.ContactFraction || Fraction < Board.ContactFraction)
		{
			continue;
		}

		bCrashed = true;
		CrashBoardColor = Board.Color;
		// Back to the contact line (at most one frame of overshoot), then frozen for good: the attempt has failed.
		const FVector ContactLocal(Board.ContactFraction * TrackLength, 0.f, HueRigPrivate::CarriageCenterZ);
		Carriage->SetWorldLocation(GetActorTransform().TransformPosition(ContactLocal), false, nullptr, ETeleportType::TeleportPhysics);
		FreezeMotion();

		// Presentation: she falls backwards (the skin's crash animation, held on its last pose).
		KeyActionTimeLeft = 0.f;
		if (Puppet)
		{
			Puppet->PlayFall();
		}
		else if (CrashAnim && IsValid(Figure) && Figure->GetSkeletalMeshAsset())
		{
			Figure->SetPlayRate(1.f);
			Figure->PlayAnimation(CrashAnim, /*bLooping*/ false);
			bPlayingMoveAnim = false;
		}
		UpdateColorVisuals(); // a key letter still showing goes back to the motion colour
		if (CarriageLabel)
		{
			// The fall carries the figure away from the body it rode on; a letter left floating there would mislead.
			// ResetRig (ApplyColor) brings it back.
			CarriageLabel->SetText(FText::GetEmpty());
		}
		if (ReasonLabel)
		{
			// The glass is what stops her now, not the colour: say that instead.
			ReasonLabel->SetText(FText::FromString(FString::Printf(TEXT("ran into the %s glass\nbreak it first: a %s move"),
				HueColor::Name(Board.Color), HueColor::Label(Board.Color))));
			ReasonTimeLeft = HueRigPrivate::ReasonSeconds;
		}
		const FLinearColor Color = HueColor::ToLinear(Board.Color);
		if (StateLabel)
		{
			StateLabel->SetText(FText::FromString(FString::Printf(TEXT("%s: hit the %s glass"), *GetPerformerName(), HueColor::Label(Board.Color))));
			StateLabel->SetTextRenderColor(Color.ToFColor(true));
		}
		break;
	}
}

void AReactiveRig::ClearMoveMarks()
{
	for (UStaticMeshComponent* Mark : MoveMarks)
	{
		if (IsValid(Mark))
		{
			Mark->DestroyComponent();
		}
	}
	MoveMarks.Reset();
}

void AReactiveRig::SetMotionActive(bool bActive)
{
	bMotionActive = bActive;
	bMotionPaused = false;
	bMotionFrozen = false;
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
	bMotionFrozen = true;
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
	bMotionFrozen = false;
	PausedVelocity = FVector::ZeroVector;
	bKeyActionPending = false;
	const bool bWasPerforming = KeyActionTimeLeft > 0.f || bCrashed;
	KeyActionTimeLeft = 0.f;
	bCrashed = false;
	if (bWasPerforming)
	{
		ResumeLoopAnimation();
	}
	// Back at the start, facing her partner; he faces her.
	if (Puppet)
	{
		Puppet->ResetPose(-1.f);
	}
	if (PartnerPuppet)
	{
		PartnerPuppet->ResetPose(1.f);
	}
	UpdatePuppets(0.f);
	RestoreStageBoards();
	ClearMoveMarks();
	ApplyColor(EHueColor::N);
	ReasonTimeLeft = 0.f;
	if (ReasonLabel)
	{
		ReasonLabel->SetText(FText::GetEmpty());
	}
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

FVector AReactiveRig::GetPerformerLocation() const
{
	// The figure rides on the carriage, so the physics body's location is where the performer is.
	return IsValid(Carriage) ? Carriage->GetComponentLocation() : GetActorLocation();
}

FHueRigSample AReactiveRig::MakeSample() const
{
	FHueRigSample Sample;
	Sample.TrackFraction = GetCarriageFraction();
	Sample.SpeedFraction = TrackLength > KINDA_SMALL_NUMBER ? GetCarriageSpeed() / TrackLength : 0.f;
	Sample.ReceiverColor = CurrentColor;
	Sample.bKeyAction = bKeyActionPending;
	Sample.KeyActionColor = PendingKeyColor;
	Sample.KeyActionFraction = PendingKeyFraction;
	Sample.bCrashed = bCrashed;
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

	// Own copy of the stage boards, all standing (as at the start of an attempt).
	TArray<FStageBoard> Boards = StageBoards;
	for (FStageBoard& Board : Boards)
	{
		Board.bBroken = false;
	}
	const float StartFraction = X / TrackLength;
	bool bCrashedHere = false;

	const int32 NumSteps = FMath::Max(0, FMath::CeilToInt32(static_cast<double>(DurationSeconds) / Dt - 1e-4));
	for (int32 Step = 0; Step < NumSteps; ++Step)
	{
		// Colour active during [t, t + dt): every event with time <= t (double time: no drift over 2400 steps).
		// Key colours leave the motion alone; the move happens where the performer is at that time and breaks a board
		// of its colour it is within reach of.
		const double T = static_cast<double>(Step) * Dt;
		FHueRigSample Sample;
		while (NextEvent < Sorted.Num() && Sorted[NextEvent].TimeSeconds <= T + 1e-6)
		{
			const EHueColor EventColor = Sorted[NextEvent].Color;
			if (HueColor::DrivesRig(EventColor))
			{
				Color = EventColor;
			}
			else
			{
				const float MoveFraction = X / TrackLength;
				Sample.bKeyAction = true;
				Sample.KeyActionColor = EventColor;
				Sample.KeyActionFraction = MoveFraction;
				for (FStageBoard& Board : Boards)
				{
					if (!bCrashedHere && !Board.bBroken && Board.Color == EventColor
						&& MoveFraction >= Board.StrikeMin && MoveFraction <= Board.StrikeMax)
					{
						Board.bBroken = true;
					}
				}
			}
			++NextEvent;
		}

		if (bCrashedHere)
		{
			V = 0.f; // down at the board: nothing moves any more
		}
		else
		{
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

			// An unbroken board ahead stops the performer at its contact line for good (see CheckStageBoardContact).
			for (const FStageBoard& Board : Boards)
			{
				if (!Board.bBroken && StartFraction < Board.ContactFraction && X / TrackLength >= Board.ContactFraction)
				{
					X = Board.ContactFraction * TrackLength;
					V = 0.f;
					bCrashedHere = true;
					break;
				}
			}
		}

		const float Fraction = X / TrackLength;
		Result.MaxFraction = FMath::Max(Result.MaxFraction, Fraction);

		Sample.TrackFraction = Fraction;
		Sample.SpeedFraction = V / TrackLength;
		Sample.ReceiverColor = Color;
		Sample.bCrashed = bCrashedHere;
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
		StateLabel->SetText(FText::FromString(FString::Printf(TEXT("%s: %s (%s)"), *GetPerformerName(), Letter, HueRigPrivate::ResponseName(CurrentColor))));
		StateLabel->SetTextRenderColor(Color.ToFColor(true));
	}
	if (CarriageLabel)
	{
		CarriageLabel->SetText(FText::FromString(Letter));
		if (ActiveSkin && ActiveSkin->bLettersInSignalColor)
		{
			CarriageLabel->SetTextRenderColor(Color.ToFColor(true));
		}
	}
	if (!ActiveSkin)
	{
		return;
	}
	// The paper screen glows slightly in the performer's current signal colour (not for white: that would only wash
	// the screen out behind her white openings).
	const FLinearColor Tint = CurrentColor == EHueColor::N ? ActiveSkin->BackdropColor
		: FMath::Lerp(ActiveSkin->BackdropColor, Color * ActiveSkin->BackdropColor.GetMax(), ActiveSkin->BackdropSignalTint);
	if (BackdropMID)
	{
		BackdropMID->SetVectorParameterValue(TEXT("Color"), Tint);
	}
	// Shadow puppets: the screen's light comes through the dyed hide; her carved openings show her colour, his always
	// the partner's red.
	if (Puppet)
	{
		Puppet->SetLeatherLight(Tint * ActiveSkin->PuppetTransmission);
		// White openings: a cool, brighter white, so they stand out from the warm screen and its lamp pools.
		Puppet->SetGlowColor(CurrentColor == EHueColor::N ? HueRigPrivate::WhiteOpeningGlow * ActiveSkin->PuppetGlow : Color * ActiveSkin->PuppetGlow);
	}
	if (PartnerPuppet)
	{
		PartnerPuppet->SetLeatherLight(Tint * ActiveSkin->PuppetTransmission);
		PartnerPuppet->SetGlowColor(HueColor::ToLinear(EHueColor::A) * ActiveSkin->PuppetGlow);
	}
}

// ---- Skin ---------------------------------------------------------------------------------------------------

namespace HueRigPrivate
{
	/** Hue.Skin [index|name]: list the skins, or apply one to every rig of the world (dev / sandbox preview). */
	static void HandleSkinCommand(const TArray<FString>& Args, UWorld* World)
	{
		TArray<TSubclassOf<UHuePerformerSkin>> Skins;
		UHueboxSettings::GetAvailableSkins(Skins);

		if (Args.Num() == 0)
		{
			for (int32 Index = 0; Index < Skins.Num(); ++Index)
			{
				UE_LOG(LogHuebox, Display, TEXT("Hue.Skin %d : %s (%s)"), Index,
					*GetDefault<UHuePerformerSkin>(Skins[Index])->DisplayName.ToString(), *Skins[Index]->GetName());
			}
			return;
		}

		TSubclassOf<UHuePerformerSkin> Chosen;
		if (Args[0].IsNumeric())
		{
			const int32 Index = FCString::Atoi(*Args[0]);
			Chosen = Skins.IsValidIndex(Index) ? Skins[Index] : nullptr;
		}
		else
		{
			for (const TSubclassOf<UHuePerformerSkin>& Skin : Skins)
			{
				if (Skin->GetName().Contains(Args[0]) || GetDefault<UHuePerformerSkin>(Skin)->DisplayName.ToString().Contains(Args[0]))
				{
					Chosen = Skin;
					break;
				}
			}
		}

		if (!Chosen || !World)
		{
			UE_LOG(LogHuebox, Warning, TEXT("Hue.Skin: no skin matches '%s' (run Hue.Skin without arguments to list them)"), *Args[0]);
			return;
		}
		for (TActorIterator<AReactiveRig> It(World); It; ++It)
		{
			It->SetSkin(Chosen);
		}
		UE_LOG(LogHuebox, Display, TEXT("Hue.Skin: applied %s"), *Chosen->GetName());
	}

	static FAutoConsoleCommandWithWorldAndArgs GHueSkinCommand(
		TEXT("Hue.Skin"),
		TEXT("Hue.Skin [index|name] - list the performer skins, or apply one to every rig"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleSkinCommand));
}

void AReactiveRig::SetSkin(TSubclassOf<UHuePerformerSkin> NewSkin)
{
	SkinClass = NewSkin;
	if (HasActorBegunPlay())
	{
		ApplySkin();
	}
}

FString AReactiveRig::GetPerformerName() const
{
	return ActiveSkin ? ActiveSkin->PerformerName : FString(TEXT("Carriage"));
}

UMaterialInstanceDynamic* AReactiveRig::MakeFlatMID(UObject* Outer, const FLinearColor& Color) const
{
	UMaterialInterface* Base = nullptr;
	if (ActiveSkin && !ActiveSkin->FlatMaterial.IsNull())
	{
		Base = ActiveSkin->FlatMaterial.LoadSynchronous();
	}
	if (!Base)
	{
		static TWeakObjectPtr<UMaterialInterface> CachedFlat;
		if (!CachedFlat.IsValid())
		{
			CachedFlat = LoadObject<UMaterialInterface>(nullptr, HueRigPrivate::FlatMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		Base = CachedFlat.Get();
	}
	if (!Base)
	{
		Base = APulseBlock::GetBasicMaterial(); // lit fallback if the project material is missing
	}
	if (!Base)
	{
		return nullptr;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, Outer);
	MID->SetVectorParameterValue(TEXT("Color"), Color);
	return MID;
}

void AReactiveRig::ClearSkinComponents()
{
	for (USceneComponent* Component : SkinComponents)
	{
		if (IsValid(Component))
		{
			Component->DestroyComponent();
		}
	}
	SkinComponents.Reset();
	BackdropMID = nullptr;
	Puppet = nullptr;
	PartnerPuppet = nullptr;
	MoveAnim = nullptr;
	IdleAnim = nullptr;
	KeyActionAnim = nullptr;
	CrashAnim = nullptr;
	bPlayingMoveAnim = false;
	KeyActionTimeLeft = 0.f;
}

void AReactiveRig::ApplySkin()
{
	using namespace HueRigPrivate;

	ClearSkinComponents();

	TSubclassOf<UHuePerformerSkin> Class = SkinClass ? SkinClass : UHueboxSettings::GetDefaultSkinClass();
	if (!Class || Class->HasAnyClassFlags(CLASS_Abstract))
	{
		Class = UHueSkin_Carriage::StaticClass();
	}
	const UHuePerformerSkin* Skin = GetDefault<UHuePerformerSkin>(Class);
	ActiveSkin = Skin;

	const bool bRidesOnBody = Skin->bShowCarriageBody;
	const FVector BoxScale = CarriageScale(CarriageHalfLength);

	// Runtime component helper: outer = this actor, registered, owned through the SkinComponents UPROPERTY.
	auto AddMesh = [this](USceneComponent* Parent, UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation,
		const FVector& Scale, const FLinearColor& Color) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
		Part->SetStaticMesh(Mesh);
		Part->SetMobility(EComponentMobility::Movable);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetCastShadow(false);
		Part->SetupAttachment(Parent);
		Part->SetUsingAbsoluteScale(Parent != SceneRoot);
		Part->SetRelativeLocationAndRotation(Location, Rotation);
		Part->SetRelativeScale3D(Scale);
		Part->RegisterComponent();
		Part->SetMaterial(0, MakeFlatMID(Part, Color));
		SkinComponents.Add(Part);
		return Part;
	};

	// Two-piece paper figure (dress cone + head) for when the skeletal mesh is not available.
	auto AddPrimitiveFigure = [&](USceneComponent* Parent, const FVector& FeetOffset, float Height)
	{
		const FVector Divisor = Parent == Carriage ? BoxScale : FVector::OneVector;
		AddMesh(Parent, APulseBlock::GetConeMesh(), (FeetOffset + FVector(0.f, 0.f, Height * 0.4f)) / Divisor, FRotator::ZeroRotator,
			FVector(Height * 0.0035f, Height * 0.0012f, Height * 0.008f), Skin->PerformerColor);
		AddMesh(Parent, APulseBlock::GetSphereMesh(), (FeetOffset + FVector(0.f, 0.f, Height * 0.9f)) / Divisor, FRotator::ZeroRotator,
			FVector(Height * 0.0018f), Skin->PerformerColor);
	};

	auto FlattenMaterials = [this, Skin](UMeshComponent* Mesh)
	{
		Mesh->EmptyOverrideMaterials();
		if (Skin->bFlatPerformerColor)
		{
			for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				Mesh->SetMaterial(Slot, MakeFlatMID(Mesh, Skin->PerformerColor));
			}
		}
	};

	// ---- Built-in parts ------------------------------------------------------------------------------------
	if (IsValid(Carriage))
	{
		Carriage->SetVisibility(Skin->bShowCarriageBody, /*bPropagateToChildren*/ false);
	}
	if (IsValid(AnchorMesh))
	{
		AnchorMesh->SetVisibility(Skin->bShowAnchorBlock);
	}
	TintComponent(TrackMesh, Skin->StructureColor);
	TintComponent(NearStop, Skin->StructureColor);
	TintComponent(FarStop, Skin->StructureColor);

	const float FeetToCarriageCenter = CarriageCenterZ - TrackHeight; // carriage centre above the track top
	if (CarriageLabel)
	{
		const float LetterZ = bRidesOnBody ? 0.f : (PerformerLetterZ - CarriageCenterZ) / BoxScale.Z;
		CarriageLabel->SetRelativeLocation(FVector(0.f, 50.f + 1.f / BoxScale.Y, LetterZ));
		CarriageLabel->SetTextRenderColor(LetterOnBodyColor);
	}
	if (AnchorLabel)
	{
		AnchorLabel->SetRelativeLocation(Skin->bShowAnchorBlock
			? FVector(AnchorCenterX, AnchorSize * 0.5f + 1.f, AnchorSize * 0.5f)
			: FVector(AnchorCenterX, 30.f, PerformerLetterZ));
		AnchorLabel->SetTextRenderColor(Skin->bLettersInSignalColor ? HueColor::ToLinear(EHueColor::A).ToFColor(true) : LetterOnBodyColor);
	}

	// ---- Performer (the existing Figure component, riding the physics body) ---------------------------------
	USkeletalMesh* PerformerMesh = Skin->PerformerMesh.IsNull() ? nullptr : Skin->PerformerMesh.LoadSynchronous();
	if (IsValid(Figure))
	{
		Figure->Stop();
		Figure->SetAnimation(nullptr);
		Figure->SetSkeletalMeshAsset(PerformerMesh);
		Figure->SetVisibility(PerformerMesh != nullptr);
		Figure->SetRelativeScale3D(FVector(Skin->PerformerScale));
		Figure->SetRelativeLocation(bRidesOnBody ? FVector(0.f, 0.f, 50.f) : FVector(0.f, 0.f, -FeetToCarriageCenter / BoxScale.Z));
		FacingSign = -1.f; // start facing the partner at the anchor
		Figure->SetRelativeRotation(Skin->bFaceMotionDirection ? FRotator(0.f, 90.f, 0.f) : FRotator::ZeroRotator);
		if (PerformerMesh)
		{
			FlattenMaterials(Figure);
			MoveAnim = Skin->PerformerMoveAnim.IsNull() ? nullptr : Skin->PerformerMoveAnim.LoadSynchronous();
			IdleAnim = Skin->PerformerIdleAnim.IsNull() ? nullptr : Skin->PerformerIdleAnim.LoadSynchronous();
			KeyActionAnim = Skin->PerformerKeyActionAnim.IsNull() ? nullptr : Skin->PerformerKeyActionAnim.LoadSynchronous();
			CrashAnim = Skin->PerformerCrashAnim.IsNull() ? nullptr : Skin->PerformerCrashAnim.LoadSynchronous();
			Figure->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			if (IdleAnim)
			{
				Figure->PlayAnimation(IdleAnim, /*bLooping*/ true);
			}
		}
	}
	// ---- Shadow puppets: built here, placed and animated by UpdatePuppets ---------------------------------------
	auto AddPuppet = [this, Skin](EHuePuppetFigure Kind) -> UHueShadowPuppetComponent*
	{
		UHueShadowPuppetComponent* NewPuppet = NewObject<UHueShadowPuppetComponent>(this);
		NewPuppet->SetupAttachment(SceneRoot);
		NewPuppet->RegisterComponent();
		NewPuppet->Build(Kind, Skin->PuppetScale);
		SkinComponents.Add(NewPuppet);
		return NewPuppet->IsBuilt() ? NewPuppet : nullptr;
	};
	if (Skin->PerformerPuppet != EHuePuppetFigure::None)
	{
		Puppet = AddPuppet(Skin->PerformerPuppet);
	}
	if (Skin->bShowPartner && Skin->PartnerPuppet != EHuePuppetFigure::None)
	{
		PartnerPuppet = AddPuppet(Skin->PartnerPuppet);
		if (PartnerPuppet)
		{
			PartnerPuppet->SetRelativeLocation(FVector(AnchorCenterX, GetPuppetPlaneY(), TrackHeight));
		}
	}
	if (Puppet && CarriageLabel)
	{
		// Her colour letter floats above her head, in the puppets' plane (it rides with the physics body).
		const float LetterZ = TrackHeight + Puppet->GetHeight() + PuppetLetterAboveHead;
		CarriageLabel->SetRelativeLocation(FVector(0.f, (GetPuppetPlaneY() + 6.f) / BoxScale.Y, (LetterZ - CarriageCenterZ) / BoxScale.Z));
	}
	if (PartnerPuppet && AnchorLabel)
	{
		AnchorLabel->SetRelativeLocation(FVector(AnchorCenterX, GetPuppetPlaneY() + 6.f, TrackHeight + PartnerPuppet->GetHeight() + PuppetLetterAboveHead));
	}
	if (StateLabel)
	{
		// The puppet stage keeps the floating state line out of the picture (the HUD shows the same line).
		StateLabel->SetRelativeLocation(FVector(AnchorCenterX, 0.f, PartnerPuppet ? PuppetStateLabelZ : StateLabelZ));
		StateLabel->SetVisibility(PartnerPuppet == nullptr);
	}

	if (!PerformerMesh && !Puppet && !Skin->bShowCarriageBody && IsValid(Carriage))
	{
		AddPrimitiveFigure(Carriage, FVector(0.f, 0.f, -FeetToCarriageCenter), 180.f * Skin->PerformerScale);
	}

	// ---- Partner at the fixed anchor ---------------------------------------------------------------------
	if (Skin->bShowPartner && !PartnerPuppet)
	{
		USkeletalMesh* PartnerMesh = Skin->PartnerMesh.IsNull() ? nullptr : Skin->PartnerMesh.LoadSynchronous();
		if (PartnerMesh)
		{
			USkeletalMeshComponent* Partner = NewObject<USkeletalMeshComponent>(this);
			Partner->SetSkeletalMeshAsset(PartnerMesh);
			Partner->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Partner->SetGenerateOverlapEvents(false);
			Partner->SetCanEverAffectNavigation(false);
			Partner->SetupAttachment(SceneRoot);
			Partner->SetRelativeLocationAndRotation(FVector(AnchorCenterX, 0.f, TrackHeight), FRotator(0.f, -90.f, 0.f)); // faces the dancer (+X)
			Partner->SetRelativeScale3D(FVector(Skin->PerformerScale));
			Partner->RegisterComponent();
			FlattenMaterials(Partner);
			if (UAnimSequence* PartnerAnim = Skin->PartnerAnim.IsNull() ? nullptr : Skin->PartnerAnim.LoadSynchronous())
			{
				Partner->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				Partner->PlayAnimation(PartnerAnim, true);
			}
			SkinComponents.Add(Partner);
		}
		else
		{
			AddPrimitiveFigure(SceneRoot, FVector(AnchorCenterX, 0.f, TrackHeight), 180.f * Skin->PerformerScale);
		}
	}

	// ---- Backlit paper screen ------------------------------------------------------------------------------
	const bool bTheatre = Skin->bShowBackdrop && Skin->bShowTheatre;
	if (Skin->bShowBackdrop)
	{
		const float Length = TrackLength + BackdropNearOverhang + BackdropFarOverhang;
		const float CenterX = -BackdropNearOverhang + Length * 0.5f;
		UStaticMeshComponent* Screen = AddMesh(SceneRoot, APulseBlock::GetCubeMesh(),
			FVector(CenterX, BackdropY, Skin->BackdropHeight * 0.5f), FRotator::ZeroRotator,
			FVector(Length / 100.f, BackdropThickness / 100.f, Skin->BackdropHeight / 100.f), Skin->BackdropColor);
		BackdropMID = Cast<UMaterialInstanceDynamic>(Screen->GetMaterial(0));
		static TWeakObjectPtr<UMaterialInterface> ScreenMaterial;
		if (UMaterialInterface* Paper = bTheatre ? LoadCachedAsset(ScreenMaterial, ScreenMaterialPath) : nullptr)
		{
			// Lamp-lit paper (texture, lamp pools and flicker in the material); "Color" is still the tinted light.
			BackdropMID = UMaterialInstanceDynamic::Create(Paper, Screen);
			BackdropMID->SetVectorParameterValue(TEXT("Color"), Skin->BackdropColor);
			Screen->SetMaterial(0, BackdropMID);
		}
	}

	// ---- Theatre (戏台) round the screen -------------------------------------------------------------------
	for (UStaticMeshComponent* Part : { TrackMesh.Get(), NearStop.Get(), FarStop.Get() })
	{
		// The sill takes the track's place; the stops keep their collision (they limit the physics body).
		if (IsValid(Part))
		{
			Part->SetVisibility(!bTheatre);
		}
	}
	if (bTheatre)
	{
		const float MinX = -BackdropNearOverhang;
		const float MaxX = TrackLength + BackdropFarOverhang;
		const float Top = Skin->BackdropHeight;
		const float ScreenFront = BackdropY + BackdropThickness * 0.5f;
		auto Box = [&](const FVector& Min, const FVector& Max, const FLinearColor& Color, bool bLit) -> UStaticMeshComponent*
		{
			UStaticMeshComponent* Part = AddMesh(SceneRoot, APulseBlock::GetCubeMesh(), (Min + Max) * 0.5f, FRotator::ZeroRotator,
				(Max - Min) / 100.f, Color);
			if (bLit)
			{
				TintComponent(Part, Color); // lacquered wood takes the scene's light
			}
			return Part;
		};
		// The dark hall behind: no sky over or beside the stage.
		Box(FVector(MinX - HallMargin, BackdropY - 34.f, -80.f), FVector(MaxX + HallMargin, BackdropY - 30.f, HallTop), HallColor, false);
		// Pillars, lintel, sill.
		for (const float X : { MinX - PillarWidth * 0.5f, MaxX + PillarWidth * 0.5f })
		{
			Box(FVector(X - PillarWidth * 0.5f, BackdropY - 4.f, SillBottomZ), FVector(X + PillarWidth * 0.5f, PillarFrontY, Top + LintelHeight),
				Skin->TheatreLacquer, true);
		}
		Box(FVector(MinX - PillarWidth, BackdropY - 6.f, Top - 4.f), FVector(MaxX + PillarWidth, LintelFrontY, Top + LintelHeight),
			Skin->TheatreLacquer, true);
		Box(FVector(MinX - PillarWidth, BackdropY - 4.f, SillBottomZ), FVector(MaxX + PillarWidth, SillFrontY, TrackHeight),
			Skin->TheatreLacquer, true);
		// Unlit: the rail faces away from the scene's light, and its captions (lit text) must stand out from it.
		Box(FVector(MinX - PillarWidth, SillFrontY - RailDepth, SillBottomZ), FVector(MaxX + PillarWidth, SillFrontY, RailTopZ),
			Skin->TheatreLacquer * 1.5f, false);
		// Curtains at both ends of the screen (folds in two shades) and a valance under the lintel.
		const FLinearColor CurtainDark = Skin->CurtainColor * 0.6f;
		for (const float Start : { MinX, MaxX - CurtainWidth })
		{
			const float FoldWidth = CurtainWidth / CurtainFolds;
			for (int32 Fold = 0; Fold < CurtainFolds; ++Fold)
			{
				const float X0 = Start + Fold * FoldWidth;
				const float Depth = (Fold % 2 == 0) ? 3.f : 0.f;
				Box(FVector(X0, ScreenFront + 1.f + Depth, TrackHeight), FVector(X0 + FoldWidth, ScreenFront + 4.f + Depth, Top),
					Fold % 2 == 0 ? Skin->CurtainColor : CurtainDark, true);
			}
		}
		Box(FVector(MinX, ScreenFront + 1.f, Top - ValanceHeight), FVector(MaxX, ScreenFront + 5.f, Top), Skin->CurtainColor, true);
		// Carved frieze along the lintel's front face.
		if (const FHuePuppetDef* Frieze = HuePuppets::GetFrieze())
		{
			if (UMaterialInterface* PuppetMaterial = HuePuppets::GetPuppetMaterial())
			{
				UMaterialInstanceDynamic* FriezeLeatherMID = UMaterialInstanceDynamic::Create(PuppetMaterial, this);
				UMaterialInstanceDynamic* FriezeGlowMID = UMaterialInstanceDynamic::Create(PuppetMaterial, this);
				FriezeLeatherMID->SetVectorParameterValue(TEXT("Color"), FriezeLeather);
				FriezeGlowMID->SetVectorParameterValue(TEXT("Color"), FriezeGlow);
				const int32 Count = FMath::Max(1, FMath::FloorToInt32((MaxX - MinX) / FriezeSpacing));
				const float Start = (MinX + MaxX) * 0.5f - (Count - 1) * FriezeSpacing * 0.5f;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					UProceduralMeshComponent* Piece = NewObject<UProceduralMeshComponent>(this);
					Piece->SetupAttachment(SceneRoot);
					Piece->SetRelativeLocation(FVector(Start + Index * FriezeSpacing, LintelFrontY + 0.3f, Top + LintelHeight * 0.5f));
					Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Piece->SetCastShadow(false);
					Piece->RegisterComponent();
					HuePuppets::CreatePieceSections(Piece, *Frieze, 0, FriezeLeatherMID, FriezeGlowMID);
					SkinComponents.Add(Piece);
				}
			}
		}
	}

	// Markers follow the skin's style (spotlight vs floor plate).
	if (ShownObjectives.Num() > 0)
	{
		ShowObjectiveMarkers(TArray<FHueObjective>(ShownObjectives));
	}
	if (Puppet)
	{
		Puppet->ResetPose(-1.f);
	}
	if (PartnerPuppet)
	{
		PartnerPuppet->ResetPose(1.f);
	}
	UpdatePuppets(0.f);
	UpdateColorVisuals();
}

float AReactiveRig::GetPuppetPlaneY() const
{
	using namespace HueRigPrivate;
	return BackdropY + BackdropThickness * 0.5f + (ActiveSkin ? ActiveSkin->PuppetScreenGap : 9.f);
}

void AReactiveRig::UpdatePuppets(float DeltaSeconds)
{
	// A player pause freezes the puppets mid-step; an ended attempt lets them settle.
	const bool bHeld = bMotionPaused && !bMotionFrozen && !bCrashed;
	if (Puppet)
	{
		const float LocalX = IsValid(Carriage) ? static_cast<float>(GetActorTransform().InverseTransformPosition(Carriage->GetComponentLocation()).X) : 0.f;
		Puppet->SetRelativeLocation(FVector(LocalX, GetPuppetPlaneY(), HueRigPrivate::TrackHeight));
		if (!bHeld)
		{
			Puppet->UpdatePuppet(DeltaSeconds, bMotionActive && !bMotionPaused ? GetCarriageSpeed() : 0.f);
		}
	}
	if (PartnerPuppet && !bHeld)
	{
		PartnerPuppet->UpdatePuppet(DeltaSeconds, 0.f);
	}
}

void AReactiveRig::UpdatePerformer(float DeltaSeconds)
{
	UpdatePuppets(DeltaSeconds);

	if (bCrashed)
	{
		return; // the fall plays out and holds its last pose until reset
	}

	// A key move plays out first (animation and letter); then the loop animation and labels follow the motion again.
	if (KeyActionTimeLeft > 0.f)
	{
		KeyActionTimeLeft -= FMath::Max(DeltaSeconds, 0.f);
		if (KeyActionTimeLeft > 0.f)
		{
			return;
		}
		KeyActionTimeLeft = 0.f;
		UpdateColorVisuals();
		ResumeLoopAnimation();
	}

	if (!ActiveSkin || !IsValid(Figure) || !Figure->GetSkeletalMeshAsset())
	{
		return;
	}

	const float Speed = GetCarriageSpeed();
	const bool bMoving = FMath::Abs(Speed) > HueRigPrivate::MovingSpeedThreshold;

	if (ActiveSkin->bFaceMotionDirection && bMoving)
	{
		const float NewSign = Speed > 0.f ? 1.f : -1.f;
		if (NewSign != FacingSign)
		{
			FacingSign = NewSign;
			// Mesh forward is +Y: yaw -90 faces +X (away from the anchor), +90 faces the anchor.
			Figure->SetRelativeRotation(FRotator(0.f, FacingSign > 0.f ? -90.f : 90.f, 0.f));
		}
	}

	if (bMoving && MoveAnim)
	{
		if (!bPlayingMoveAnim)
		{
			Figure->PlayAnimation(MoveAnim, true);
			bPlayingMoveAnim = true;
		}
		Figure->SetPlayRate(FMath::Clamp(FMath::Abs(Speed) / FMath::Max(MaxSpeed, 1.f) * 1.4f, 0.4f, 1.6f));
	}
	else if (!bMoving && bPlayingMoveAnim)
	{
		bPlayingMoveAnim = false;
		Figure->SetPlayRate(1.f);
		if (IdleAnim)
		{
			Figure->PlayAnimation(IdleAnim, true);
		}
		else
		{
			Figure->Stop();
		}
	}
}

void AReactiveRig::ResumeLoopAnimation()
{
	if (!IsValid(Figure) || !Figure->GetSkeletalMeshAsset())
	{
		return;
	}
	// UpdatePerformer picks the move loop up again on its next tick if the performer is moving.
	bPlayingMoveAnim = false;
	Figure->SetPlayRate(1.f);
	if (IdleAnim)
	{
		Figure->PlayAnimation(IdleAnim, /*bLooping*/ true);
	}
	else
	{
		Figure->Stop();
	}
}

void AReactiveRig::ShowObjectiveMarkers(const TArray<FHueObjective>& Objectives)
{
	using namespace HueRigPrivate;

	ClearObjectiveMarkers();
	ShownObjectives = Objectives;

	// Stage boards are rules, not decoration: they exist (and block the performer) even where no marker can be built.
	for (const FHueObjective& Objective : Objectives)
	{
		if (Objective.Type == EHueObjectiveType::BreakBoard)
		{
			FStageBoard Board;
			Board.Color = Objective.Color;
			Board.StrikeMin = FMath::Clamp(FMath::Min(Objective.ZoneMin, Objective.ZoneMax), 0.f, 1.f);
			Board.StrikeMax = FMath::Clamp(FMath::Max(Objective.ZoneMin, Objective.ZoneMax), 0.f, 1.f);
			Board.ContactFraction = FMath::Clamp(FMath::Max(Objective.TrackFraction, Board.StrikeMax), 0.f, 1.f);
			StageBoards.Add(Board);
		}
	}
	StageBoardMeshes.Init(nullptr, StageBoards.Num());
	StageBoardLabels.Init(nullptr, StageBoards.Num());
	StageBoardGlowMIDs.Init(nullptr, StageBoards.Num());
	StageBoardPuppets.Init(nullptr, StageBoards.Num());

	if (!GetWorld() || !APulseBlock::GetCubeMesh())
	{
		return;
	}
	const bool bSpotlight = ActiveSkin && ActiveSkin->bShowBackdrop && ActiveSkin->bZoneAsSpotlight;
	const bool bTheatre = ActiveSkin && ActiveSkin->bShowBackdrop && ActiveSkin->bShowTheatre;
	int32 BoardIndex = 0;

	// A zone on the screen: a soft additive pool of lamp light in the theatre, an opaque disc otherwise. Its caption
	// goes on the sill's front in the theatre (the lintel covers the space above the screen).
	auto AddZoneLight = [this, bTheatre](float CenterX, float Length, const FLinearColor& Color) -> UStaticMeshComponent*
	{
		const float Diameter = FMath::Min(Length, ActiveSkin->BackdropHeight * 0.9f);
		const FVector Center(CenterX, SpotlightY, ActiveSkin->BackdropHeight * 0.45f);
		static TWeakObjectPtr<UStaticMesh> PlaneMesh;
		static TWeakObjectPtr<UMaterialInterface> PoolMaterial;
		UStaticMesh* Plane = bTheatre ? LoadCachedAsset(PlaneMesh, PlaneMeshPath) : nullptr;
		UMaterialInterface* Pool = bTheatre ? LoadCachedAsset(PoolMaterial, PoolMaterialPath) : nullptr;
		if (Plane && Pool)
		{
			// The engine plane lies in XY facing +Z: roll 90 turns it to face the audience (+Y).
			UStaticMeshComponent* Light = AddMarkerMesh(Center, FVector(Diameter / 100.f, Diameter / 100.f, 1.f), Color,
				Plane, /*bFlat*/ true, FRotator(0.f, 0.f, 90.f));
			UMaterialInstanceDynamic* PoolMID = UMaterialInstanceDynamic::Create(Pool, Light);
			PoolMID->SetVectorParameterValue(TEXT("Color"), Color);
			Light->SetMaterial(0, PoolMID);
			return Light;
		}
		return AddMarkerMesh(Center, FVector(Diameter / 100.f, Diameter / 100.f, 0.01f), Color,
			APulseBlock::GetCylinderMesh(), /*bFlat*/ true, FRotator(0.f, 0.f, 90.f));
	};
	auto AddZoneCaption = [this, bTheatre](const FString& Text, float CenterX, const FLinearColor& Color)
	{
		if (bTheatre)
		{
			UTextRenderComponent* Caption = AddMarkerLabel(Text, FVector(CenterX, SillFrontY + 0.5f, SillCaptionZ), Color, SillCaptionSize);
			Caption->SetTextRenderColor(SillCaptionColor);
		}
		else
		{
			AddMarkerLabel(Text, FVector(CenterX, SpotlightY + 2.f, ActiveSkin->BackdropHeight + 18.f), Color);
		}
	};

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
			if (bSpotlight)
			{
				// A glowing spotlight circle on the paper screen (disc = cylinder rolled to face the viewer).
				AddZoneLight(CenterX, Length, ActiveSkin->SpotlightColor * (bTheatre ? PoolStrength : 1.f));
				AddZoneCaption(TEXT("STOP HERE"), CenterX, ZoneColor);
			}
			else
			{
				AddMarkerMesh(FVector(CenterX, 0.f, ZonePlateZ),
					FVector(Length / 100.f, ZonePlateWidth / 100.f, ZonePlateThickness / 100.f), ZoneColor);
				// Floats above the mannequin's head so it never covers the carriage's colour letter when stopped here.
				AddMarkerLabel(TEXT("STOP ZONE"), FVector(CenterX, ZonePlateWidth * 0.5f + 2.f, 135.f), ZoneColor);
			}
			break;
		}
		case EHueObjectiveType::BreakBoard:
		{
			// The lock on the stage: a panel in the key's colour standing on the track, and in front of it the strike
			// zone as a spotlight (or floor plate) in that colour, dim until the board breaks.
			const int32 Index = BoardIndex++;
			const FStageBoard& Board = StageBoards[Index];

			const FLinearColor KeyColor = HueColor::ToLinear(Objective.Color);
			const float Length = FMath::Max((Board.StrikeMax - Board.StrikeMin) * TrackLength, 1.f);
			const float CenterX = (Board.StrikeMin + Board.StrikeMax) * 0.5f * TrackLength;
			const FString Caption = FString::Printf(TEXT("%s STRIKE ZONE"), HueColor::Label(Objective.Color));
			UStaticMeshComponent* Glow = nullptr;
			if (bSpotlight)
			{
				Glow = AddZoneLight(CenterX, Length, StrikeZoneColor(Objective.Color, false));
				AddZoneCaption(Caption, CenterX, KeyColor);
			}
			else
			{
				Glow = AddMarkerMesh(FVector(CenterX, 0.f, ZonePlateZ),
					FVector(Length / 100.f, ZonePlateWidth / 100.f, ZonePlateThickness / 100.f), StrikeZoneColor(Objective.Color, false),
					nullptr, /*bFlat*/ true);
				AddMarkerLabel(Caption, FVector(CenterX, ZonePlateWidth * 0.5f + 2.f, 135.f), KeyColor);
			}

			// The panel's near face is where the performer's front meets it: the carriage box's half length, or the front
			// of a figure riding without the box.
			const float FrontOffset = (ActiveSkin && !ActiveSkin->bShowCarriageBody)
				? (Puppet ? Puppet->GetFrontReach() : PerformerFrontOffset) : CarriageHalfLength;
			if (ActiveSkin && ActiveSkin->PerformerPuppet != EHuePuppetFigure::None)
			{
				// Shadow puppets: a carved lattice window in the puppets' plane, its openings in the key's colour; the
				// letter stands above it.
				UHueShadowPuppetComponent* Pane = NewObject<UHueShadowPuppetComponent>(this);
				Pane->SetupAttachment(SceneRoot);
				const float PaneX = Board.ContactFraction * TrackLength + FrontOffset + PaneHalfWidth * ActiveSkin->PuppetScale;
				Pane->SetRelativeLocation(FVector(PaneX, GetPuppetPlaneY() + PaneInFront, TrackHeight));
				Pane->RegisterComponent();
				Pane->Build(EHuePuppetFigure::Pane, ActiveSkin->PuppetScale);
				Pane->SetLeatherLight(ActiveSkin->BackdropColor * ActiveSkin->PuppetTransmission);
				Pane->SetGlowColor(KeyColor * ActiveSkin->PuppetGlow * PaneGlow);
				StageBoardPuppets[Index] = Pane;
				StageBoardLabels[Index] = AddMarkerLabel(HueColor::Label(Objective.Color),
					FVector(PaneX, GetPuppetPlaneY() + 6.f, TrackHeight + Pane->GetHeight() + PuppetLetterAboveHead), FLinearColor(0.02f, 0.02f, 0.02f), BoardLetterSize);
			}
			else
			{
				const float PanelX = Board.ContactFraction * TrackLength + FrontOffset + BoardThickness * 0.5f;
				StageBoardMeshes[Index] = AddMarkerMesh(FVector(PanelX, 0.f, TrackHeight + BoardHeight * 0.5f),
					FVector(BoardThickness / 100.f, BoardDepth / 100.f, BoardHeight / 100.f), KeyColor * BoardGlow, nullptr, /*bFlat*/ true);
				StageBoardLabels[Index] = AddMarkerLabel(HueColor::Label(Objective.Color),
					FVector(PanelX, BoardDepth * 0.5f + 1.f, TrackHeight + BoardHeight * 0.6f), FLinearColor(0.02f, 0.02f, 0.02f), BoardLetterSize);
			}
			StageBoardGlowMIDs[Index] = Glow ? Cast<UMaterialInstanceDynamic>(Glow->GetMaterial(0)) : nullptr;
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
	ClearShards();
	for (UHueShadowPuppetComponent* Pane : StageBoardPuppets)
	{
		if (IsValid(Pane))
		{
			Pane->DestroyComponent();
		}
	}
	StageBoardPuppets.Reset();
	StageBoards.Reset();
	StageBoardMeshes.Reset();
	StageBoardLabels.Reset();
	StageBoardGlowMIDs.Reset();

	for (UTextRenderComponent* Label : MarkerLabels)
	{
		if (IsValid(Label))
		{
			Label->DestroyComponent();
		}
	}
	MarkerLabels.Reset();
}

UStaticMeshComponent* AReactiveRig::AddMarkerMesh(const FVector& RelativeLocation, const FVector& RelativeScale, const FLinearColor& Color,
	UStaticMesh* Mesh, bool bFlat, const FRotator& RelativeRotation, TArray<TObjectPtr<UStaticMeshComponent>>* OwnerList)
{
	// Runtime component: outer = this actor, attached + registered, owned through the MarkerComponents UPROPERTY.
	UStaticMeshComponent* Marker = NewObject<UStaticMeshComponent>(this);
	Marker->SetStaticMesh(Mesh ? Mesh : APulseBlock::GetCubeMesh());
	Marker->SetRelativeRotation(RelativeRotation);
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
	if (bFlat)
	{
		Marker->SetMaterial(0, MakeFlatMID(Marker, Color));
	}
	else
	{
		HueRigPrivate::TintComponent(Marker, Color);
	}
	(OwnerList ? *OwnerList : MarkerComponents).Add(Marker);
	return Marker;
}

UTextRenderComponent* AReactiveRig::AddMarkerLabel(const FString& Text, const FVector& RelativeLocation, const FLinearColor& Color, float WorldSize)
{
	UTextRenderComponent* Label = NewObject<UTextRenderComponent>(this);
	Label->SetupAttachment(SceneRoot);
	HueRigPrivate::SetupText(Label, WorldSize, Color.ToFColor(true));
	Label->SetRelativeLocation(RelativeLocation);
	Label->SetText(FText::FromString(Text));
	Label->RegisterComponent();
	MarkerLabels.Add(Label);
	return Label;
}
