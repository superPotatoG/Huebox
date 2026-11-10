#include "Rig/HueShadowPuppet.h"

#include "Blocks/PulseBlock.h"
#include "Core/HueTypes.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

// Generated pieces (Scripts/puppets/gen_puppets.py), kept out of the global namespace.
namespace HuePuppetShapes
{
#include "Rig/HuePuppetShapes.inl"
}

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HuePuppetPrivate
{
	const TCHAR* const PuppetMaterialPath = TEXT("/Game/Huebox/Materials/M_HuePuppet.M_HuePuppet");
	const TCHAR* const FlatMaterialPath = TEXT("/Game/Huebox/Materials/M_HueUnlit.M_HueUnlit");

	/** cm/s: slower than this the dancer keeps her facing. */
	constexpr float MovingSpeed = 15.f;
	/** Seconds to flip the figure over when she turns round. */
	constexpr float TurnSeconds = 0.22f;
	/** Gait cycle length in generated cm (one step of each foot). */
	constexpr float StrideCm = 62.f;
	/** Speed (cm/s) at which the walk is at full swing. */
	constexpr float FullGaitSpeed = 160.f;
	constexpr float Gravity = 980.f;
	constexpr float FlashSeconds = 0.45f;
	/** Extra glow at the peak of a flash (x the normal brightness). */
	constexpr float FlashBoost = 1.6f;
	constexpr float FallSeconds = 0.5f;
	constexpr float FallDegrees = 80.f;
	constexpr float GestureSeconds = 1.0f;
	/** Rods: thickness and distance behind the pieces (generated cm). */
	constexpr float RodThickness = 0.6f;
	constexpr float RodDepth = -3.8f;
	/** How far below the feet the rods' lower ends are (hidden by the stage sill). */
	constexpr float RodFootZ = -95.f;
	/** Skirt / robe hem ahead of the origin (generated cm). */
	constexpr float FrontReach = 21.f;
	static const FLinearColor RodColor(0.012f, 0.010f, 0.008f);

	UMaterialInterface* LoadMaterial(const TCHAR* Path)
	{
		return LoadObject<UMaterialInterface>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	float Smooth01(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3.f - 2.f * X);
	}

	/** 0 -> 1 -> 0 over Duration: rises in Rise seconds, falls back over the last Release seconds. */
	float Envelope(float T, float Duration, float Rise, float Release)
	{
		if (T < 0.f || T > Duration)
		{
			return 0.f;
		}
		return FMath::Min(Smooth01(T / FMath::Max(Rise, KINDA_SMALL_NUMBER)), Smooth01((Duration - T) / FMath::Max(Release, KINDA_SMALL_NUMBER)));
	}

	uint8 Quantize(float V)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(V * 255.f), 0, 255));
	}
}

const FHuePuppetDef* HuePuppets::GetDef(EHuePuppetFigure Figure)
{
	switch (Figure)
	{
	case EHuePuppetFigure::Dancer:  return &HuePuppetShapes::DancerPuppet;
	case EHuePuppetFigure::Partner: return &HuePuppetShapes::PartnerPuppet;
	case EHuePuppetFigure::Pane:    return &HuePuppetShapes::PanePuppet;
	default:                        return nullptr;
	}
}

UHueShadowPuppetComponent::UHueShadowPuppetComponent()
{
	// The rig drives it (UpdatePuppet) from its own tick.
	PrimaryComponentTick.bCanEverTick = false;
}

int32 UHueShadowPuppetComponent::FindPiece(const TCHAR* Name) const
{
	if (Def)
	{
		for (int32 Index = 0; Index < Def->NumPieces; ++Index)
		{
			if (FCString::Strcmp(Def->Pieces[Index].Name, Name) == 0)
			{
				return Index;
			}
		}
	}
	return INDEX_NONE;
}

void UHueShadowPuppetComponent::Build(EHuePuppetFigure InFigure, float InScale)
{
	using namespace HuePuppetPrivate;

	Figure = InFigure;
	Def = HuePuppets::GetDef(InFigure);
	Scale = FMath::Max(InScale, 0.01f);
	if (!Def)
	{
		return;
	}

	// Sub-components belong to the owning actor (like every runtime part of the rig); this component keeps the
	// references (UPROPERTY) and destroys them with itself.
	UObject* Outer = GetOwner() ? static_cast<UObject*>(GetOwner()) : static_cast<UObject*>(this);
	auto MakeScene = [this, Outer](USceneComponent* Parent, const TCHAR* Name)
	{
		USceneComponent* Scene = NewObject<USceneComponent>(Outer, MakeUniqueObjectName(Outer, USceneComponent::StaticClass(), Name));
		Scene->SetupAttachment(Parent);
		Scene->RegisterComponent();
		return Scene;
	};
	FallRoot = MakeScene(this, TEXT("PuppetFall"));
	FlipRoot = MakeScene(FallRoot, TEXT("PuppetFlip"));

	UMaterialInterface* PuppetMaterial = LoadMaterial(PuppetMaterialPath);
	if (!PuppetMaterial)
	{
		UE_LOG(LogHuebox, Warning, TEXT("Shadow puppet: %s is missing (run Scripts/create_materials.py) - flat fallback"), PuppetMaterialPath);
		PuppetMaterial = APulseBlock::GetBasicMaterial();
	}
	LeatherMID = PuppetMaterial ? UMaterialInstanceDynamic::Create(PuppetMaterial, this) : nullptr;
	GlowMID = PuppetMaterial ? UMaterialInstanceDynamic::Create(PuppetMaterial, this) : nullptr;

	// ---- Pieces: one procedural mesh each, attached along the joints (parents come first in the data) ----------
	PieceMeshes.Reset();
	Angles.Init(0.f, Def->NumPieces);
	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
	for (int32 Index = 0; Index < Def->NumPieces; ++Index)
	{
		const FHuePuppetPieceDef& Piece = Def->Pieces[Index];
		USceneComponent* Parent = PieceMeshes.IsValidIndex(Piece.Parent) ? PieceMeshes[Piece.Parent].Get() : FlipRoot.Get();
		const float ParentLayer = Piece.Parent >= 0 ? Def->Pieces[Piece.Parent].Layer : 0.f;

		UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Outer,
			MakeUniqueObjectName(Outer, UProceduralMeshComponent::StaticClass(), *FString::Printf(TEXT("Puppet_%s"), Piece.Name)));
		Mesh->SetupAttachment(Parent);
		Mesh->SetRelativeLocation(FVector(Piece.PivotX, Piece.Layer - ParentLayer, Piece.PivotZ));
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->RegisterComponent();

		for (int32 Section = 0; Section < 2; ++Section)
		{
			const FHuePuppetSectionRange& Range = Piece.Sections[Section];
			if (Range.NumIndices <= 0 || Range.NumVertices <= 0)
			{
				continue;
			}
			Vertices.Reset(Range.NumVertices);
			Normals.Reset(Range.NumVertices);
			Colors.Reset(Range.NumVertices);
			Triangles.Reset(Range.NumIndices);
			for (int32 V = 0; V < Range.NumVertices; ++V)
			{
				const float* Src = Def->Vertices + static_cast<int64>(Range.FirstVertex + V) * 6;
				Vertices.Emplace(Src[0], Src[1], Src[2]);
				Normals.Emplace(0.f, 1.f, 0.f); // flat, facing the viewer
				Colors.Emplace(Quantize(Src[3]), Quantize(Src[4]), Quantize(Src[5]), 255);
			}
			for (int32 I = 0; I < Range.NumIndices; ++I)
			{
				Triangles.Add(Def->Indices[Range.FirstIndex + I]);
			}
			Mesh->CreateMeshSection(Section, Vertices, Triangles, Normals, TArray<FVector2D>(), Colors, TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);
			Mesh->SetMaterial(Section, Section == 0 ? LeatherMID.Get() : GlowMID.Get());
		}
		PieceMeshes.Add(Mesh);
	}

	Lower = FindPiece(TEXT("lower"));
	Upper = FindPiece(TEXT("upper"));
	Head = FindPiece(TEXT("head"));
	ArmUpperNear = FindPiece(TEXT("arm_upper_near"));
	ArmForeNear = FindPiece(TEXT("arm_fore_near"));
	ArmUpperFar = FindPiece(TEXT("arm_upper_far"));
	ArmForeFar = FindPiece(TEXT("arm_fore_far"));
	LegNear = FindPiece(TEXT("leg_near"));
	LegFar = FindPiece(TEXT("leg_far"));
	Fan = FindPiece(TEXT("fan"));
	SleeveNear = FindPiece(TEXT("sleeve_near"));

	// ---- Swinging parts ---------------------------------------------------------------------------------------
	Swings.Reset();
	auto AddSwing = [this](const TCHAR* Name, float Stiffness, float Damping, float Drag, float SwayPeriod, float SwayDegrees)
	{
		FSwing Swing;
		Swing.Piece = FindPiece(Name);
		if (Swing.Piece != INDEX_NONE)
		{
			Swing.Stiffness = Stiffness;
			Swing.Damping = Damping;
			Swing.Drag = Drag;
			Swing.SwayPeriod = SwayPeriod;
			Swing.SwayDegrees = SwayDegrees;
			Swings.Add(Swing);
		}
	};
	AddSwing(TEXT("sleeve_near"), 42.f, 3.0f, 0.075f, 2.7f, 4.f);
	AddSwing(TEXT("sleeve_far"), 40.f, 3.0f, 0.075f, 3.1f, 4.f);
	AddSwing(TEXT("tassel"), 70.f, 2.4f, 0.09f, 1.9f, 5.f);
	AddSwing(TEXT("ribbon"), 85.f, 4.0f, 0.05f, 2.3f, 2.f);
	AddSwing(TEXT("streamers"), 55.f, 3.0f, 0.06f, 2.5f, 3.f);

	// ---- Rods: neck and both hands, down to the puppeteer below the stage -------------------------------------
	Rods.Reset();
	RodAnchors.Reset();
	RodFeet.Reset();
	UMaterialInterface* FlatMaterial = LoadMaterial(FlatMaterialPath);
	if (!FlatMaterial)
	{
		FlatMaterial = APulseBlock::GetBasicMaterial();
	}
	struct FRodSpec { const TCHAR* Anchor; float FootX; };
	for (const FRodSpec& Spec : { FRodSpec{ TEXT("collar"), -18.f }, FRodSpec{ TEXT("hand_near"), 16.f }, FRodSpec{ TEXT("hand_far"), -4.f } })
	{
		int32 Anchor = INDEX_NONE;
		for (int32 A = 0; A < Def->NumAnchors; ++A)
		{
			if (FCString::Strcmp(Def->Anchors[A].Name, Spec.Anchor) == 0)
			{
				Anchor = A;
			}
		}
		if (Anchor == INDEX_NONE || !APulseBlock::GetCubeMesh())
		{
			continue;
		}
		UStaticMeshComponent* Rod = NewObject<UStaticMeshComponent>(Outer, MakeUniqueObjectName(Outer, UStaticMeshComponent::StaticClass(), TEXT("PuppetRod")));
		Rod->SetStaticMesh(APulseBlock::GetCubeMesh());
		Rod->SetupAttachment(FlipRoot);
		Rod->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Rod->SetGenerateOverlapEvents(false);
		Rod->SetCanEverAffectNavigation(false);
		Rod->SetCastShadow(false);
		Rod->RegisterComponent();
		if (FlatMaterial)
		{
			UMaterialInstanceDynamic* RodMID = UMaterialInstanceDynamic::Create(FlatMaterial, Rod);
			RodMID->SetVectorParameterValue(TEXT("Color"), RodColor);
			Rod->SetMaterial(0, RodMID);
		}
		Rods.Add(Rod);
		RodAnchors.Add(Anchor);
		RodFeet.Add(FVector(Spec.FootX, RodDepth, RodFootZ));
	}

	ResetPose(1.f);
}

void UHueShadowPuppetComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// The pieces belong to the owning actor: take them down with the puppet (a skin change rebuilds it).
	for (UStaticMeshComponent* Rod : Rods)
	{
		if (IsValid(Rod))
		{
			Rod->DestroyComponent();
		}
	}
	for (int32 Index = PieceMeshes.Num() - 1; Index >= 0; --Index)
	{
		if (IsValid(PieceMeshes[Index]))
		{
			PieceMeshes[Index]->DestroyComponent();
		}
	}
	for (USceneComponent* Root : { FlipRoot.Get(), FallRoot.Get() })
	{
		if (IsValid(Root))
		{
			Root->DestroyComponent();
		}
	}
	Rods.Reset();
	PieceMeshes.Reset();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

// ---- Colour ---------------------------------------------------------------------------------------------------

void UHueShadowPuppetComponent::SetLeatherLight(const FLinearColor& Light)
{
	LeatherLight = Light;
	UpdateMaterials();
}

void UHueShadowPuppetComponent::SetGlowColor(const FLinearColor& Glow)
{
	GlowColor = Glow;
	UpdateMaterials();
}

void UHueShadowPuppetComponent::Flash()
{
	FlashLeft = HuePuppetPrivate::FlashSeconds;
	UpdateMaterials();
}

void UHueShadowPuppetComponent::UpdateMaterials()
{
	using namespace HuePuppetPrivate;
	const float Flash = FlashLeft > 0.f ? FMath::Sin(PI * FlashLeft / FlashSeconds) * FlashBoost : 0.f;
	if (GlowMID)
	{
		GlowMID->SetVectorParameterValue(TEXT("Color"), GlowColor * (1.f + Flash));
	}
	if (LeatherMID)
	{
		LeatherMID->SetVectorParameterValue(TEXT("Color"), LeatherLight);
	}
}

// ---- Moves ----------------------------------------------------------------------------------------------------

void UHueShadowPuppetComponent::SetFacing(float Sign, bool bInstant)
{
	TargetFacing = Sign >= 0.f ? 1.f : -1.f;
	if (bInstant)
	{
		FacingScale = TargetFacing;
	}
}

float UHueShadowPuppetComponent::PlayKeyMove()
{
	if (FallTime >= 0.f)
	{
		return 0.f;
	}
	KeyMoveTime = 0.f;
	// The water sleeve is thrown: a kick on its spring, it flies up and settles again.
	for (FSwing& Swing : Swings)
	{
		if (Swing.Piece == SleeveNear)
		{
			Swing.Velocity += 560.f;
		}
	}
	Flash();
	return KeyMoveSeconds;
}

void UHueShadowPuppetComponent::PlayFall()
{
	FallTime = 0.f;
	KeyMoveTime = -1.f;
}

void UHueShadowPuppetComponent::PlayGesture(EHuePuppetGesture Gesture)
{
	GestureKind = Gesture;
	GestureTime = 0.f;
}

void UHueShadowPuppetComponent::ResetPose(float FacingSign)
{
	SetFacing(FacingSign, /*bInstant*/ true);
	GaitPhase = 0.f;
	LastSpeed = 0.f;
	SmoothedSpeed = 0.f;
	SmoothedAccel = 0.f;
	Lean = 0.f;
	LeanVelocity = 0.f;
	Bob = 0.f;
	KeyMoveTime = -1.f;
	FallTime = -1.f;
	GestureTime = -1.f;
	FlashLeft = 0.f;
	for (FSwing& Swing : Swings)
	{
		Swing.WorldAngle = 0.f;
		Swing.Velocity = 0.f;
	}
	if (Def)
	{
		ComputePose(0.f);
		ApplyPose();
		UpdateRods();
		UpdateMaterials();
	}
}

float UHueShadowPuppetComponent::GetFrontReach() const
{
	return HuePuppetPrivate::FrontReach * Scale;
}

float UHueShadowPuppetComponent::GetHeight() const
{
	return Def ? Def->Height * Scale : 0.f;
}

// ---- Animation ------------------------------------------------------------------------------------------------

void UHueShadowPuppetComponent::UpdatePuppet(float DeltaSeconds, float Speed)
{
	using namespace HuePuppetPrivate;
	if (!Def)
	{
		return;
	}
	const float Dt = FMath::Clamp(DeltaSeconds, 0.f, 0.05f);
	Time += Dt;

	if (FallTime >= 0.f)
	{
		FallTime += Dt;
		Speed = 0.f;
	}
	if (KeyMoveTime >= 0.f)
	{
		KeyMoveTime += Dt;
		if (KeyMoveTime > KeyMoveSeconds)
		{
			KeyMoveTime = -1.f;
		}
	}
	if (GestureTime >= 0.f)
	{
		GestureTime += Dt;
		if (GestureTime > GestureSeconds)
		{
			GestureTime = -1.f;
		}
	}
	FlashLeft = FMath::Max(0.f, FlashLeft - Dt);

	// The dancer turns to where she goes: the puppeteer flips the figure over (the mirror passes through edge-on).
	if (FallTime < 0.f && Figure == EHuePuppetFigure::Dancer && FMath::Abs(Speed) > MovingSpeed)
	{
		TargetFacing = Speed > 0.f ? 1.f : -1.f;
	}
	FacingScale = FMath::FInterpConstantTo(FacingScale, TargetFacing, Dt, 2.f / TurnSeconds);

	// Smoothed motion (the physics speed is exact; a pause / reset jump is clamped).
	const float RawAccel = Dt > KINDA_SMALL_NUMBER ? (Speed - LastSpeed) / Dt : 0.f;
	LastSpeed = Speed;
	SmoothedAccel = FMath::FInterpTo(SmoothedAccel, FMath::Clamp(RawAccel, -1500.f, 1500.f), Dt, 8.f);
	SmoothedSpeed = FMath::FInterpTo(SmoothedSpeed, Speed, Dt, 10.f);
	GaitPhase = FMath::Fmod(GaitPhase + FMath::Abs(SmoothedSpeed) * Dt / (StrideCm * Scale) * UE_TWO_PI, UE_TWO_PI);

	ComputePose(Dt);
	ApplyPose();
	UpdateRods();
	UpdateMaterials();
}

void UHueShadowPuppetComponent::ComputePose(float Dt)
{
	using namespace HuePuppetPrivate;

	for (float& Angle : Angles)
	{
		Angle = 0.f;
	}
	auto Set = [this](int32 Piece, float Degrees)
	{
		if (Angles.IsValidIndex(Piece))
		{
			Angles[Piece] = Degrees;
		}
	};
	auto Add = [this](int32 Piece, float Degrees)
	{
		if (Angles.IsValidIndex(Piece))
		{
			Angles[Piece] += Degrees;
		}
	};
	auto Blend = [this](int32 Piece, float Degrees, float Weight)
	{
		if (Angles.IsValidIndex(Piece))
		{
			Angles[Piece] = FMath::Lerp(Angles[Piece], Degrees, Weight);
		}
	};

	// Angles are counter-clockwise as the viewer sees a puppet facing +X: a hanging arm swings forward with +,
	// the body leans back with +.
	const float Gait = FMath::Clamp(FMath::Abs(SmoothedSpeed) / FullGaitSpeed, 0.f, 1.f);
	const float Forward = SmoothedSpeed * TargetFacing;
	const float ForwardAccel = SmoothedAccel * TargetFacing;
	const float Step = FMath::Sin(GaitPhase);
	const float Breath = FMath::Sin(Time * UE_TWO_PI / 3.4f);
	const float Down = FallTime >= 0.f ? 1.f : 0.f;

	// Leaning into a push (accelerating ahead), back against a brake or a pull; on a spring so it overshoots a little.
	const float AccelTilt = FMath::RadiansToDegrees(FMath::Atan(ForwardAccel / Gravity));
	const float LeanTarget = (1.f - Down) * FMath::Clamp(-6.f * Gait - 0.6f * AccelTilt, -16.f, 14.f);
	if (Dt > 0.f)
	{
		LeanVelocity += (90.f * (LeanTarget - Lean) - 11.f * LeanVelocity) * Dt;
		Lean += LeanVelocity * Dt;
	}
	else
	{
		Lean = LeanTarget;
		LeanVelocity = 0.f;
	}
	Bob = (1.f - Down) * Gait * 1.6f * FMath::Abs(Step);

	if (Figure == EHuePuppetFigure::Dancer)
	{
		Set(Lower, 0.35f * Lean + 0.5f * Breath);
		Set(Upper, 0.65f * Lean + 1.2f * Breath);
		Set(Head, 3.f - 0.4f * Lean + 0.8f * FMath::Sin(Time * UE_TWO_PI / 3.4f + 0.7f));
		// A dance pose at rest (the near hand raised in an orchid gesture), arms opening into the walk.
		Set(ArmUpperNear, FMath::Lerp(52.f, 28.f, Gait) - 9.f * Gait * Step + 1.5f * Breath);
		Set(ArmForeNear, FMath::Lerp(78.f, 40.f, Gait));
		Set(ArmUpperFar, FMath::Lerp(-22.f, -36.f, Gait) + 9.f * Gait * Step);
		Set(ArmForeFar, FMath::Lerp(26.f, 20.f, Gait));
		Set(LegNear, 22.f * Gait * Step);
		Set(LegFar, -22.f * Gait * Step);

		if (KeyMoveTime >= 0.f)
		{
			// 甩袖: the near arm sweeps up and out, the body follows forward.
			const float E = Envelope(KeyMoveTime, KeyMoveSeconds, 0.16f, 0.32f);
			Blend(ArmUpperNear, 150.f, E);
			Blend(ArmForeNear, 18.f, E);
			Blend(ArmUpperFar, -62.f, E);
			Blend(ArmForeFar, 30.f, E);
			Add(Upper, -9.f * E);
			Add(Head, 7.f * E);
			Add(LegNear, 16.f * E);
			Add(LegFar, -6.f * E);
		}
		if (FallTime >= 0.f)
		{
			const float E = Smooth01(FallTime / 0.35f);
			Blend(ArmUpperNear, 125.f, E);
			Blend(ArmForeNear, 35.f, E);
			Blend(ArmUpperFar, 105.f, E);
			Blend(ArmForeFar, 25.f, E);
			Blend(LegNear, 28.f, E);
			Blend(LegFar, 12.f, E);
			Add(Upper, -14.f * E);
			Add(Head, -16.f * E);
		}
	}
	else
	{
		// The scholar stands by the anchor: fan at the chest, answering her colours with a gesture.
		Set(Lower, 0.3f * Breath);
		Set(Upper, 0.8f * Breath);
		Set(Head, 1.5f + 0.6f * FMath::Sin(Time * UE_TWO_PI / 3.4f + 0.7f));
		Set(ArmUpperNear, 40.f + 1.2f * Breath);
		Set(ArmForeNear, 55.f);
		Set(Fan, -25.f);
		Set(ArmUpperFar, -10.f);
		Set(ArmForeFar, 10.f);
		if (GestureTime >= 0.f)
		{
			const float E = Envelope(GestureTime, GestureSeconds, 0.18f, 0.35f);
			switch (GestureKind)
			{
			case EHuePuppetGesture::Push:
				Blend(ArmUpperNear, 88.f, E);
				Blend(ArmForeNear, 8.f, E);
				Blend(Fan, -70.f, E);
				Add(Upper, -4.f * E);
				break;
			case EHuePuppetGesture::Beckon:
				Blend(ArmUpperNear, 58.f, E);
				Blend(ArmForeNear, 85.f + 30.f * FMath::Sin(GestureTime * UE_TWO_PI * 2.2f), E);
				Blend(Fan, -10.f, E);
				Add(Upper, 3.f * E);
				break;
			case EHuePuppetGesture::Nod:
			default:
				Add(Head, -7.f * E);
				Add(Upper, -2.f * E);
				break;
			}
		}
	}

	// The fall turns the whole figure about its feet (falling backwards: the head goes behind her).
	float FallAngle = 0.f;
	if (FallTime >= 0.f)
	{
		const float T = FallTime / FallSeconds;
		FallAngle = T < 1.f ? FallDegrees * T * T : FallDegrees - 6.f * FMath::Sin(FMath::Min((FallTime - FallSeconds) / 0.25f, 1.f) * PI);
	}

	// Hanging parts: their angle against gravity is a damped spring towards hanging straight down, trailing the
	// motion and tilted back by forward acceleration (like a pendulum on a cart); local = world - parent's world.
	TArray<float, TInlineAllocator<16>> World;
	World.SetNumZeroed(Angles.Num());
	for (int32 Index = 0; Index < Angles.Num(); ++Index)
	{
		const int32 Parent = Def->Pieces[Index].Parent;
		const float ParentWorld = World.IsValidIndex(Parent) ? World[Parent] : FallAngle;
		FSwing* Swing = Swings.FindByPredicate([Index](const FSwing& S) { return S.Piece == Index; });
		if (Swing)
		{
			const float Sway = Swing->SwayPeriod > 0.f ? Swing->SwayDegrees * FMath::Sin(Time * UE_TWO_PI / Swing->SwayPeriod + Index) : 0.f;
			const float Target = (1.f - Down) * (-Swing->Drag * Forward - AccelTilt) + Sway;
			if (Dt > 0.f)
			{
				Swing->Velocity += (Swing->Stiffness * (Target - Swing->WorldAngle) - Swing->Damping * Swing->Velocity) * Dt;
				Swing->WorldAngle = FMath::Clamp(Swing->WorldAngle + Swing->Velocity * Dt, -160.f, 160.f);
			}
			else
			{
				Swing->WorldAngle = Target;
				Swing->Velocity = 0.f;
			}
			Angles[Index] = Swing->WorldAngle - ParentWorld;
		}
		World[Index] = ParentWorld + Angles[Index];
	}

	if (FallRoot)
	{
		FallRoot->SetRelativeRotation(FRotator(FallAngle * TargetFacing, 0.f, 0.f));
	}
}

void UHueShadowPuppetComponent::ApplyPose()
{
	for (int32 Index = 0; Index < PieceMeshes.Num() && Index < Angles.Num(); ++Index)
	{
		if (UProceduralMeshComponent* Mesh = PieceMeshes[Index])
		{
			// Pitch turns +X towards +Z: counter-clockwise for a viewer on the +Y side.
			Mesh->SetRelativeRotation(FRotator(Angles[Index], 0.f, 0.f));
		}
	}
	if (FlipRoot)
	{
		// Never exactly edge-on: the transform must stay invertible for the rods.
		const float Mirror = FMath::Abs(FacingScale) < 0.03f ? 0.03f * TargetFacing : FacingScale;
		FlipRoot->SetRelativeLocation(FVector(0.f, 0.f, Bob * Scale));
		FlipRoot->SetRelativeScale3D(FVector(Mirror * Scale, Scale, Scale));
	}
}

FVector UHueShadowPuppetComponent::AnchorInFlipSpace(int32 AnchorIndex) const
{
	const FHuePuppetAnchorDef& Anchor = Def->Anchors[AnchorIndex];
	const UProceduralMeshComponent* Mesh = PieceMeshes.IsValidIndex(Anchor.Piece) ? PieceMeshes[Anchor.Piece].Get() : nullptr;
	if (!Mesh || !FlipRoot)
	{
		return FVector::ZeroVector;
	}
	const FVector WorldPoint = Mesh->GetComponentTransform().TransformPosition(FVector(Anchor.X, 0.f, Anchor.Z));
	return FlipRoot->GetComponentTransform().InverseTransformPosition(WorldPoint);
}

void UHueShadowPuppetComponent::UpdateRods()
{
	using namespace HuePuppetPrivate;
	for (int32 Index = 0; Index < Rods.Num(); ++Index)
	{
		UStaticMeshComponent* Rod = Rods[Index];
		if (!Rod || !RodAnchors.IsValidIndex(Index))
		{
			continue;
		}
		FVector Top = AnchorInFlipSpace(RodAnchors[Index]);
		Top.Y = RodDepth;
		const FVector Foot = RodFeet[Index];
		const FVector Along = Top - Foot;
		const float Length = static_cast<float>(Along.Size());
		if (Length < 1.f)
		{
			continue;
		}
		// Cube mesh: 100 cm along its X, stretched from the rod's foot to where it holds the puppet.
		Rod->SetRelativeLocationAndRotation((Top + Foot) * 0.5f, Along.Rotation());
		Rod->SetRelativeScale3D(FVector(Length / 100.f, RodThickness / 100.f, RodThickness / 100.f));
	}
}
