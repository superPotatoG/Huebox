#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "HueInteractable.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)
class UHueInteractable : public UInterface
{
	GENERATED_BODY()
};

/**
 * Anything the player can use with the Interact action (left mouse button): pickups, placed modules.
 * Interactor is the player pawn; implementations look up its UHueBuildToolComponent
 * (Interactor->FindComponentByClass<UHueBuildToolComponent>()), so every pawn type works.
 * Call through IHueInteractable::Execute_Interact(Object, Pawn) so Blueprint implementations also work.
 */
class HUEBOX_API IHueInteractable
{
	GENERATED_BODY()

public:
	/** Short prompt for the HUD, e.g. "LMB  Collect Turn block". */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Hue|Interaction")
	FString GetInteractPrompt(AActor* Interactor) const;

	/** Returns true if something happened. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Hue|Interaction")
	bool Interact(AActor* Interactor);
};
