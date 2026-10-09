#include "Core/HueTypes.h"

// LogHuebox is defined once in Huebox.cpp (DEFINE_LOG_CATEGORY); this file only implements the helpers.

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit, and
// identically named helpers in anonymous namespaces of different files would then collide.
namespace HueTypesPrivate
{
	/** Direction index wrapped into [0, 3]; robust against out-of-range values read from serialized data. */
	static int32 DirIndex(EHueDir Dir)
	{
		return static_cast<int32>(Dir) & 3;
	}

	static EHueDir DirFromIndex(int32 Index)
	{
		return static_cast<EHueDir>(((Index % 4) + 4) % 4);
	}

	/** Colour index in [0, 2]; anything invalid is treated as N (the neutral colour). */
	static int32 ColorIndex(EHueColor Color)
	{
		const int32 Index = static_cast<int32>(Color);
		return (Index >= 0 && Index <= 2) ? Index : 2;
	}

	/** Module index in [0, 4]; anything invalid is treated as None. */
	static int32 ModuleIndex(EHueModuleType Type)
	{
		const int32 Index = static_cast<int32>(Type);
		return (Index >= 0 && Index <= 4) ? Index : 0;
	}
}

// ---- HueDir ---------------------------------------------------------------------------------------------------

FIntPoint HueDir::ToOffset(EHueDir Dir)
{
	// Order: East, North, West, South (board-local: East = +X, North = +Y).
	static const FIntPoint Offsets[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };
	return Offsets[HueTypesPrivate::DirIndex(Dir)];
}

EHueDir HueDir::Opposite(EHueDir Dir)
{
	return HueTypesPrivate::DirFromIndex(HueTypesPrivate::DirIndex(Dir) + 2);
}

EHueDir HueDir::RotatePlus90(EHueDir Dir)
{
	return HueTypesPrivate::DirFromIndex(HueTypesPrivate::DirIndex(Dir) + 1);
}

EHueDir HueDir::RotateMinus90(EHueDir Dir)
{
	return HueTypesPrivate::DirFromIndex(HueTypesPrivate::DirIndex(Dir) + 3);
}

EHueDir HueDir::Rotate(EHueDir Dir, int32 Steps)
{
	// Reduce first so very large |Steps| cannot overflow the addition.
	return HueTypesPrivate::DirFromIndex(HueTypesPrivate::DirIndex(Dir) + (Steps % 4));
}

float HueDir::ToYaw(EHueDir Dir)
{
	return 90.f * static_cast<float>(HueTypesPrivate::DirIndex(Dir));
}

FVector HueDir::ToVector(EHueDir Dir)
{
	const FIntPoint Offset = ToOffset(Dir);
	return FVector(static_cast<double>(Offset.X), static_cast<double>(Offset.Y), 0.0);
}

const TCHAR* HueDir::ToString(EHueDir Dir)
{
	static const TCHAR* const Names[4] = { TEXT("East"), TEXT("North"), TEXT("West"), TEXT("South") };
	return Names[HueTypesPrivate::DirIndex(Dir)];
}

// ---- HueColor -------------------------------------------------------------------------------------------------

FLinearColor HueColor::ToLinear(EHueColor Color)
{
	// A = amber, B = cyan, N = white. Amber/cyan stay distinguishable for the common colour-vision deficiencies,
	// and the letter label always accompanies the colour anyway.
	static const FLinearColor Colors[3] =
	{
		FLinearColor(1.00f, 0.50f, 0.04f),
		FLinearColor(0.04f, 0.72f, 1.00f),
		FLinearColor(0.92f, 0.92f, 0.92f)
	};
	return Colors[HueTypesPrivate::ColorIndex(Color)];
}

const TCHAR* HueColor::Label(EHueColor Color)
{
	static const TCHAR* const Labels[3] = { TEXT("A"), TEXT("B"), TEXT("N") };
	return Labels[HueTypesPrivate::ColorIndex(Color)];
}

EHueColor HueColor::Shift(EHueColor Color)
{
	switch (Color)
	{
	case EHueColor::A: return EHueColor::B;
	case EHueColor::B: return EHueColor::A;
	default:           return EHueColor::N;
	}
}

// ---- HueModule ------------------------------------------------------------------------------------------------

const TCHAR* HueModule::DisplayName(EHueModuleType Type)
{
	static const TCHAR* const Names[5] = { TEXT("None"), TEXT("Turn"), TEXT("Colour shift"), TEXT("Delay"), TEXT("Mixer") };
	return Names[HueTypesPrivate::ModuleIndex(Type)];
}

const TCHAR* HueModule::ShortName(EHueModuleType Type)
{
	static const TCHAR* const Names[5] = { TEXT("-"), TEXT("TURN"), TEXT("SHIFT"), TEXT("DELAY"), TEXT("MIX") };
	return Names[HueTypesPrivate::ModuleIndex(Type)];
}

FLinearColor HueModule::BodyColor(EHueModuleType Type)
{
	static const FLinearColor Colors[5] =
	{
		FLinearColor(0.50f, 0.50f, 0.50f),	// None
		FLinearColor(0.55f, 0.55f, 0.55f),	// Turn: gray
		FLinearColor(0.45f, 0.30f, 0.60f),	// Colour shift: violet
		FLinearColor(0.80f, 0.70f, 0.45f),	// Delay: tan
		FLinearColor(0.15f, 0.45f, 0.45f)	// Mixer: teal
	};
	return Colors[HueTypesPrivate::ModuleIndex(Type)];
}
