#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/HueTypes.h"
#include "HueInventoryComponent.generated.h"

/** One hotbar slot: a stack of identical modules. */
USTRUCT(BlueprintType)
struct FHueInventorySlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory")
	EHueModuleType Type = EHueModuleType::None;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory")
	int32 Count = 0;

	bool IsEmpty() const { return Type == EHueModuleType::None || Count <= 0; }
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnHueInventoryChanged);

/**
 * The maker's module tray: a fixed number of stack slots plus a selected slot.
 * Pure data + rules (no world access), so it is unit-testable with NewObject and no world.
 */
UCLASS(ClassGroup = (Huebox), meta = (BlueprintSpawnableComponent))
class HUEBOX_API UHueInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHueInventoryComponent();

	/** (Re)creates NumSlots empty slots. Called automatically on first use. */
	void InitSlots();

	/** Adds up to Count items (existing stacks first, then empty slots). Returns how many were added. */
	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	int32 AddItem(EHueModuleType Type, int32 Count = 1);

	/** True if all Count items fit. */
	UFUNCTION(BlueprintPure, Category = "Hue|Inventory")
	bool CanAdd(EHueModuleType Type, int32 Count = 1) const;

	/** Removes Count items from slot Index. Fails (no change) if the slot has fewer. */
	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	bool RemoveFromSlot(int32 Index, int32 Count = 1);

	/** Removes Count items of Type across slots. Fails (no change) if not enough. */
	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	bool RemoveItem(EHueModuleType Type, int32 Count = 1);

	UFUNCTION(BlueprintPure, Category = "Hue|Inventory")
	int32 CountOf(EHueModuleType Type) const;

	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	void SelectSlot(int32 Index);

	/** Mouse wheel: +1 / -1 with wrap-around. */
	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	void CycleSelection(int32 Delta);

	UFUNCTION(BlueprintPure, Category = "Hue|Inventory")
	int32 GetSelectedIndex() const { return SelectedIndex; }

	UFUNCTION(BlueprintPure, Category = "Hue|Inventory")
	EHueModuleType GetSelectedType() const;

	UFUNCTION(BlueprintCallable, Category = "Hue|Inventory")
	void ClearAll();

	const TArray<FHueInventorySlot>& GetSlots() const { return Slots; }
	int32 GetNumSlots() const { return NumSlots; }
	int32 GetMaxStack() const { return MaxStack; }

	UPROPERTY(BlueprintAssignable, Category = "Hue|Inventory")
	FOnHueInventoryChanged OnInventoryChanged;

protected:
	UPROPERTY(EditAnywhere, Category = "Hue|Inventory", meta = (ClampMin = "1", ClampMax = "9"))
	int32 NumSlots = 6;

	UPROPERTY(EditAnywhere, Category = "Hue|Inventory", meta = (ClampMin = "1"))
	int32 MaxStack = 12;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Inventory")
	TArray<FHueInventorySlot> Slots;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Inventory")
	int32 SelectedIndex = 0;

	/** Sizes Slots to NumSlots if needed (non-const callers). Const queries treat missing slots as empty. */
	void EnsureSlots();
	void Broadcast();
};
