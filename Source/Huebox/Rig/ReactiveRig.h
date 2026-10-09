#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "ReactiveRig.generated.h"

class UStaticMeshComponent;
class USkeletalMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;
class UPhysicalMaterial;

/** Result of the analytic motion preview (used by tests / graybox validation). */
struct FHueMotionPrediction
{
	float FinalFraction = 0.f;
	float MaxFraction = 0.f;
	float FinalSpeed = 0.f;
};

/**
 * The reactive mechanism: a fixed A-coloured anchor at the local -X end of a straight track and a carriage
 * (carrying the mannequin) constrained to the track with Chaos DOF locks (Y/Z translation + all rotation).
 *
 * Colour response (fixed pair only, no N-body forces):
 *   A on carriage -> same colour as anchor -> repel: constant acceleration away from the anchor, speed capped.
 *   B on carriage -> different colour     -> attract: acceleration towards the anchor (outward motion slows, reverses).
 *   N on carriage -> colour force off, linear damping raised -> gradual braking, never a teleport/instant stop.
 *
 * The rig is expected to have zero rotation (track along world +X), because DOF locks are world-axis aligned.
 */
UCLASS()
class HUEBOX_API AReactiveRig : public AActor
{
	GENERATED_BODY()

public:
	AReactiveRig();

	virtual void Tick(float DeltaSeconds) override;

	/** Receiver output: change the carriage's colour state. */
	void ApplyColor(EHueColor NewColor);
	EHueColor GetCurrentColor() const { return CurrentColor; }

	/** Physics on/off. Run start -> true. Pause stores the velocity and freezes; resume restores it. */
	void SetMotionActive(bool bActive);
	void SetMotionPaused(bool bPaused);
	/** End of an attempt (complete / time limit): freezes the carriage in place for good. Unlike a pause it keeps no
	 *  velocity to resume, so the carriage reports speed 0; ResetRig / the next run start from rest. */
	void FreezeMotion();
	bool IsMotionActive() const { return bMotionActive; }

	/** Restore carriage transform, velocity and colour (N). */
	void ResetRig();

	/** 0 at the anchor end, 1 at the far end (carriage centre). */
	float GetCarriageFraction() const;
	/** Signed speed along the track in cm/s (+ = away from the anchor). */
	float GetCarriageSpeed() const;
	float GetTrackLength() const { return TrackLength; }
	FVector GetTrackDirection() const;
	FHueRigSample MakeSample() const;

	/** Shows marker posts for the objectives (far marker, return marker, target zone). */
	void ShowObjectiveMarkers(const TArray<FHueObjective>& Objectives);

	/** Extreme positions of the current attempt (kept visible after a failed run). */
	float GetAttemptMaxFraction() const { return AttemptMaxFraction; }

	/**
	 * Fixed-step integration of the same model the physics uses (acceleration, speed cap, Chaos-style linear
	 * damping v *= max(0, 1 - c*dt), inelastic end stops). Starts at CarriageStartFraction with colour N at rest;
	 * Events (sorted by time, seconds since run start) change the colour. OnStep receives a sample after every
	 * step (ReceiverColor = colour active during that step) and the step length.
	 * Pure function of the tuning values; used by automation tests to validate the tutorial timings.
	 */
	FHueMotionPrediction PredictMotion(const TArray<FHueColorEvent>& Events, float DurationSeconds, float StepSeconds,
		TFunctionRef<void(const FHueRigSample& Sample, float StepSeconds)> OnStep) const;
	FHueMotionPrediction PredictMotion(const TArray<FHueColorEvent>& Events, float DurationSeconds, float StepSeconds = 1.f / 120.f) const;

	/** Allowed range of the carriage centre along the track (cm from the anchor end), limited by the end stops. */
	float GetMinCarriageCenter() const { return CarriageHalfLength; }
	float GetMaxCarriageCenter() const { return TrackLength - CarriageHalfLength; }

	// ---- Tuning ---------------------------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, Category = "Hue|Rig", meta = (ClampMin = "200"))
	float TrackLength = 1000.f;

	UPROPERTY(EditAnywhere, Category = "Hue|Rig", meta = (ClampMin = "0", ClampMax = "1"))
	float CarriageStartFraction = 0.06f;

	/** cm/s^2 applied along the track while A or B is active. */
	UPROPERTY(EditAnywhere, Category = "Hue|Rig")
	float ColorAcceleration = 250.f;

	/** cm/s speed limit. */
	UPROPERTY(EditAnywhere, Category = "Hue|Rig")
	float MaxSpeed = 300.f;

	/** Linear damping while A or B is active. */
	UPROPERTY(EditAnywhere, Category = "Hue|Rig")
	float BaseDamping = 0.02f;

	/** Linear damping while N is active (braking). */
	UPROPERTY(EditAnywhere, Category = "Hue|Rig")
	float BrakeDamping = 3.0f;

	/** Half length of the carriage along the track (cm). */
	UPROPERTY(EditAnywhere, Category = "Hue|Rig")
	float CarriageHalfLength = 40.f;

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	void LayoutComponents();
	FVector GetStartLocation() const;
	void UpdateColorVisuals();

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> TrackMesh;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> AnchorMesh;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> NearStop;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> FarStop;

	/** Physics body. Not the root: the rig itself never moves. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> Carriage;

	/** Premade mannequin riding on the carriage in a static pose (no collision, no animation). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USkeletalMeshComponent> Figure;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> StateLabel;

	/** Marker posts created at runtime for objectives. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> MarkerComponents;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CarriageMID;

	/** Zero friction / zero restitution material created at runtime; UPROPERTY keeps it from being collected. */
	UPROPERTY(Transient)
	TObjectPtr<UPhysicalMaterial> LowBounceMaterial;

	/** Colour letter on the anchor's viewer face (letters always accompany colours). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> AnchorLabel;

	/** Colour letter on the carriage's viewer face; rides with the physics body. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> CarriageLabel;

	/** Percentage / zone captions of the objective markers (runtime, destroyed together with the markers). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextRenderComponent>> MarkerLabels;

	EHueColor CurrentColor = EHueColor::N;
	bool bMotionActive = false;
	bool bMotionPaused = false;
	FVector PausedVelocity = FVector::ZeroVector;
	float AttemptMaxFraction = 0.f;

private:
	/** Carriage centre at the start of an attempt, rig-local (clamped to the end stops). */
	FVector GetLocalStart() const;
	/** Restores the carriage under the rig root (simulation detaches it) and teleports it to the start. */
	void PlaceCarriageAtStart();
	void ClearObjectiveMarkers();
	UStaticMeshComponent* AddMarkerMesh(const FVector& RelativeLocation, const FVector& RelativeScale, const FLinearColor& Color);
	void AddMarkerLabel(const FString& Text, const FVector& RelativeLocation, const FLinearColor& Color);
};
