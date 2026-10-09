#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/HueTypes.h"
#include "PulseSubsystem.generated.h"

class AHueBoard;
class APulseBlock;
class AHuePulseVisual;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHueFeedback, const FHueFeedback&, Feedback);

/**
 * Deterministic pulse simulation for the board that is currently running.
 *
 * - Logical time is integer milliseconds. A pulse needs StepMs (250 ms at 4 cells/s) per cell.
 * - All work is driven by a min-heap of timed events (pulse arrivals, block events such as delay release and
 *   mixer timeout). Events with the same time form one group; after a group, touched blocks get
 *   OnTimeGroupEnd() so the receiver can detect simultaneous conflicting inputs.
 * - Ordering inside a group: arrivals (priority 0) before block events (priority 1+), then insertion order.
 *   This makes the mixer window inclusive: an input arriving exactly at the timeout instant still mixes.
 * - Visual pulses are interpolated on the same timeline but never decide whether a logical hit happened.
 * - Limits (GDD): at most MaxActiveSignals live signals (travelling + held in blocks), MaxOpsPerSignal module
 *   operations per signal, then the signal is terminated with feedback.
 *
 * Lifetime: a world subsystem lives exactly as long as its UWorld. Deinitialize() cancels every pending event
 * so nothing can call back into a removed actor or leak into the next attempt.
 */
UCLASS()
class HUEBOX_API UPulseSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// UWorldSubsystem / FTickableGameObject
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	static UPulseSubsystem* Get(const UObject* WorldContext);

	// ---- Run control (used by AExhibitManager) -----------------------------------------------------------

	/** Clears any previous run and emits one pulse from every source on Board at t = 0. */
	bool StartRun(AHueBoard* Board);
	void SetPaused(bool bInPaused);
	/** Cancels all pending events, removes all pulses and visuals. Does not reset blocks or rig. */
	void StopAndClear();

	bool IsRunning() const { return bRunning; }
	bool IsPaused() const { return bPaused; }
	AHueBoard* GetActiveBoard() const { return ActiveBoard.Get(); }

	/** Current logical time of the active run. */
	int64 GetSimTimeMs() const { return SimTimeMs; }
	double GetSimTimeSeconds() const { return SimTimeExactMs / 1000.0; }
	int32 GetTravelingPulseCount() const { return Pulses.Num(); }
	int32 GetPendingEventCount() const { return EventHeap.Num(); }
	/** Travelling pulses + signals held inside blocks. */
	int32 GetActiveSignalCount() const;

	/** Advances logical time by Seconds and processes due events. Used by Tick and by automation tests. */
	void StepSimulation(double Seconds);

	/** When false (tests), no pulse visual actors are spawned. */
	void SetVisualsEnabled(bool bEnabled) { bVisualsEnabled = bEnabled; }

	// ---- API for blocks while handling pulses ----------------------------------------------------------------

	/**
	 * Launches a pulse that leaves FromCell at DepartTimeMs travelling Dir (arrives at the neighbour cell
	 * StepMs later). Fails (returns INDEX_NONE) with feedback when the active signal cap is reached.
	 */
	int32 EmitPulse(EHueColor Color, FIntPoint FromCell, EHueDir Dir, int64 DepartTimeMs, int32 RemainingOps);

	/** Calls Block->OnScheduledEvent(Tag) at TimeMs. Priority >= 1 (arrivals at the same time run first). */
	void ScheduleBlockEvent(APulseBlock* Block, int64 TimeMs, int32 Tag, uint8 Priority = 1);

	/** Raises a feedback line (broadcast through OnFeedback, logged). */
	void Report(EHueFeedbackKind Kind, const FString& Message, FIntPoint Cell = FIntPoint(INDEX_NONE, INDEX_NONE));

	int64 GetStepMs() const { return StepMs; }
	int64 GetMixWindowMs() const { return MixWindowMs; }
	int32 GetMaxOpsPerSignal() const { return MaxOpsPerSignal; }

	/** Board-local cell -> world position of a pulse at that cell (helper for visuals). */
	FVector CellToWorld(FIntPoint Cell) const;

	UPROPERTY(BlueprintAssignable, Category = "Hue|Pulse")
	FOnHueFeedback OnFeedback;

	/** Every feedback line raised since the last StartRun (tests / debugging). */
	const TArray<FHueFeedback>& GetRunFeedbackLog() const { return RunFeedbackLog; }
	/** True if any feedback line of this run contains Substring (case-insensitive). */
	bool HasFeedbackContaining(const FString& Substring) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	enum class EEventType : uint8
	{
		PulseArrive,
		BlockEvent
	};

	/** A queued simulation event. Block reference is weak: a removed block simply never receives it. */
	struct FSimEvent
	{
		int64 TimeMs = 0;
		uint8 Priority = 0;
		uint64 Seq = 0;
		EEventType Type = EEventType::PulseArrive;
		int32 PulseId = INDEX_NONE;
		int32 Tag = 0;
		TWeakObjectPtr<APulseBlock> Block;
	};

	struct FSimEventOrder
	{
		/** TArray heap is a min-heap on this predicate. */
		bool operator()(const FSimEvent& A, const FSimEvent& B) const
		{
			if (A.TimeMs != B.TimeMs) { return A.TimeMs < B.TimeMs; }
			if (A.Priority != B.Priority) { return A.Priority < B.Priority; }
			return A.Seq < B.Seq;
		}
	};

	/** A pulse in flight between two cells. Pure value type. */
	struct FTravelingPulse
	{
		FPulseData Data;
		FIntPoint FromCell = FIntPoint::ZeroValue;
		int64 DepartTimeMs = 0;
	};

	void PushEvent(FSimEvent&& Event);
	void ProcessDueEvents(int64 UntilMs);
	void DispatchEvent(const FSimEvent& Event);
	void HandleArrival(int32 PulseId);
	/** Moves the pulse one cell further in its Direction, departing at its current arrival time. */
	void ContinuePulse(FTravelingPulse& Pulse);
	void RemovePulse(int32 PulseId);
	/** Calls OnTimeGroupEnd once per valid block touched in the group that just finished. */
	void FlushTouchedBlocks(uint32 Serial);
	/** Drops every queued event, travelling pulse and touched-block entry (no visuals, no flags). */
	void CancelAllEvents();

	// Visual pool -------------------------------------------------------------------------------------------
	AHuePulseVisual* AcquireVisual();
	void ReleaseVisual(int32 PulseId);
	/** Hides a visual and returns it to FreeVisuals (destroys it if the pool is already full). */
	void RecycleVisual(AHuePulseVisual* Visual);
	void UpdateVisuals();

	/** Guard against zero-time loops: max events dispatched by one ProcessDueEvents call. */
	static constexpr int32 MaxEventsPerCall = 10000;
	static constexpr int32 MaxRunFeedbackLines = 256;
	/** More than the 16-signal cap needs; extra visuals are destroyed instead of pooled. */
	static constexpr int32 MaxPooledVisuals = 32;
	/** Frame clamp shared with the rig physics so logical time and carriage motion stay in step. */
	static constexpr double MaxFrameSeconds = 1.0 / 30.0;
	/** Pulse visuals float this high above the board surface (cm). */
	static constexpr double VisualHeightCm = 45.0;
	/** Absorbs float accumulation error (999.9999999 ms must count as 1000 ms). */
	static constexpr double TimeEpsilonMs = 1.0e-6;

	/**
	 * Incremented by StopAndClear (and therefore StartRun). Event processing captures it and stops as soon as it
	 * changes, so a run restarted from inside a callback never mixes events of two runs.
	 */
	uint32 RunSerial = 0;

	TWeakObjectPtr<AHueBoard> ActiveBoard;

	TArray<FHueFeedback> RunFeedbackLog;

	TArray<FSimEvent> EventHeap;
	TMap<int32, FTravelingPulse> Pulses;
	/** Blocks that received a pulse or event during the current time group. */
	TArray<TWeakObjectPtr<APulseBlock>, TInlineAllocator<8>> TouchedThisGroup;

	/** Visual actors for live pulses, keyed by pulse id. Strong refs: the subsystem owns its pooled actors. */
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AHuePulseVisual>> ActiveVisuals;

	/** Hidden, reusable visuals (object pool: avoids spawning/destroying an actor per pulse). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AHuePulseVisual>> FreeVisuals;

	int64 SimTimeMs = 0;
	double SimTimeExactMs = 0.0;
	int64 StepMs = 250;
	int64 MixWindowMs = 500;
	int32 MaxActiveSignals = 16;
	int32 MaxOpsPerSignal = 64;
	int32 NextPulseId = 1;
	uint64 NextSeq = 1;
	bool bRunning = false;
	bool bPaused = false;
	bool bVisualsEnabled = true;
	bool bProcessing = false;
};
