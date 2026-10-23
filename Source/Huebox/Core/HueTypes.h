#pragma once

#include "CoreMinimal.h"
#include "HueTypes.generated.h"

class AHueBoard;

DECLARE_LOG_CATEGORY_EXTERN(LogHuebox, Log, All);

/**
 * Signal colour: additive light, i.e. a set of the primaries red, green and blue (HueColor::ToMask).
 *  Motion signals - the only colours the dancer reacts to: A = red (repels), B = cyan = green + blue (attracts),
 *  N = white = all three (brakes).
 *  Key colours - no effect of their own, they act on the blocks they hit (a colour brick breaks only for its colour):
 *  G = green, Y = yellow = red + green, M = magenta = red + blue, V = violet (blue).
 * The set is closed under mixing (disjoint primaries add up) and colour shift (complement), so every pulse a board
 * can produce has a letter. Enum order is saved data: append new values at the end.
 */
UENUM(BlueprintType)
enum class EHueColor : uint8
{
	A,
	B,
	N,
	G,
	Y,
	M,
	V
};

/**
 * Board-local grid direction. East = +X, North = +Y.
 * Enum order matters: index + 1 is a +90 degree yaw turn (East -> North -> West -> South).
 */
UENUM(BlueprintType)
enum class EHueDir : uint8
{
	East,
	North,
	West,
	South
};

/** Placeable module kinds that can live in the player's inventory. */
UENUM(BlueprintType)
enum class EHueModuleType : uint8
{
	None,
	Turn,
	ColorShift,
	Delay,
	Mixer
};

/** Exhibit / attempt state machine (GDD: Build, Run, Paused, Complete + Ended for failed/timed-out runs). */
UENUM(BlueprintType)
enum class EHueRunState : uint8
{
	Build,
	Running,
	Paused,
	Ended,
	Complete
};

/**
 * How the level is played.
 *  Instruction: top-down build view locked to the focused board (AHueBuilderPawn) + side "watch" view while a run
 *               plays; modules come from each board's tray.
 *  Sandbox:     free first-person maker (AHueboxCharacter) with a personal inventory and floor pickups.
 */
UENUM(BlueprintType)
enum class EHuePlayMode : uint8
{
	Instruction,
	Sandbox
};

/** Builder pawn view: placing modules from above, or watching the performance from the side. */
UENUM(BlueprintType)
enum class EHueViewMode : uint8
{
	Build,
	Watch
};

UENUM(BlueprintType)
enum class EHueFeedbackKind : uint8
{
	Info,
	Success,
	Warning,
	Error
};

/** Result of a block handling a pulse that entered its cell. */
enum class EPulseResponse : uint8
{
	/** The (possibly modified) pulse keeps travelling from this cell in Pulse.Direction. */
	Continue,
	/** The pulse ends here (blocked, absorbed, stored or merged). */
	Consumed
};

/**
 * One finite light pulse. Plain value data: copied freely, never owns UObjects, invisible to the GC.
 * Times are integer milliseconds of *logical* simulation time so simultaneous arrivals compare exactly.
 */
USTRUCT(BlueprintType)
struct FPulseData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	int32 PulseId = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	EHueColor Color = EHueColor::B;

	/** Cell the pulse arrives at (or currently occupies). */
	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	FIntPoint Cell = FIntPoint::ZeroValue;

	/** Travel direction. When a block receives a pulse this is the direction it was moving when it entered. */
	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	EHueDir Direction = EHueDir::East;

	/** Logical time (ms) at which the pulse arrives at Cell. */
	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	int64 ArrivalTimeMs = 0;

	/** Module operations left before the pulse is terminated as an invalid cycle. */
	UPROPERTY(BlueprintReadOnly, Category = "Pulse")
	int32 RemainingOps = 64;
};

/** A player-facing feedback line (blocked port, mixer failure, objective reached, ...). */
USTRUCT(BlueprintType)
struct FHueFeedback
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Feedback")
	EHueFeedbackKind Kind = EHueFeedbackKind::Info;

	UPROPERTY(BlueprintReadOnly, Category = "Feedback")
	FString Message;

	/** Grid cell the message refers to, if any. */
	UPROPERTY(BlueprintReadOnly, Category = "Feedback")
	FIntPoint Cell = FIntPoint(INDEX_NONE, INDEX_NONE);

	/** Board the message refers to. Weak: a feedback line must never keep a board alive. */
	UPROPERTY(BlueprintReadOnly, Category = "Feedback")
	TWeakObjectPtr<AHueBoard> Board;

	/** World real time (seconds) when it was raised; used by the HUD to fade lines out. */
	UPROPERTY(BlueprintReadOnly, Category = "Feedback")
	double RealTimeSeconds = 0.0;
};

UENUM(BlueprintType)
enum class EHueObjectiveType : uint8
{
	/** The receiver must currently hold Color. */
	ReceiverColor,
	/** Carriage track fraction >= TrackFraction (it left the start area). */
	LeaveStart,
	/** Carriage track fraction >= TrackFraction (far trigger region). */
	PassFarMarker,
	/** Carriage track fraction <= TrackFraction (came back towards the anchor). */
	ReturnBelow,
	/** Inside [ZoneMin, ZoneMax], slower than MaxSpeedFraction * TrackLength, for HoldSeconds. */
	StopInZone
};

/** One ordered objective. Only the current (first incomplete) objective can be completed. */
USTRUCT(BlueprintType)
struct FHueObjective
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective")
	EHueObjectiveType Type = EHueObjectiveType::LeaveStart;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective")
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective")
	EHueColor Color = EHueColor::A;

	/** 0 = anchor end of the track, 1 = far end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective", meta = (ClampMin = "0", ClampMax = "1"))
	float TrackFraction = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective", meta = (ClampMin = "0", ClampMax = "1"))
	float ZoneMin = 0.425f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective", meta = (ClampMin = "0", ClampMax = "1"))
	float ZoneMax = 0.575f;

	/** GDD: maximum stopping speed = 2% of track length per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective")
	float MaxSpeedFraction = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Objective")
	float HoldSeconds = 1.0f;
};

/** One timed colour change applied to the rig (receiver history / motion prediction input). */
struct FHueColorEvent
{
	float TimeSeconds = 0.f;
	EHueColor Color = EHueColor::N;
};

/** Where a live pulse is right now, for the minimap. CellPosition is fractional board-local cell coordinates:
 *  (x, y) = centre of cell (x, y); a pulse halfway from (1,2) to (2,2) is at (1.5, 2). */
struct FHuePulseSnapshot
{
	FVector2D CellPosition = FVector2D::ZeroVector;
	EHueColor Color = EHueColor::B;
};

/** Snapshot of the reactive rig used to evaluate objectives (kept separate so objective logic is unit-testable). */
struct FHueRigSample
{
	/** 0..1 along the track, 0 at the anchor. */
	float TrackFraction = 0.f;
	/** Signed speed along the track as a fraction of track length per second (+ = away from anchor). */
	float SpeedFraction = 0.f;
	EHueColor ReceiverColor = EHueColor::N;
};

namespace HueDir
{
	HUEBOX_API FIntPoint ToOffset(EHueDir Dir);
	HUEBOX_API EHueDir Opposite(EHueDir Dir);
	/** index + 1: East -> North -> West -> South -> East (yaw + 90). */
	HUEBOX_API EHueDir RotatePlus90(EHueDir Dir);
	/** index - 1: East -> South -> West -> North -> East (yaw - 90). */
	HUEBOX_API EHueDir RotateMinus90(EHueDir Dir);
	/** Rotate by Steps quarter turns (positive = +90 each, negative allowed). */
	HUEBOX_API EHueDir Rotate(EHueDir Dir, int32 Steps);
	/** Board-local yaw in degrees: East 0, North 90, West 180, South 270. */
	HUEBOX_API float ToYaw(EHueDir Dir);
	/** Unit vector in board-local space (X = East, Y = North). */
	HUEBOX_API FVector ToVector(EHueDir Dir);
	HUEBOX_API const TCHAR* ToString(EHueDir Dir);
}

namespace HueColor
{
	/** Primary bits of HueColor::ToMask. */
	constexpr uint8 Red = 1;
	constexpr uint8 Green = 2;
	constexpr uint8 Blue = 4;

	/** Display colour (red / cyan / white / green / yellow / magenta / violet). */
	HUEBOX_API FLinearColor ToLinear(EHueColor Color);
	/** "A", "B", "N", "G", "Y", "M" or "V" - letters always accompany colour (accessibility). */
	HUEBOX_API const TCHAR* Label(EHueColor Color);
	/** Plain name for rule text: "red", "cyan", "white", ... */
	HUEBOX_API const TCHAR* Name(EHueColor Color);
	/** Primaries of Color as Red | Green | Blue bits (never 0). */
	HUEBOX_API uint8 ToMask(EHueColor Color);
	/** Colour holding exactly the primaries in Mask; false for 0 (no light). */
	HUEBOX_API bool FromMask(uint8 Mask, EHueColor& OutColor);
	/** "red", "green + blue", ... for a primary mask (mixer failure text). */
	HUEBOX_API FString DescribePrimaries(uint8 Mask);
	/**
	 * Mixer rule (additive light): two colours without a primary in common add up (A + B = N, A + G = Y, G + V = B,
	 * Y + V = N, ...). Returns false when they share a primary (matching colours, anything with N, A + Y, ...).
	 */
	HUEBOX_API bool Mix(EHueColor First, EHueColor Second, EHueColor& OutMixed);
	/** Colour-shift rule: the complementary colour (A <-> B, G <-> M, Y <-> V); N passes unchanged. */
	HUEBOX_API EHueColor Shift(EHueColor Color);
	/** A, B and N move the dancer; every other colour is a key that only acts on the blocks it hits. */
	HUEBOX_API bool DrivesRig(EHueColor Color);
}

namespace HueModule
{
	HUEBOX_API const TCHAR* DisplayName(EHueModuleType Type);
	/** Short label used on hotbar / pickups, e.g. "TURN". */
	HUEBOX_API const TCHAR* ShortName(EHueModuleType Type);
	/** Body colour used for the module's placeholder mesh. */
	HUEBOX_API FLinearColor BodyColor(EHueModuleType Type);
	/** Per-type accent (gray Turn, violet Colour shift, tan Delay, teal Mixer) for the Sandbox supply: floor pickups
	 *  and hotbar swatches, where the type must read at a glance without a label. */
	HUEBOX_API FLinearColor AccentColor(EHueModuleType Type);
}

namespace HueTime
{
	inline int64 SecondsToMs(double Seconds) { return FMath::RoundToInt64(Seconds * 1000.0); }
	inline double MsToSeconds(int64 Ms) { return static_cast<double>(Ms) / 1000.0; }
}
