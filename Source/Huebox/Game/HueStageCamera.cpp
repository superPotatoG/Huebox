#include "Game/HueStageCamera.h"

#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Rig/ReactiveRig.h"

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HueStageCameraPrivate
{
	/** The aim point sits a little behind the track (towards the backdrop), so the performer is framed against it. */
	constexpr float LookBehindTrack = 20.f;

	/** Used while the viewport size is unknown (no window yet, -nullrhi, automation tests). */
	constexpr float FallbackAspect = 16.f / 9.f;

	/** Tallest thing on the grid the bottom frame edge must clear: barrier tiles (10 cm) and port markers (~11 cm). */
	constexpr float GridTileHeight = 12.f;
}

AHueStageCamera::AHueStageCamera()
{
	// Cheap follow every frame. The performer is a physics body, so read it after physics moved it.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	SetCanBeDamaged(false);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	RootComponent = Camera;
	Camera->SetFieldOfView(FieldOfView);
	// FieldOfView is horizontal at every window shape (the project's local player default keeps the vertical FOV).
	Camera->bOverrideAspectRatioAxisConstraint = true;
	Camera->SetAspectRatioAxisConstraint(EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV);
}

void AHueStageCamera::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const AReactiveRig* FollowedRig = Rig.Get();
	if (!IsValid(FollowedRig))
	{
		return;
	}

	const FVector Origin = FollowedRig->GetActorLocation();
	const float TargetX = ClampFollowX(static_cast<float>(FollowedRig->GetPerformerLocation().X), static_cast<float>(Origin.X),
		FollowedRig->GetTrackLength(), TrackMargin);
	FollowX = FMath::FInterpTo(FollowX, TargetX, DeltaSeconds, FollowSpeed);
	ApplyTransform();
}

void AHueStageCamera::SetRig(AReactiveRig* InRig, bool bSnap)
{
	Rig = InRig;
	if (!IsValid(InRig))
	{
		return;
	}

	if (bSnap)
	{
		FollowX = ClampFollowX(static_cast<float>(InRig->GetPerformerLocation().X), static_cast<float>(InRig->GetActorLocation().X),
			InRig->GetTrackLength(), TrackMargin);
	}
	ApplyTransform();
}

void AHueStageCamera::ApplyTransform()
{
	if (Camera && !FMath::IsNearlyEqual(Camera->FieldOfView, FieldOfView))
	{
		Camera->SetFieldOfView(FieldOfView);
	}

	const AReactiveRig* FollowedRig = Rig.Get();
	if (!IsValid(FollowedRig))
	{
		return;
	}

	// Rigs are unrotated (track along world +X): the viewer side is +Y, the backdrop -Y.
	const FVector Origin = FollowedRig->GetActorLocation();
	const FVector Location = ComputeCameraLocation(Origin, FollowX, SideDistance, Height);
	const FVector LookAt(FollowX, Origin.Y - HueStageCameraPrivate::LookBehindTrack, Origin.Z + LookHeight);
	FRotator Rotation = (LookAt - Location).Rotation();

	// The camera stands above the board's grid. FieldOfView is horizontal, so a taller window (16:10, 4:3) sees
	// further down: tilt up just enough that the bottom edge still passes over the grid's near edge.
	const float ToGridEdge = SideDistance - GridEdgeOffset;
	if (GridEdgeOffset > 0.f && ToGridEdge > 0.f)
	{
		const float HalfHorizontal = FMath::DegreesToRadians(FMath::Clamp(FieldOfView, 1.f, 170.f) * 0.5f);
		const float HalfVerticalDeg = FMath::RadiansToDegrees(FMath::Atan(FMath::Tan(HalfHorizontal) / GetViewAspect()));
		const float MaxBottomDeg = FMath::RadiansToDegrees(FMath::Atan2(Height - HueStageCameraPrivate::GridTileHeight, ToGridEdge));
		// Pitch is negative looking down; the bottom ray is Pitch - HalfVertical.
		Rotation.Pitch = FMath::Max(Rotation.Pitch, static_cast<double>(HalfVerticalDeg - MaxBottomDeg));
	}
	SetActorLocationAndRotation(Location, Rotation);
}

float AHueStageCamera::GetViewAspect() const
{
	// The owning builder pawn's player is the one looking through this camera.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const APlayerController* PC = OwnerPawn ? OwnerPawn->GetController<APlayerController>() : nullptr;
	int32 SizeX = 0;
	int32 SizeY = 0;
	if (PC)
	{
		PC->GetViewportSize(SizeX, SizeY);
	}
	return (SizeX > 0 && SizeY > 0) ? static_cast<float>(SizeX) / static_cast<float>(SizeY) : HueStageCameraPrivate::FallbackAspect;
}

// Parameter names differ from the header on purpose: SideDistance / Height would shadow the members (C4458).
FVector AHueStageCamera::ComputeCameraLocation(const FVector& RigOrigin, float FollowWorldX, float InSideDistance, float InHeight)
{
	return FVector(FollowWorldX, RigOrigin.Y + InSideDistance, RigOrigin.Z + InHeight);
}

float AHueStageCamera::ClampFollowX(float PerformerWorldX, float RigWorldX, float TrackLength, float Margin)
{
	const float MinX = RigWorldX - Margin;
	// Max() keeps the range valid for a degenerate track / negative margin.
	const float MaxX = FMath::Max(MinX, RigWorldX + FMath::Max(TrackLength, 0.f) + Margin);
	return FMath::Clamp(PerformerWorldX, MinX, MaxX);
}
