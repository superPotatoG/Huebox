#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "Player/HueInteractable.h"
#include "HueModulePickup.generated.h"

class UStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;

/**
 * A stack of modules lying in the world (supply tray item, or something the player dropped with G).
 * Interact (LMB) moves as many as fit into the inventory; the pickup disappears when empty.
 */
UCLASS()
class HUEBOX_API AHueModulePickup : public AActor, public IHueInteractable
{
	GENERATED_BODY()

public:
	AHueModulePickup();

	virtual void Tick(float DeltaSeconds) override;

	void SetContents(EHueModuleType InType, int32 InCount);
	EHueModuleType GetModuleType() const { return Type; }
	int32 GetCount() const { return Count; }

	virtual FString GetInteractPrompt_Implementation(AHueboxCharacter* Interactor) const override;
	virtual bool Interact_Implementation(AHueboxCharacter* Interactor) override;

protected:
	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	void RefreshVisuals();

	UPROPERTY(EditAnywhere, Category = "Hue|Pickup")
	EHueModuleType Type = EHueModuleType::Turn;

	UPROPERTY(EditAnywhere, Category = "Hue|Pickup", meta = (ClampMin = "1"))
	int32 Count = 1;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UTextRenderComponent> Label;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MID;

	float BobTime = 0.f;
};
