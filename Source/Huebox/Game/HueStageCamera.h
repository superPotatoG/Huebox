#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HueStageCamera.generated.h"

class UCameraComponent;
class AReactiveRig;

/**
 * Side camera of the Watch view: stands on the viewer side (+Y) of a rig and follows the performer along the
 * track (X only, smoothed, clamped to the track +- TrackMargin), looking slightly down at the stage.
 * Owned by the AHueBuilderPawn that spawned it; used as a player view target via SetViewTargetWithBlend.
 */
UCLASS(NotPlaceable, Transient)
class HUEBOX_API AHueStageCamera : public AActor
{
	GENERATED_BODY()

public:
	AHueStageCamera();

	virtual void Tick(float DeltaSeconds) override;

	/** Follows Rig (weak). bSnap jumps to the performer immediately (use when the view becomes active). */
	void SetRig(AReactiveRig* InRig, bool bSnap);
	AReactiveRig* GetRig() const { return Rig.Get(); }
	UCameraComponent* GetCamera() const { return Camera; }

	/** Pure math (unit-tested): camera world location for a rig origin and a followed track X (world). */
	static FVector ComputeCameraLocation(const FVector& RigOrigin, float FollowWorldX, float SideDistance, float Height);
	/** Followed X: performer X clamped to [RigX - Margin, RigX + TrackLength + Margin]. */
	static float ClampFollowX(float PerformerWorldX, float RigWorldX, float TrackLength, float Margin);

	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float SideDistance = 650.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float Height = 170.f;

	/** Height (above the rig origin) the camera aims at; high enough that the zone label above the backdrop
	 *  ("STOP HERE", ~300 cm) stays in frame and, at 16:9, that the frame's bottom edge passes south of the board's
	 *  grid (the camera stands above it), so no row-0 tile pokes into the shot. */
	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float LookHeight = 150.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float FollowSpeed = 4.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float FieldOfView = 55.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float TrackMargin = 150.f;

protected:
	void ApplyTransform();

	/** Aspect ratio of the owning player's viewport (16:9 while unknown: no window, -nullrhi, tests). */
	float GetViewAspect() const;

	/**
	 * Distance (cm, towards the viewer) from the rig origin to the near edge of the board's grid, which lies between
	 * the camera and the stage. A viewport taller than 16:9 sees more below the aim, so ApplyTransform tilts up just
	 * enough to keep the grid's tiles out of the bottom of the frame. 0 disables the correction.
	 */
	UPROPERTY(EditAnywhere, Category = "Hue|Stage Camera")
	float GridEdgeOffset = 170.f;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UCameraComponent> Camera;

	TWeakObjectPtr<AReactiveRig> Rig;
	float FollowX = 0.f;
};
