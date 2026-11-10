#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "HuePulseVisual.generated.h"

class UStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;

/**
 * Pooled presentation of one pulse: a glowing bead with a short fading trail, and its colour letter.
 * Purely cosmetic; owned and recycled by UPulseSubsystem. No collision, no tick.
 */
UCLASS(NotPlaceable, Transient)
class HUEBOX_API AHuePulseVisual : public AActor
{
	GENERATED_BODY()

public:
	AHuePulseVisual();

	void Activate(EHueColor Color, const FVector& Location);
	void Deactivate();
	void SetColor(EHueColor Color);
	/** Puts the bead at Alpha along From -> To (one cell); the trail follows a little behind on the same leg. */
	void SetTravel(const FVector& From, const FVector& To, double Alpha);

	static constexpr int32 NumTrailBeads = 4;

protected:
	virtual void BeginPlay() override;

	/** Unscaled root so the sphere can be scaled without scaling the letter. */
	UPROPERTY(VisibleAnywhere, Category = "Hue")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Hue")
	TObjectPtr<UStaticMeshComponent> Sphere;

	UPROPERTY(VisibleAnywhere, Category = "Hue")
	TObjectPtr<UTextRenderComponent> Letter;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MID;

	/** Fading beads behind the pulse (absolute locations, set by SetTravel). */
	UPROPERTY(VisibleAnywhere, Category = "Hue")
	TArray<TObjectPtr<UStaticMeshComponent>> Trail;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> TrailMIDs;

private:
	/** Creates the sphere's material instance on first use (a pooled visual may be activated before BeginPlay). */
	UMaterialInstanceDynamic* EnsureMID();

	/** Colour currently shown; SetColor is called every frame, so unchanged colours are skipped. */
	EHueColor ShownColor = EHueColor::B;
	bool bHasShownColor = false;
};
