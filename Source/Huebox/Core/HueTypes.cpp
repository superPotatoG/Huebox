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

	constexpr int32 NumColors = 7;

	/** Colour index in [0, 6]; anything invalid is treated as N (the neutral colour). */
	static int32 ColorIndex(EHueColor Color)
	{
		const int32 Index = static_cast<int32>(Color);
		return (Index >= 0 && Index < NumColors) ? Index : static_cast<int32>(EHueColor::N);
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
	// Order: A, B, N, G, Y, M, V. A leans red (not amber) so it never reads as Y; the letter label always
	// accompanies the colour anyway (red / green are hard to tell apart for the common colour-vision deficiencies).
	static const FLinearColor Colors[HueTypesPrivate::NumColors] =
	{
		FLinearColor(1.00f, 0.16f, 0.08f),	// A red
		FLinearColor(0.04f, 0.72f, 1.00f),	// B cyan
		FLinearColor(0.92f, 0.92f, 0.92f),	// N white
		FLinearColor(0.10f, 0.85f, 0.20f),	// G green
		FLinearColor(1.00f, 0.86f, 0.08f),	// Y yellow
		FLinearColor(0.95f, 0.18f, 0.85f),	// M magenta
		FLinearColor(0.32f, 0.30f, 1.00f)	// V violet
	};
	return Colors[HueTypesPrivate::ColorIndex(Color)];
}

const TCHAR* HueColor::Label(EHueColor Color)
{
	// The colour's own initial (enum order A, B, N, G, Y, M, V). Until 11-01 these were the GDD letters A / B / N,
	// and "B" for cyan read as blue to players.
	static const TCHAR* const Labels[HueTypesPrivate::NumColors] =
		{ TEXT("R"), TEXT("C"), TEXT("W"), TEXT("G"), TEXT("Y"), TEXT("M"), TEXT("B") };
	return Labels[HueTypesPrivate::ColorIndex(Color)];
}

const TCHAR* HueColor::Name(EHueColor Color)
{
	static const TCHAR* const Names[HueTypesPrivate::NumColors] =
		{ TEXT("red"), TEXT("cyan"), TEXT("white"), TEXT("green"), TEXT("yellow"), TEXT("magenta"), TEXT("blue") };
	return Names[HueTypesPrivate::ColorIndex(Color)];
}

uint8 HueColor::ToMask(EHueColor Color)
{
	static const uint8 Masks[HueTypesPrivate::NumColors] =
	{
		Red,				// A
		Green | Blue,		// B
		Red | Green | Blue,	// N
		Green,				// G
		Red | Green,		// Y
		Red | Blue,			// M
		Blue				// V
	};
	return Masks[HueTypesPrivate::ColorIndex(Color)];
}

bool HueColor::FromMask(uint8 Mask, EHueColor& OutColor)
{
	const uint8 Primaries = Mask & (Red | Green | Blue);
	for (int32 Index = 0; Primaries != 0 && Index < HueTypesPrivate::NumColors; ++Index)
	{
		const EHueColor Candidate = static_cast<EHueColor>(Index);
		if (ToMask(Candidate) == Primaries)
		{
			OutColor = Candidate;
			return true;
		}
	}
	return false;
}

FString HueColor::DescribePrimaries(uint8 Mask)
{
	TArray<FString, TInlineAllocator<3>> Parts;
	if (Mask & Red)
	{
		Parts.Add(TEXT("red"));
	}
	if (Mask & Green)
	{
		Parts.Add(TEXT("green"));
	}
	if (Mask & Blue)
	{
		Parts.Add(TEXT("blue"));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(" + ")) : FString(TEXT("no light"));
}

FString HueColor::SlotsOfMask(uint8 Mask)
{
	return FString::Printf(TEXT("[%s%s%s]"),
		(Mask & Red) ? TEXT("R") : TEXT("-"),
		(Mask & Green) ? TEXT("G") : TEXT("-"),
		(Mask & Blue) ? TEXT("B") : TEXT("-"));
}

FString HueColor::Slots(EHueColor Color)
{
	return SlotsOfMask(ToMask(Color));
}

FString HueColor::LabelWithSlots(EHueColor Color)
{
	return FString(Label(Color)) + Slots(Color);
}

const TCHAR* HueColor::MotionMeaning(EHueColor Color)
{
	// The partner (the rig's anchor) is red: same colour repels, a different motion colour attracts.
	switch (Color)
	{
	case EHueColor::A: return TEXT("same colour as her red partner - pushed away");
	case EHueColor::B: return TEXT("not her partner's colour - pulled back");
	case EHueColor::N: return TEXT("red + cyan at once - push and pull cancel: brakes");
	default:           return TEXT("a key - she does its move");
	}
}

bool HueColor::Mix(EHueColor First, EHueColor Second, EHueColor& OutMixed)
{
	const uint8 FirstMask = ToMask(First);
	const uint8 SecondMask = ToMask(Second);
	if ((FirstMask & SecondMask) != 0)
	{
		return false;
	}
	return FromMask(FirstMask | SecondMask, OutMixed);
}

EHueColor HueColor::Shift(EHueColor Color)
{
	// White has no complement (that would be no light at all), so N passes unchanged.
	EHueColor Complement = EHueColor::N;
	return FromMask(ToMask(Color) ^ (Red | Green | Blue), Complement) ? Complement : EHueColor::N;
}

bool HueColor::DrivesRig(EHueColor Color)
{
	return Color == EHueColor::A || Color == EHueColor::B || Color == EHueColor::N;
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
	// "2D white props": every module is a white tile told apart by its dark label (TURN, SHIFT, D 1.25s, MIX)
	// and port markers, so colour on the grid always means a signal colour (sources, receivers, pulses).
	static const FLinearColor ModuleWhite(0.92f, 0.92f, 0.90f);
	static const FLinearColor Colors[5] =
	{
		FLinearColor(0.50f, 0.50f, 0.50f),	// None
		ModuleWhite,						// Turn
		ModuleWhite,						// Colour shift
		ModuleWhite,						// Delay
		ModuleWhite							// Mixer
	};
	return Colors[HueTypesPrivate::ModuleIndex(Type)];
}

FLinearColor HueModule::AccentColor(EHueModuleType Type)
{
	// The pre-2D body colours: the Sandbox supply (floor pickups, hotbar) keeps telling the types apart by colour.
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
