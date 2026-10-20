#include "Player/HueInputConfig.h"

#include "Core/HueTypes.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace HueInputPrivate
{
	/** Actions are subobjects of the config (Outer = config), so the config's UPROPERTYs keep them alive. */
	static UInputAction* MakeAction(UHueInputConfig* Outer, const TCHAR* Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, Name);
		Action->ValueType = ValueType;
		return Action;
	}

	/**
	 * Modifiers are owned by the mapping context (Outer = context) and referenced from the mapping's
	 * UPROPERTY Modifiers array, which is what the GC follows.
	 */
	static UInputModifierSwizzleAxis* MakeSwizzleYXZ(UInputMappingContext* Context)
	{
		UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(Context);
		Swizzle->Order = EInputAxisSwizzle::YXZ;
		return Swizzle;
	}

	static UInputModifierNegate* MakeNegate(UInputMappingContext* Context, bool bX = true, bool bY = true, bool bZ = true)
	{
		UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(Context);
		Negate->bX = bX;
		Negate->bY = bY;
		Negate->bZ = bZ;
		return Negate;
	}

	static UInputModifierScalar* MakeScalar(UInputMappingContext* Context, const FVector& Scalar)
	{
		UInputModifierScalar* Modifier = NewObject<UInputModifierScalar>(Context);
		Modifier->Scalar = Scalar;
		return Modifier;
	}

	/**
	 * Maps Key to Action and appends Modifiers in order (evaluated first to last).
	 * MapKey returns a reference into the context's mapping array; it is used immediately and never cached,
	 * because the next MapKey may reallocate the array.
	 */
	static void Map(UInputMappingContext* Context, const UInputAction* Action, const FKey& Key,
		std::initializer_list<UInputModifier*> Modifiers = {})
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
		for (UInputModifier* Modifier : Modifiers)
		{
			if (Modifier)
			{
				Mapping.Modifiers.Add(Modifier);
			}
		}
	}

	/** Number keys share one Axis1D action; the scalar turns the key's 1.0 into the slot number 1..6. */
	static void MapHotbarSlots(UInputMappingContext* Context, const UInputAction* HotbarSlot)
	{
		const FKey SlotKeys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six };
		double SlotNumber = 1.0;
		for (const FKey& SlotKey : SlotKeys)
		{
			Map(Context, HotbarSlot, SlotKey, { MakeScalar(Context, FVector(SlotNumber, 1.0, 1.0)) });
			SlotNumber += 1.0;
		}
	}

	/** Keys shared by both Instruction-mode contexts: run control, next exercise, help and the pause menu. */
	static void MapRunAndMenuKeys(UInputMappingContext* Context, const UHueInputConfig& Config)
	{
		Map(Context, Config.RunToggle, EKeys::SpaceBar);
		Map(Context, Config.ResetRun, EKeys::R);
		Map(Context, Config.NextBoard, EKeys::Tab);
		Map(Context, Config.NextBoard, EKeys::RightBracket);
		Map(Context, Config.ToggleHelp, EKeys::H);
		Map(Context, Config.PauseMenu, EKeys::Escape);
		Map(Context, Config.PauseMenu, EKeys::P);
	}
}

void UHueInputConfig::BuildDefaults()
{
	if (IsBuilt())
	{
		return;
	}

	// Actions first: IsBuilt() keys off MappingContext, so the sandbox context is created last.
	Move         = HueInputPrivate::MakeAction(this, TEXT("IA_Move"),         EInputActionValueType::Axis2D);
	Look         = HueInputPrivate::MakeAction(this, TEXT("IA_Look"),         EInputActionValueType::Axis2D);
	Interact     = HueInputPrivate::MakeAction(this, TEXT("IA_Interact"),     EInputActionValueType::Boolean);
	RotateLeft   = HueInputPrivate::MakeAction(this, TEXT("IA_RotateLeft"),   EInputActionValueType::Boolean);
	RotateRight  = HueInputPrivate::MakeAction(this, TEXT("IA_RotateRight"),  EInputActionValueType::Boolean);
	DelayDown    = HueInputPrivate::MakeAction(this, TEXT("IA_DelayDown"),    EInputActionValueType::Boolean);
	DelayUp      = HueInputPrivate::MakeAction(this, TEXT("IA_DelayUp"),      EInputActionValueType::Boolean);
	HotbarScroll = HueInputPrivate::MakeAction(this, TEXT("IA_HotbarScroll"), EInputActionValueType::Axis1D);
	HotbarSlot   = HueInputPrivate::MakeAction(this, TEXT("IA_HotbarSlot"),   EInputActionValueType::Axis1D);
	Drop         = HueInputPrivate::MakeAction(this, TEXT("IA_Drop"),         EInputActionValueType::Boolean);
	RunToggle    = HueInputPrivate::MakeAction(this, TEXT("IA_RunToggle"),    EInputActionValueType::Boolean);
	ResetRun     = HueInputPrivate::MakeAction(this, TEXT("IA_ResetRun"),     EInputActionValueType::Boolean);
	ToggleHelp   = HueInputPrivate::MakeAction(this, TEXT("IA_ToggleHelp"),   EInputActionValueType::Boolean);
	PauseMenu    = HueInputPrivate::MakeAction(this, TEXT("IA_PauseMenu"),    EInputActionValueType::Boolean);

	// Instruction mode (builder pawn).
	CursorMove   = HueInputPrivate::MakeAction(this, TEXT("IA_CursorMove"),   EInputActionValueType::Axis2D);
	Secondary    = HueInputPrivate::MakeAction(this, TEXT("IA_Secondary"),    EInputActionValueType::Boolean);
	NextBoard    = HueInputPrivate::MakeAction(this, TEXT("IA_NextBoard"),    EInputActionValueType::Boolean);
	PrevBoard    = HueInputPrivate::MakeAction(this, TEXT("IA_PrevBoard"),    EInputActionValueType::Boolean);

	// The pause key must still fire while the game is paused, otherwise the menu could never be closed.
	PauseMenu->bTriggerWhenPaused = true;

	// ---- Build view (top-down cursor) -------------------------------------------------------------------------
	UInputMappingContext* Build = NewObject<UInputMappingContext>(this, TEXT("IMC_HueBuild"));

	// CursorMove (Axis2D: X = screen right, Y = screen up). Same swizzle / negate scheme as Move, keys + arrows.
	for (const FKey& Key : { EKeys::D, EKeys::Right })
	{
		HueInputPrivate::Map(Build, CursorMove, Key);
	}
	for (const FKey& Key : { EKeys::A, EKeys::Left })
	{
		HueInputPrivate::Map(Build, CursorMove, Key, { HueInputPrivate::MakeNegate(Build) });
	}
	for (const FKey& Key : { EKeys::W, EKeys::Up })
	{
		HueInputPrivate::Map(Build, CursorMove, Key, { HueInputPrivate::MakeSwizzleYXZ(Build) });
	}
	for (const FKey& Key : { EKeys::S, EKeys::Down })
	{
		HueInputPrivate::Map(Build, CursorMove, Key, { HueInputPrivate::MakeSwizzleYXZ(Build), HueInputPrivate::MakeNegate(Build) });
	}

	HueInputPrivate::Map(Build, Interact, EKeys::LeftMouseButton);
	HueInputPrivate::Map(Build, Secondary, EKeys::RightMouseButton);
	HueInputPrivate::Map(Build, RotateLeft, EKeys::Q);
	HueInputPrivate::Map(Build, RotateRight, EKeys::E);
	HueInputPrivate::Map(Build, DelayDown, EKeys::Z);
	HueInputPrivate::Map(Build, DelayUp, EKeys::X);
	HueInputPrivate::Map(Build, HotbarScroll, EKeys::MouseWheelAxis);
	HueInputPrivate::MapHotbarSlots(Build, HotbarSlot);
	HueInputPrivate::Map(Build, PrevBoard, EKeys::LeftBracket);
	HueInputPrivate::MapRunAndMenuKeys(Build, *this);
	BuildContext = Build;

	// ---- Watch view (side camera): only run control, next exercise, help and pause -----------------------------
	UInputMappingContext* Watch = NewObject<UInputMappingContext>(this, TEXT("IMC_HueWatch"));
	HueInputPrivate::MapRunAndMenuKeys(Watch, *this);
	WatchContext = Watch;

	// ---- Sandbox / first person (unchanged) ----------------------------------------------------------------------
	UInputMappingContext* Context = NewObject<UInputMappingContext>(this, TEXT("IMC_Huebox"));

	// Move (Axis2D: X = right, Y = forward). A boolean key yields (1,0,0); swizzle moves it onto Y.
	HueInputPrivate::Map(Context, Move, EKeys::W, { HueInputPrivate::MakeSwizzleYXZ(Context) });
	HueInputPrivate::Map(Context, Move, EKeys::S, { HueInputPrivate::MakeSwizzleYXZ(Context), HueInputPrivate::MakeNegate(Context) });
	HueInputPrivate::Map(Context, Move, EKeys::D);
	HueInputPrivate::Map(Context, Move, EKeys::A, { HueInputPrivate::MakeNegate(Context) });

	// Look: mouse up is +Y, pitch input up is negative -> invert Y only.
	HueInputPrivate::Map(Context, Look, EKeys::Mouse2D, { HueInputPrivate::MakeNegate(Context, false, true, false) });

	HueInputPrivate::Map(Context, Interact, EKeys::LeftMouseButton);
	HueInputPrivate::Map(Context, RotateLeft, EKeys::Q);
	HueInputPrivate::Map(Context, RotateRight, EKeys::E);
	HueInputPrivate::Map(Context, DelayDown, EKeys::Z);
	HueInputPrivate::Map(Context, DelayUp, EKeys::X);
	HueInputPrivate::Map(Context, HotbarScroll, EKeys::MouseWheelAxis);
	HueInputPrivate::MapHotbarSlots(Context, HotbarSlot);

	HueInputPrivate::Map(Context, Drop, EKeys::G);
	HueInputPrivate::Map(Context, RunToggle, EKeys::SpaceBar);
	HueInputPrivate::Map(Context, ResetRun, EKeys::R);
	HueInputPrivate::Map(Context, ToggleHelp, EKeys::H);
	HueInputPrivate::Map(Context, PauseMenu, EKeys::Escape);
	HueInputPrivate::Map(Context, PauseMenu, EKeys::P);

	MappingContext = Context;

	UE_LOG(LogHuebox, Verbose, TEXT("HueInputConfig: built %d / %d / %d key mappings (sandbox / build / watch)"),
		Context->GetMappings().Num(), Build->GetMappings().Num(), Watch->GetMappings().Num());
}
