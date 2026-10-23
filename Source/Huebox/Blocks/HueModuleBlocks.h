#pragma once

#include "CoreMinimal.h"
#include "Blocks/PulseBlock.h"
#include "HueModuleBlocks.generated.h"

/**
 * Turn block: a 90-degree corner with two ports on adjacent faces: Facing and RotatePlus90(Facing).
 * A pulse entering through one port leaves through the other; the two remaining faces block entry.
 * (Bidirectional so that the four rotations cover both left and right turns with only Q/E.)
 */
UCLASS()
class HUEBOX_API AHueTurnBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueTurnBlock();

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;

	/** Pure port rule, exposed for tests: returns true and the exit direction if TravelDir may enter. */
	bool TryTurn(EHueDir TravelDir, EHueDir& OutExitDir) const;
};

/** Colour-shift block: one-way along Facing. Turns a colour into its complement (A <-> B, G <-> M, Y <-> V), passes N
 *  unchanged, preserves direction. */
UCLASS()
class HUEBOX_API AHueColorShiftBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueColorShiftBlock();

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
};

/**
 * Delay block: one-way along Facing. Stores one pulse and releases it (same colour and direction) after
 * DelaySeconds (0..6 s in 0.25 s steps). While it holds a pulse, new pulses are rejected (occupied).
 */
UCLASS()
class HUEBOX_API AHueDelayBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueDelayBlock();

	static constexpr float MinDelay = 0.f;
	static constexpr float MaxDelay = 6.f;
	static constexpr float DelayStep = 0.25f;

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual void OnScheduledEvent(int32 EventTag, UPulseSubsystem& Sim) override;
	virtual void ResetBlock() override;
	virtual int32 GetHeldSignalCount() const override { return bHolding ? 1 : 0; }
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
	virtual void RefreshVisuals() override;

	float GetDelaySeconds() const { return DelaySeconds; }
	/** Snaps to 0.25 s steps and clamps to [0, 6]. */
	void SetDelaySeconds(float NewDelay);
	/** +/- Steps * 0.25 s (Z / X keys). */
	void AdjustDelay(int32 Steps);
	bool IsHolding() const { return bHolding; }

protected:
	/** Snaps an editor-entered delay to the 0.25 s grid. */
	virtual void OnConstruction(const FTransform& Transform) override;

	/** Newly placed delays start at 0 s so the player has to tune them (Z / X). */
	UPROPERTY(EditAnywhere, Category = "Hue|Delay", meta = (ClampMin = "0", ClampMax = "6", UIMin = "0", UIMax = "6"))
	float DelaySeconds = 0.0f;

	bool bHolding = false;
	FPulseData HeldPulse;
	int64 ReleaseTimeMs = 0;
	/** Incremented on every store/reset so stale release events are ignored. */
	int32 EventSerial = 0;
};

/**
 * Mixer block: output leaves through Facing. Inputs are the two side faces, RotatePlus90(Facing) (input 0) and
 * RotateMinus90(Facing) (input 1). Two pulses arriving at different inputs within the mixing window (inclusive) are
 * consumed and, if their colours share no primary, one pulse of the added light is emitted (HueColor::Mix: A + B = N,
 * A + G = Y, ...). Colours sharing a primary (matching colours, anything with N, A + Y, ...) or a timeout consume the
 * inputs and report the cause. A second pulse on the already-waiting input is rejected.
 */
UCLASS()
class HUEBOX_API AHueMixerBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueMixerBlock();

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual void OnScheduledEvent(int32 EventTag, UPulseSubsystem& Sim) override;
	virtual void ResetBlock() override;
	virtual int32 GetHeldSignalCount() const override { return bWaiting ? 1 : 0; }
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
	virtual void RefreshVisuals() override;

	/** Returns 0 or 1 for a valid input, INDEX_NONE if TravelDir enters through a non-input face. */
	int32 GetInputIndexForTravel(EHueDir TravelDir) const;
	bool IsWaiting() const { return bWaiting; }
	const FString& GetLastFailure() const { return LastFailure; }

protected:
	bool bWaiting = false;
	int32 WaitingInput = INDEX_NONE;
	EHueColor WaitingColor = EHueColor::A;
	int64 WaitingSinceMs = 0;
	int32 WaitingOps = 0;
	int32 EventSerial = 0;

	/** Arrival time labels per input for the HUD (GDD: identify which route needs adjustment). */
	int64 LastArrivalMs[2] = { -1, -1 };
	EHueColor LastArrivalColor[2] = { EHueColor::A, EHueColor::A };
	FString LastFailure;
	/** Logical time of LastFailure (-1 = none this attempt). */
	int64 LastFailureMs = -1;

	void ClearWaiting();

private:
	/** Board-local face of input 0 / 1 for the current Facing. */
	EHueDir GetInputFace(int32 Input) const;
	/** First input: store it and schedule the window timeout (priority 2, tag = ++EventSerial). */
	void StartWaiting(int32 Input, const FPulseData& Pulse, int64 NowMs, UPulseSubsystem& Sim);
	/** Records + reports a failed mix (Warning, flash, LastFailure). Waiting state must already be cleared. */
	void ReportMixFailure(UPulseSubsystem& Sim, const FString& Reason, const FString& Detail, EHueDir Face);
	/** "in0 A @2.00s" or "in0 -" */
	FString DescribeArrival(int32 Input) const;
};
