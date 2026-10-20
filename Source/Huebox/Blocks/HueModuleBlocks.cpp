#include "Blocks/HueModuleBlocks.h"

#include "Grid/HueBoard.h"
#include "Simulation/PulseSubsystem.h"

namespace HueModuleBlocksPrivate
{
	/** Priority of block events relative to arrivals (0) at the same logical time. */
	constexpr uint8 DelayReleasePriority = 1;
	constexpr uint8 MixerTimeoutPriority = 2;

	/** Snaps to the 0.25 s grid and clamps to [MinDelay, MaxDelay]. */
	float SnapDelay(float Seconds)
	{
		const int32 MaxSteps = FMath::RoundToInt(AHueDelayBlock::MaxDelay / AHueDelayBlock::DelayStep);
		const int32 MinSteps = FMath::RoundToInt(AHueDelayBlock::MinDelay / AHueDelayBlock::DelayStep);
		const int32 Steps = FMath::Clamp(FMath::RoundToInt(Seconds / AHueDelayBlock::DelayStep), MinSteps, MaxSteps);
		return static_cast<float>(Steps) * AHueDelayBlock::DelayStep;
	}

	/** Shared blocked-entry reason for one-way modules. */
	FString OneWayReason(EHueDir TravelDir, EHueDir Facing)
	{
		return FString::Printf(TEXT("no input on the %s face (accepts pulses travelling %s)"),
			HueDir::ToString(HueDir::Opposite(TravelDir)), HueDir::ToString(Facing));
	}
}

// Presentation of every module: the base class' flat 80 x 80 x 6 cm tile ("2D white prop") in
// HueModule::BodyColor with a dark label on the top face (TileLabelColor).

// =============================================================================================================
// Turn
// =============================================================================================================

AHueTurnBlock::AHueTurnBlock()
{
	ModuleType = EHueModuleType::Turn;
	BlockDisplayName = TEXT("Turn");
	BaseColor = HueModule::BodyColor(EHueModuleType::Turn);
	SetLabelText(TEXT("TURN"), TileLabelColor());
}

bool AHueTurnBlock::TryTurn(EHueDir TravelDir, EHueDir& OutExitDir) const
{
	// A pulse travelling D enters through face Opposite(D); it leaves through the other port, travelling
	// in that port's face direction.
	const EHueDir EntryFace = HueDir::Opposite(TravelDir);
	const EHueDir PortA = Facing;
	const EHueDir PortB = HueDir::RotatePlus90(Facing);
	if (EntryFace == PortA)
	{
		OutExitDir = PortB;
		return true;
	}
	if (EntryFace == PortB)
	{
		OutExitDir = PortA;
		return true;
	}
	return false;
}

EPulseResponse AHueTurnBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	EHueDir ExitDir = Pulse.Direction;
	if (!TryTurn(Pulse.Direction, ExitDir))
	{
		ReportBlocked(Pulse, Sim,
			FString::Printf(TEXT("no port on the %s face"), HueDir::ToString(HueDir::Opposite(Pulse.Direction))));
		return EPulseResponse::Consumed;
	}

	NoteInput(Pulse, Sim);
	Pulse.Direction = ExitDir;
	NoteOutput(FormatOutput(Pulse.Color, ExitDir, Sim.GetSimTimeMs()));
	return EPulseResponse::Continue;
}

void AHueTurnBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	FHuePort PortA;
	PortA.Face = Facing;
	PortA.Kind = EHuePortKind::Both;
	OutPorts.Add(PortA);

	FHuePort PortB;
	PortB.Face = HueDir::RotatePlus90(Facing);
	PortB.Kind = EHuePortKind::Both;
	OutPorts.Add(PortB);
}

FString AHueTurnBlock::GetRuleText() const
{
	return FString::Printf(TEXT("90-degree corner: a pulse entering one port leaves through the other (ports: %s + %s)."),
		HueDir::ToString(Facing), HueDir::ToString(HueDir::RotatePlus90(Facing)));
}

FString AHueTurnBlock::GetStateText() const
{
	return FString::Printf(TEXT("Ports %s + %s | %s"),
		HueDir::ToString(Facing), HueDir::ToString(HueDir::RotatePlus90(Facing)), *Super::GetStateText());
}

// =============================================================================================================
// Colour shift
// =============================================================================================================

AHueColorShiftBlock::AHueColorShiftBlock()
{
	ModuleType = EHueModuleType::ColorShift;
	BlockDisplayName = TEXT("Colour shift");
	BaseColor = HueModule::BodyColor(EHueModuleType::ColorShift);
	SetLabelText(TEXT("A<>B"), TileLabelColor());
}

EPulseResponse AHueColorShiftBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	if (Pulse.Direction != Facing)
	{
		ReportBlocked(Pulse, Sim, HueModuleBlocksPrivate::OneWayReason(Pulse.Direction, Facing));
		return EPulseResponse::Consumed;
	}

	NoteInput(Pulse, Sim);
	const EHueColor Before = Pulse.Color;
	Pulse.Color = HueColor::Shift(Pulse.Color); // direction preserved
	NoteOutput(FString::Printf(TEXT("%s (was %s)"),
		*FormatOutput(Pulse.Color, Pulse.Direction, Sim.GetSimTimeMs()), HueColor::Label(Before)));
	return EPulseResponse::Continue;
}

void AHueColorShiftBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	FHuePort In;
	In.Face = HueDir::Opposite(Facing);
	In.Kind = EHuePortKind::Input;
	OutPorts.Add(In);

	FHuePort Out;
	Out.Face = Facing;
	Out.Kind = EHuePortKind::Output;
	OutPorts.Add(Out);
}

FString AHueColorShiftBlock::GetRuleText() const
{
	return FString::Printf(TEXT("Swaps A and B; N passes unchanged. One-way: accepts pulses travelling %s."),
		HueDir::ToString(Facing));
}

FString AHueColorShiftBlock::GetStateText() const
{
	return Super::GetStateText();
}

// =============================================================================================================
// Delay
// =============================================================================================================

AHueDelayBlock::AHueDelayBlock()
{
	ModuleType = EHueModuleType::Delay;
	BlockDisplayName = TEXT("Delay");
	BaseColor = HueModule::BodyColor(EHueModuleType::Delay);
	SetLabelText(FString::Printf(TEXT("D %.2fs"), DelaySeconds), TileLabelColor());
}

void AHueDelayBlock::OnConstruction(const FTransform& Transform)
{
	DelaySeconds = HueModuleBlocksPrivate::SnapDelay(DelaySeconds);
	Super::OnConstruction(Transform);
}

EPulseResponse AHueDelayBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	if (Pulse.Direction != Facing)
	{
		ReportBlocked(Pulse, Sim, HueModuleBlocksPrivate::OneWayReason(Pulse.Direction, Facing));
		return EPulseResponse::Consumed;
	}

	const int64 NowMs = Sim.GetSimTimeMs();
	if (bHolding)
	{
		// Occupied: one buffer slot only. The newcomer is consumed; the stored pulse is unaffected.
		LastInputText = FString::Printf(TEXT("rejected: %s from %s @%.2fs (occupied)"),
			HueColor::Label(Pulse.Color), HueDir::ToString(HueDir::Opposite(Pulse.Direction)), HueTime::MsToSeconds(NowMs));
		Sim.Report(EHueFeedbackKind::Warning,
			FString::Printf(TEXT("%s: occupied - still holding %s, incoming %s pulse rejected"),
				*DescribeBlock(), HueColor::Label(HeldPulse.Color), HueColor::Label(Pulse.Color)),
			Cell);
		FlashBlocked(HueDir::Opposite(Pulse.Direction));
		return EPulseResponse::Consumed;
	}

	NoteInput(Pulse, Sim);
	bHolding = true;
	HeldPulse = Pulse; // colour + remaining ops travel on with the released pulse
	HeldPulse.Cell = Cell;
	ReleaseTimeMs = NowMs + HueTime::SecondsToMs(DelaySeconds);

	// New serial per stored pulse: a release event scheduled before a reset can never release a later pulse.
	++EventSerial;
	Sim.ScheduleBlockEvent(this, ReleaseTimeMs, EventSerial, HueModuleBlocksPrivate::DelayReleasePriority);

	NoteOutput(FString::Printf(TEXT("holding %s until %.2fs"), HueColor::Label(Pulse.Color), HueTime::MsToSeconds(ReleaseTimeMs)));
	RefreshVisuals();
	return EPulseResponse::Consumed;
}

void AHueDelayBlock::OnScheduledEvent(int32 EventTag, UPulseSubsystem& Sim)
{
	if (!bHolding || EventTag != EventSerial)
	{
		return; // stale event (reset since it was scheduled)
	}

	// Clear the held state BEFORE emitting: the stored signal must not count twice against the signal cap.
	const FPulseData Released = HeldPulse;
	bHolding = false;
	HeldPulse = FPulseData();
	ReleaseTimeMs = 0;

	const int64 NowMs = Sim.GetSimTimeMs();
	const int32 NewId = Sim.EmitPulse(Released.Color, Cell, Facing, NowMs, Released.RemainingOps);
	if (NewId != INDEX_NONE)
	{
		NoteOutput(FormatOutput(Released.Color, Facing, NowMs));
	}
	else
	{
		NoteOutput(FString::Printf(TEXT("release of %s failed @%.2fs: signal limit"), HueColor::Label(Released.Color), HueTime::MsToSeconds(NowMs)));
	}
	RefreshVisuals();
}

void AHueDelayBlock::ResetBlock()
{
	bHolding = false;
	HeldPulse = FPulseData();
	ReleaseTimeMs = 0;
	++EventSerial;
	Super::ResetBlock();
}

void AHueDelayBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	FHuePort In;
	In.Face = HueDir::Opposite(Facing);
	In.Kind = EHuePortKind::Input;
	OutPorts.Add(In);

	FHuePort Out;
	Out.Face = Facing;
	Out.Kind = EHuePortKind::Output;
	OutPorts.Add(Out);
}

FString AHueDelayBlock::GetRuleText() const
{
	return FString::Printf(TEXT("Holds one pulse for %.2f s, then releases it travelling %s with the same colour. Rejects pulses while holding. Z/X: -/+ %.2f s."),
		DelaySeconds, HueDir::ToString(Facing), DelayStep);
}

FString AHueDelayBlock::GetStateText() const
{
	if (bHolding)
	{
		const int64 RemainingMs = FMath::Max<int64>(0, ReleaseTimeMs - GetHudSimTimeMs());
		return FString::Printf(TEXT("Holding %s, releases in %.2f s | %s"),
			HueColor::Label(HeldPulse.Color), HueTime::MsToSeconds(RemainingMs), *LastInputText);
	}
	return FString::Printf(TEXT("Delay %.2f s | %s"), DelaySeconds, *Super::GetStateText());
}

void AHueDelayBlock::RefreshVisuals()
{
	// TextRender line break is "<br>".
	LabelString = bHolding
		? FString::Printf(TEXT("D %.2fs<br>[%s]"), DelaySeconds, HueColor::Label(HeldPulse.Color))
		: FString::Printf(TEXT("D %.2fs"), DelaySeconds);
	LabelColor = TileLabelColor();
	Super::RefreshVisuals();
}

void AHueDelayBlock::SetDelaySeconds(float NewDelay)
{
	DelaySeconds = HueModuleBlocksPrivate::SnapDelay(NewDelay);
	RefreshVisuals();
}

void AHueDelayBlock::AdjustDelay(int32 Steps)
{
	SetDelaySeconds(DelaySeconds + static_cast<float>(Steps) * DelayStep);
}

// =============================================================================================================
// Mixer
// =============================================================================================================

AHueMixerBlock::AHueMixerBlock()
{
	ModuleType = EHueModuleType::Mixer;
	BlockDisplayName = TEXT("Mixer");
	BaseColor = HueModule::BodyColor(EHueModuleType::Mixer);
	SetLabelText(TEXT("MIX"), TileLabelColor());
}

EHueDir AHueMixerBlock::GetInputFace(int32 Input) const
{
	return Input == 0 ? HueDir::RotatePlus90(Facing) : HueDir::RotateMinus90(Facing);
}

int32 AHueMixerBlock::GetInputIndexForTravel(EHueDir TravelDir) const
{
	const EHueDir EntryFace = HueDir::Opposite(TravelDir);
	if (EntryFace == GetInputFace(0))
	{
		return 0;
	}
	if (EntryFace == GetInputFace(1))
	{
		return 1;
	}
	return INDEX_NONE;
}

EPulseResponse AHueMixerBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	const EHueDir EntryFace = HueDir::Opposite(Pulse.Direction);
	const int32 Input = GetInputIndexForTravel(Pulse.Direction);
	if (Input == INDEX_NONE)
	{
		const FString Why = (EntryFace == Facing)
			? FString::Printf(TEXT("the %s face is the output, not an input"), HueDir::ToString(EntryFace))
			: FString::Printf(TEXT("no input on the %s face"), HueDir::ToString(EntryFace));
		ReportBlocked(Pulse, Sim, Why);
		return EPulseResponse::Consumed;
	}

	const int64 NowMs = Sim.GetSimTimeMs();
	const int64 WindowMs = Sim.GetMixWindowMs();

	if (bWaiting && Input == WaitingInput)
	{
		// Second pulse on the input that is already waiting: rejected, the waiting pulse keeps its window.
		LastInputText = FString::Printf(TEXT("rejected: %s at in%d @%.2fs"), HueColor::Label(Pulse.Color), Input, HueTime::MsToSeconds(NowMs));
		LastFailure = FString::Printf(TEXT("in%d (%s) already waiting with %s - extra %s pulse rejected"),
			Input, HueDir::ToString(EntryFace), HueColor::Label(WaitingColor), HueColor::Label(Pulse.Color));
		LastFailureMs = NowMs;
		Sim.Report(EHueFeedbackKind::Warning, FString::Printf(TEXT("%s: %s"), *DescribeBlock(), *LastFailure), Cell);
		FlashBlocked(EntryFace);
		return EPulseResponse::Consumed;
	}

	NoteInput(Pulse, Sim);
	LastArrivalMs[Input] = NowMs;
	LastArrivalColor[Input] = Pulse.Color;

	if (bWaiting && NowMs - WaitingSinceMs > WindowMs)
	{
		// Defensive only: the timeout event (same time or earlier) normally clears a stale wait first.
		const FString Reason = FString::Printf(TEXT("timeout: other input did not arrive within %s s"),
			*FString::SanitizeFloat(HueTime::MsToSeconds(WindowMs)));
		ClearWaiting();
		ReportMixFailure(Sim, Reason, TEXT("waiting pulse discarded"), EntryFace);
	}

	if (!bWaiting)
	{
		StartWaiting(Input, Pulse, NowMs, Sim);
		RefreshVisuals();
		return EPulseResponse::Consumed;
	}

	// Second input on the other side, inside the inclusive window (arrivals run before the timeout event).
	const EHueColor FirstColor = WaitingColor;
	const EHueColor SecondColor = Pulse.Color;
	const int32 FirstInput = WaitingInput;
	const int32 OutOps = FMath::Min(WaitingOps, Pulse.RemainingOps);
	const int64 DeltaMs = NowMs - WaitingSinceMs;

	// Clear first (++EventSerial invalidates the pending timeout; the waiting signal leaves the cap count).
	ClearWaiting();

	const bool bIsAB = (FirstColor == EHueColor::A && SecondColor == EHueColor::B)
		|| (FirstColor == EHueColor::B && SecondColor == EHueColor::A);
	if (bIsAB)
	{
		LastFailure.Reset();
		LastFailureMs = -1;
		const int32 NewId = Sim.EmitPulse(EHueColor::N, Cell, Facing, NowMs, OutOps);
		if (NewId != INDEX_NONE)
		{
			NoteOutput(FString::Printf(TEXT("%s (A+B, dt %.2fs)"), *FormatOutput(EHueColor::N, Facing, NowMs), HueTime::MsToSeconds(DeltaMs)));
			Sim.Report(EHueFeedbackKind::Info,
				FString::Printf(TEXT("%s: A + B -> N (dt %.2f s)"), *DescribeBlock(), HueTime::MsToSeconds(DeltaMs)), Cell);
		}
		else
		{
			NoteOutput(FString::Printf(TEXT("mixed @%.2fs but the N pulse hit the signal limit"), HueTime::MsToSeconds(NowMs)));
		}
	}
	else
	{
		const FString Reason = (FirstColor == EHueColor::N || SecondColor == EHueColor::N)
			? FString(TEXT("N cannot be mixed"))
			: FString::Printf(TEXT("matching colours %s+%s"), HueColor::Label(FirstColor), HueColor::Label(SecondColor));
		const FString Detail = FString::Printf(TEXT("in%d %s + in%d %s, both pulses consumed"),
			FirstInput, HueColor::Label(FirstColor), Input, HueColor::Label(SecondColor));
		ReportMixFailure(Sim, Reason, Detail, EntryFace);
	}

	RefreshVisuals();
	return EPulseResponse::Consumed;
}

void AHueMixerBlock::StartWaiting(int32 Input, const FPulseData& Pulse, int64 NowMs, UPulseSubsystem& Sim)
{
	bWaiting = true;
	WaitingInput = Input;
	WaitingColor = Pulse.Color;
	WaitingSinceMs = NowMs;
	WaitingOps = Pulse.RemainingOps;

	++EventSerial;
	Sim.ScheduleBlockEvent(this, NowMs + Sim.GetMixWindowMs(), EventSerial, HueModuleBlocksPrivate::MixerTimeoutPriority);

	NoteOutput(FString::Printf(TEXT("waiting at in%d until %.2fs"), Input, HueTime::MsToSeconds(NowMs + Sim.GetMixWindowMs())));
}

void AHueMixerBlock::OnScheduledEvent(int32 EventTag, UPulseSubsystem& Sim)
{
	if (!bWaiting || EventTag != EventSerial)
	{
		return; // stale timeout: the wait was mixed, failed or reset after it was scheduled
	}

	const int32 MissingInput = 1 - WaitingInput;
	const FString Reason = FString::Printf(TEXT("timeout: other input did not arrive within %s s"),
		*FString::SanitizeFloat(HueTime::MsToSeconds(Sim.GetMixWindowMs())));
	const FString Detail = FString::Printf(TEXT("in%d %s waited since %.2fs, nothing reached in%d on the %s face"),
		WaitingInput, HueColor::Label(WaitingColor), HueTime::MsToSeconds(WaitingSinceMs),
		MissingInput, HueDir::ToString(GetInputFace(MissingInput)));

	ClearWaiting();
	ReportMixFailure(Sim, Reason, Detail, GetInputFace(MissingInput));
	RefreshVisuals();
}

void AHueMixerBlock::ReportMixFailure(UPulseSubsystem& Sim, const FString& Reason, const FString& Detail, EHueDir Face)
{
	LastFailure = Reason;
	LastFailureMs = Sim.GetSimTimeMs();
	NoteOutput(FString::Printf(TEXT("failed @%.2fs: %s"), HueTime::MsToSeconds(LastFailureMs), *Reason));
	Sim.Report(EHueFeedbackKind::Warning, FString::Printf(TEXT("%s: %s (%s)"), *DescribeBlock(), *Reason, *Detail), Cell);
	FlashBlocked(Face);
}

void AHueMixerBlock::ClearWaiting()
{
	bWaiting = false;
	WaitingInput = INDEX_NONE;
	WaitingColor = EHueColor::A;
	WaitingSinceMs = 0;
	WaitingOps = 0;
	++EventSerial;
}

void AHueMixerBlock::ResetBlock()
{
	ClearWaiting();
	LastArrivalMs[0] = LastArrivalMs[1] = -1;
	LastArrivalColor[0] = LastArrivalColor[1] = EHueColor::A;
	LastFailure.Reset();
	LastFailureMs = -1;
	Super::ResetBlock();
}

void AHueMixerBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	for (int32 Input = 0; Input < 2; ++Input)
	{
		FHuePort In;
		In.Face = GetInputFace(Input);
		In.Kind = EHuePortKind::Input;
		OutPorts.Add(In);
	}

	FHuePort Out;
	Out.Face = Facing;
	Out.Kind = EHuePortKind::Output;
	OutPorts.Add(Out);
}

FString AHueMixerBlock::GetRuleText() const
{
	const AHueBoard* Board = GetBoard();
	const double WindowSeconds = Board ? Board->MixWindowSeconds : 0.5;
	return FString::Printf(TEXT("A at one input + B at the other within %s s -> one N pulse out %s. in0 = %s face, in1 = %s face. Matching colours, N or a timeout fail."),
		*FString::SanitizeFloat(WindowSeconds), HueDir::ToString(Facing),
		HueDir::ToString(GetInputFace(0)), HueDir::ToString(GetInputFace(1)));
}

FString AHueMixerBlock::DescribeArrival(int32 Input) const
{
	if (LastArrivalMs[Input] < 0)
	{
		return FString::Printf(TEXT("in%d -"), Input);
	}
	return FString::Printf(TEXT("in%d %s @%.2fs"), Input, HueColor::Label(LastArrivalColor[Input]), HueTime::MsToSeconds(LastArrivalMs[Input]));
}

FString AHueMixerBlock::GetStateText() const
{
	// GDD: arrival labels per input let the player see which route needs adjusting.
	TArray<FString> Parts;
	if (bWaiting)
	{
		Parts.Add(FString::Printf(TEXT("waiting: in%d %s since %.2fs"),
			WaitingInput, HueColor::Label(WaitingColor), HueTime::MsToSeconds(WaitingSinceMs)));
	}
	Parts.Add(DescribeArrival(0));
	Parts.Add(DescribeArrival(1));
	if (LastArrivalMs[0] >= 0 && LastArrivalMs[1] >= 0)
	{
		Parts.Add(FString::Printf(TEXT("dt %.2fs"), HueTime::MsToSeconds(FMath::Abs(LastArrivalMs[0] - LastArrivalMs[1]))));
	}
	if (!LastFailure.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("failed @%.2fs: %s"), HueTime::MsToSeconds(FMath::Max<int64>(0, LastFailureMs)), *LastFailure));
	}
	else if (!LastOutputText.IsEmpty())
	{
		Parts.Add(LastOutputText);
	}
	return FString::Join(Parts, TEXT(" | "));
}

void AHueMixerBlock::RefreshVisuals()
{
	LabelString = bWaiting ? TEXT("MIX...") : TEXT("MIX");
	LabelColor = TileLabelColor();
	Super::RefreshVisuals();
}
