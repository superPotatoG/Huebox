#pragma once

#include "CoreMinimal.h"
#include "Blocks/PulseBlock.h"
#include "HueFixedBlocks.generated.h"

class AReactiveRig;

/**
 * Source base output: emits one finite pulse of EmitColor in direction Facing when a run starts (t = 0).
 * There is no continuous beam. Fixed (not removable). Pulses that hit a source are blocked.
 */
UCLASS()
class HUEBOX_API AHueSourceBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueSourceBlock();

	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
	virtual void RefreshVisuals() override;

	EHueColor GetEmitColor() const { return EmitColor; }
	void SetEmitColor(EHueColor InColor);

	/** Called by the subsystem when it emits this source's pulse. */
	void NotifyEmitted(int64 TimeMs);

protected:
	UPROPERTY(EditAnywhere, Category = "Hue|Source")
	EHueColor EmitColor = EHueColor::B;
};

/**
 * Receiving port of the reactive mechanism. Accepts pulses through AcceptFaces only. All pulses that arrive at
 * the same logical time are grouped: if they disagree in colour the state is unchanged and a conflict is shown,
 * otherwise the colour is applied to the linked rig. Never emits another pulse.
 */
UCLASS()
class HUEBOX_API AHueReceiverBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueReceiverBlock();

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual void OnTimeGroupEnd(UPulseSubsystem& Sim) override;
	virtual void ResetBlock() override;
	virtual void GetPorts(TArray<FHuePort>& OutPorts) const override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
	virtual void RefreshVisuals() override;

	void SetAcceptFaces(const TArray<EHueDir>& Faces);
	const TArray<EHueDir>& GetAcceptFaces() const { return AcceptFaces; }

	void SetRig(AReactiveRig* InRig);
	AReactiveRig* GetRig() const { return Rig; }

	/** Colour currently held (N after reset). */
	EHueColor GetCurrentColor() const { return CurrentColor; }

	/** Every colour actually applied to the rig during this attempt (logical seconds). Cleared on reset. */
	const TArray<FHueColorEvent>& GetAppliedHistory() const { return AppliedHistory; }
	/** Number of conflicts (different colours at the same logical time) during this attempt. */
	int32 GetConflictCount() const { return ConflictCount; }

protected:
	TArray<FHueColorEvent> AppliedHistory;
	int32 ConflictCount = 0;

	/** Faces (board-local) through which pulses may enter. */
	UPROPERTY(EditAnywhere, Category = "Hue|Receiver")
	TArray<EHueDir> AcceptFaces;

	/** Required reference: the mechanism this port drives. Strong UPROPERTY (GDD: TObjectPtr for required refs). */
	UPROPERTY(EditAnywhere, Category = "Hue|Receiver")
	TObjectPtr<AReactiveRig> Rig;

	EHueColor CurrentColor = EHueColor::N;

	/** Colours received during the current logical time step. */
	TArray<EHueColor, TInlineAllocator<4>> PendingColors;
	int64 PendingTimeMs = 0;

	/** Entry face of the latest accepted pulse (where the conflict flash is drawn). */
	EHueDir LastEntryFace = EHueDir::West;

	/** "West, South" */
	FString DescribeAcceptFaces() const;
};

/** Solid obstruction: absorbs any pulse. Fixed. */
UCLASS()
class HUEBOX_API AHueBarrierBlock : public APulseBlock
{
	GENERATED_BODY()

public:
	AHueBarrierBlock();

	virtual EPulseResponse ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim) override;
	virtual FString GetRuleText() const override;
	virtual FString GetStateText() const override;
};
