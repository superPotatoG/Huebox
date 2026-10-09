#include "Player/HueInventoryComponent.h"

UHueInventoryComponent::UHueInventoryComponent()
{
	// Pure data container: never ticks, needs no world.
	PrimaryComponentTick.bCanEverTick = false;

	Slots.SetNum(FMath::Clamp(NumSlots, 1, 9));
}

void UHueInventoryComponent::InitSlots()
{
	NumSlots = FMath::Clamp(NumSlots, 1, 9);
	Slots.Reset(NumSlots);
	Slots.SetNum(NumSlots);
	SelectedIndex = FMath::Clamp(SelectedIndex, 0, NumSlots - 1);
	Broadcast();
}

void UHueInventoryComponent::EnsureSlots()
{
	const int32 WantedSlots = FMath::Clamp(NumSlots, 1, 9);
	if (Slots.Num() != WantedSlots)
	{
		// Keeps the leading stacks if NumSlots was edited (e.g. in a Blueprint subclass after serialization).
		Slots.SetNum(WantedSlots);
	}

	// Normalise: an empty stack always reads as (None, 0) so equality checks and the HUD stay simple.
	for (FHueInventorySlot& Slot : Slots)
	{
		if (Slot.IsEmpty())
		{
			Slot = FHueInventorySlot();
		}
	}

	SelectedIndex = FMath::Clamp(SelectedIndex, 0, Slots.Num() - 1);
}

void UHueInventoryComponent::Broadcast()
{
	OnInventoryChanged.Broadcast();
}

int32 UHueInventoryComponent::AddItem(EHueModuleType Type, int32 Count)
{
	if (Type == EHueModuleType::None || Count <= 0)
	{
		return 0;
	}

	EnsureSlots();

	const int32 StackCap = FMath::Max(1, MaxStack);
	int32 Remaining = Count;

	// 1) Top up existing stacks of the same type.
	for (FHueInventorySlot& Slot : Slots)
	{
		if (Remaining <= 0)
		{
			break;
		}
		if (!Slot.IsEmpty() && Slot.Type == Type && Slot.Count < StackCap)
		{
			const int32 Take = FMath::Min(Remaining, StackCap - Slot.Count);
			Slot.Count += Take;
			Remaining -= Take;
		}
	}

	// 2) Open new stacks in empty slots.
	for (FHueInventorySlot& Slot : Slots)
	{
		if (Remaining <= 0)
		{
			break;
		}
		if (Slot.IsEmpty())
		{
			const int32 Take = FMath::Min(Remaining, StackCap);
			Slot.Type = Type;
			Slot.Count = Take;
			Remaining -= Take;
		}
	}

	const int32 Added = Count - Remaining;
	if (Added > 0)
	{
		Broadcast();
	}
	return Added;
}

bool UHueInventoryComponent::CanAdd(EHueModuleType Type, int32 Count) const
{
	if (Type == EHueModuleType::None || Count <= 0)
	{
		return false;
	}

	const int32 StackCap = FMath::Max(1, MaxStack);
	const int32 WantedSlots = FMath::Clamp(NumSlots, 1, 9);

	// int64: NumSlots * MaxStack must not overflow for large designer values.
	int64 Free = 0;
	for (int32 Index = 0; Index < WantedSlots; ++Index)
	{
		// Slots that do not exist yet count as empty (EnsureSlots would create them).
		if (!Slots.IsValidIndex(Index) || Slots[Index].IsEmpty())
		{
			Free += StackCap;
		}
		else if (Slots[Index].Type == Type)
		{
			Free += FMath::Max(0, StackCap - Slots[Index].Count);
		}

		if (Free >= Count)
		{
			return true;
		}
	}
	return Free >= Count;
}

bool UHueInventoryComponent::RemoveFromSlot(int32 Index, int32 Count)
{
	if (Count <= 0)
	{
		return false;
	}

	EnsureSlots();

	if (!Slots.IsValidIndex(Index))
	{
		return false;
	}

	FHueInventorySlot& Slot = Slots[Index];
	// All-or-nothing: never take a partial amount.
	if (Slot.IsEmpty() || Slot.Count < Count)
	{
		return false;
	}

	Slot.Count -= Count;
	if (Slot.Count <= 0)
	{
		Slot = FHueInventorySlot();
	}

	Broadcast();
	return true;
}

bool UHueInventoryComponent::RemoveItem(EHueModuleType Type, int32 Count)
{
	if (Type == EHueModuleType::None || Count <= 0)
	{
		return false;
	}

	EnsureSlots();

	// All-or-nothing: check the total before touching any slot.
	if (CountOf(Type) < Count)
	{
		return false;
	}

	// Drain from the last slot backwards so the first (usually selected) stack survives longest.
	int32 Remaining = Count;
	for (int32 Index = Slots.Num() - 1; Index >= 0 && Remaining > 0; --Index)
	{
		FHueInventorySlot& Slot = Slots[Index];
		if (Slot.IsEmpty() || Slot.Type != Type)
		{
			continue;
		}

		const int32 Take = FMath::Min(Remaining, Slot.Count);
		Slot.Count -= Take;
		Remaining -= Take;
		if (Slot.Count <= 0)
		{
			Slot = FHueInventorySlot();
		}
	}

	check(Remaining == 0);
	Broadcast();
	return true;
}

int32 UHueInventoryComponent::CountOf(EHueModuleType Type) const
{
	if (Type == EHueModuleType::None)
	{
		return 0;
	}

	int32 Total = 0;
	for (const FHueInventorySlot& Slot : Slots)
	{
		if (!Slot.IsEmpty() && Slot.Type == Type)
		{
			Total += Slot.Count;
		}
	}
	return Total;
}

void UHueInventoryComponent::SelectSlot(int32 Index)
{
	EnsureSlots();

	const int32 NewIndex = FMath::Clamp(Index, 0, Slots.Num() - 1);
	if (NewIndex != SelectedIndex)
	{
		SelectedIndex = NewIndex;
		Broadcast();
	}
}

void UHueInventoryComponent::CycleSelection(int32 Delta)
{
	EnsureSlots();

	const int32 Num = Slots.Num();
	if (Num <= 0 || Delta == 0)
	{
		return;
	}

	// Positive modulo so -1 from slot 0 wraps to the last slot.
	const int32 NewIndex = ((SelectedIndex + Delta) % Num + Num) % Num;
	if (NewIndex != SelectedIndex)
	{
		SelectedIndex = NewIndex;
		Broadcast();
	}
}

EHueModuleType UHueInventoryComponent::GetSelectedType() const
{
	if (Slots.IsValidIndex(SelectedIndex) && !Slots[SelectedIndex].IsEmpty())
	{
		return Slots[SelectedIndex].Type;
	}
	return EHueModuleType::None;
}

void UHueInventoryComponent::ClearAll()
{
	EnsureSlots();

	for (FHueInventorySlot& Slot : Slots)
	{
		Slot = FHueInventorySlot();
	}
	Broadcast();
}
