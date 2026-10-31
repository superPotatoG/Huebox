#include "Blocks/HueFixedBlocks.h"

#include "Rig/ReactiveRig.h"
#include "Simulation/PulseSubsystem.h"

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HueFixedBlocksPrivate
{
	/** Sources and receivers: flat tiles slightly larger than the 80 cm modules, so fixed units stand out. */
	const FVector FixedTileScale(0.86f, 0.86f, 0.06f);
	/** Barrier: a near-black tile filling the cell, a little thicker. */
	const FVector BarrierTileScale(0.98f, 0.98f, 0.1f);
	const FLinearColor BarrierColor(0.04f, 0.04f, 0.045f);
	/** Brick: a thick slab in its colour filling the cell (thicker than the flat sources); once broken, a thin dark rim
	 *  stays as a trace. */
	const FVector BrickStandingScale(0.98f, 0.98f, 0.16f);
	const FVector BrickBrokenScale(0.9f, 0.9f, 0.015f);
	constexpr float BrickBrokenDim = 0.22f;
}

// =============================================================================================================
// Source
// =============================================================================================================

AHueSourceBlock::AHueSourceBlock()
{
	bRemovable = false;
	ModuleType = EHueModuleType::None;
	BlockDisplayName = TEXT("Source");
	BodyScale = HueFixedBlocksPrivate::FixedTileScale;
	ApplyBodyLayout();
	// Tile in the emitted colour with dark ink.
	BaseColor = HueColor::ToLinear(EmitColor);
	SetLabelText(FString::Printf(TEXT("SRC %s"), HueColor::Label(EmitColor)), TileLabelColor());
}

void AHueSourceBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	TArray<EHueDir> Faces;
	GetEmitFaces(Faces);
	for (const EHueDir Face : Faces)
	{
		FHuePort Out;
		Out.Face = Face;
		Out.Kind = EHuePortKind::Output;
		OutPorts.Add(Out);
	}
}

FString AHueSourceBlock::GetRuleText() const
{
	if (IsMultiOutput())
	{
		return FString::Printf(TEXT("Source base: emits one %s pulse through each of its %d outputs (%s) when the run starts (t = 0). Blocks incoming pulses."),
			HueColor::Label(EmitColor), EmitFaces.Num(), *DescribeEmitFaces());
	}
	return FString::Printf(TEXT("Emits one %s pulse travelling %s when the run starts (t = 0). Blocks incoming pulses."),
		HueColor::Label(EmitColor), *DescribeEmitFaces());
}

void AHueSourceBlock::SetEmitFaces(const TArray<EHueDir>& Faces)
{
	// Unique faces, in the given order (that is the emission order).
	EmitFaces.Reset();
	for (const EHueDir Face : Faces)
	{
		EmitFaces.AddUnique(Face);
	}
	RebuildPortMarkers();
	RefreshVisuals();
}

void AHueSourceBlock::GetEmitFaces(TArray<EHueDir>& OutFaces) const
{
	OutFaces.Reset();
	if (EmitFaces.Num() == 0)
	{
		OutFaces.Add(Facing);
	}
	else
	{
		OutFaces.Append(EmitFaces);
	}
}

FString AHueSourceBlock::DescribeEmitFaces() const
{
	TArray<EHueDir> Faces;
	GetEmitFaces(Faces);
	TArray<FString> Names;
	for (const EHueDir Face : Faces)
	{
		Names.Add(HueDir::ToString(Face));
	}
	return FString::Join(Names, TEXT(" "));
}

FString AHueSourceBlock::GetStateText() const
{
	if (LastOutputText.IsEmpty())
	{
		return IsMultiOutput()
			? FString::Printf(TEXT("Ready: fires %d x %s at t = 0"), EmitFaces.Num(), HueColor::Label(EmitColor))
			: FString::Printf(TEXT("Ready: fires %s at t = 0"), HueColor::Label(EmitColor));
	}
	return Super::GetStateText();
}

void AHueSourceBlock::RefreshVisuals()
{
	BaseColor = HueColor::ToLinear(EmitColor);
	// A base reads "BASE" (its output arrows on every face show how many); short enough for the tile font.
	LabelString = FString::Printf(TEXT("%s %s"), IsMultiOutput() ? TEXT("BASE") : TEXT("SRC"), HueColor::Label(EmitColor));
	LabelColor = TileLabelColor();
	Super::RefreshVisuals();
}

void AHueSourceBlock::SetEmitColor(EHueColor InColor)
{
	EmitColor = InColor;
	RefreshVisuals();
}

void AHueSourceBlock::NotifyEmitted(int64 TimeMs)
{
	if (IsMultiOutput())
	{
		NoteOutput(FString::Printf(TEXT("out: %d x %s to %s @%.2fs"), EmitFaces.Num(), HueColor::Label(EmitColor),
			*DescribeEmitFaces(), HueTime::MsToSeconds(TimeMs)));
		return;
	}
	NoteOutput(FormatOutput(EmitColor, Facing, TimeMs));
}

// =============================================================================================================
// Receiver
// =============================================================================================================

AHueReceiverBlock::AHueReceiverBlock()
{
	bRemovable = false;
	ModuleType = EHueModuleType::None;
	BlockDisplayName = TEXT("Receiver");
	// Permissive default for receivers placed by hand; layouts always call SetAcceptFaces.
	AcceptFaces = { EHueDir::East, EHueDir::North, EHueDir::West, EHueDir::South };
	BodyScale = HueFixedBlocksPrivate::FixedTileScale;
	ApplyBodyLayout();
	// Tile in the currently held colour (N = white) with dark ink.
	BaseColor = HueColor::ToLinear(CurrentColor);
	SetLabelText(FString::Printf(TEXT("RCV %s"), HueColor::Label(CurrentColor)), TileLabelColor());
}

EPulseResponse AHueReceiverBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	const EHueDir EntryFace = HueDir::Opposite(Pulse.Direction);
	if (!AcceptFaces.Contains(EntryFace))
	{
		ReportBlocked(Pulse, Sim, FString::Printf(TEXT("no input on the %s face (accepts: %s)"),
			HueDir::ToString(EntryFace), *DescribeAcceptFaces()));
		return EPulseResponse::Consumed;
	}

	const int64 NowMs = Sim.GetSimTimeMs();
	if ((PendingColors.Num() > 0 || PendingKeyColors.Num() > 0) && PendingTimeMs != NowMs)
	{
		// Defensive: a previous group was never closed; resolve it before starting a new one.
		OnTimeGroupEnd(Sim);
	}

	NoteInput(Pulse, Sim);
	LastEntryFace = EntryFace;
	PendingTimeMs = NowMs;
	// Key colours stay out of the motion group: they can neither move the dancer nor cancel an A / B / N arriving at
	// the same time. Both are delivered in OnTimeGroupEnd, once every arrival of this logical time has been seen.
	if (HueColor::DrivesRig(Pulse.Color))
	{
		PendingColors.Add(Pulse.Color);
	}
	else
	{
		PendingKeyColors.AddUnique(Pulse.Color);
	}
	return EPulseResponse::Consumed;
}

void AHueReceiverBlock::OnTimeGroupEnd(UPulseSubsystem& Sim)
{
	ResolveMotionGroup(Sim);
	DeliverKeyColors(Sim);
}

void AHueReceiverBlock::DeliverKeyColors(UPulseSubsystem& Sim)
{
	if (PendingKeyColors.Num() == 0)
	{
		return;
	}
	const double TimeSeconds = HueTime::MsToSeconds(PendingTimeMs);
	for (const EHueColor Key : PendingKeyColors)
	{
		FHueColorEvent Event;
		Event.TimeSeconds = static_cast<float>(TimeSeconds);
		Event.Color = Key;
		AppliedHistory.Add(Event);

		if (IsValid(Rig))
		{
			Rig->PerformKeyAction(Key);
		}
		NoteOutput(FString::Printf(TEXT("%s move @%.2fs"), HueColor::Label(Key), TimeSeconds));
		Sim.Report(EHueFeedbackKind::Info,
			FString::Printf(TEXT("Receiver -> %s: the %s move"), HueColor::Label(Key), HueColor::Name(Key)), Cell);
	}
	PendingKeyColors.Reset();
}

void AHueReceiverBlock::ResolveMotionGroup(UPulseSubsystem& Sim)
{
	if (PendingColors.Num() == 0)
	{
		return;
	}

	TArray<EHueColor, TInlineAllocator<3>> Distinct;
	for (const EHueColor Color : PendingColors)
	{
		Distinct.AddUnique(Color);
	}
	PendingColors.Reset();
	const double TimeSeconds = HueTime::MsToSeconds(PendingTimeMs);

	if (Distinct.Num() > 1)
	{
		// Different colours at the same logical time cancel: state unchanged (GDD).
		++ConflictCount;
		TArray<FString> Names;
		for (const EHueColor Color : Distinct)
		{
			Names.Add(HueColor::Label(Color));
		}
		const FString List = FString::Join(Names, TEXT(" + "));
		NoteOutput(FString::Printf(TEXT("conflict @%.2fs: %s (unchanged)"), TimeSeconds, *List));
		Sim.Report(EHueFeedbackKind::Warning,
			FString::Printf(TEXT("%s: conflict at %.2fs - %s arrived at the same time; colour unchanged (%s)"),
				*DescribeBlock(), TimeSeconds, *List, HueColor::Label(CurrentColor)),
			Cell);
		FlashBlocked(LastEntryFace);
		return;
	}

	const EHueColor NewColor = Distinct[0];
	CurrentColor = NewColor;

	FHueColorEvent Event;
	Event.TimeSeconds = static_cast<float>(TimeSeconds);
	Event.Color = NewColor;
	AppliedHistory.Add(Event);

	if (IsValid(Rig))
	{
		Rig->ApplyColor(NewColor);
	}

	NoteOutput(FString::Printf(TEXT("applied %s @%.2fs"), HueColor::Label(NewColor), TimeSeconds));
	Sim.Report(EHueFeedbackKind::Info, FString::Printf(TEXT("Receiver -> %s"), HueColor::Label(NewColor)), Cell);
	RefreshVisuals();
}

void AHueReceiverBlock::ResetBlock()
{
	CurrentColor = EHueColor::N;
	PendingColors.Reset();
	PendingKeyColors.Reset();
	PendingTimeMs = 0;
	AppliedHistory.Reset();
	ConflictCount = 0;
	Super::ResetBlock();
}

void AHueReceiverBlock::GetPorts(TArray<FHuePort>& OutPorts) const
{
	for (const EHueDir Face : AcceptFaces)
	{
		FHuePort In;
		In.Face = Face;
		In.Kind = EHuePortKind::Input;
		OutPorts.Add(In);
	}
}

FString AHueReceiverBlock::GetRuleText() const
{
	return FString::Printf(TEXT("Drives the dancer: A repels, B attracts, N brakes; any other colour makes her do its move where she is. Accepts pulses through: %s. Different A / B / N at the same moment cancel."),
		*DescribeAcceptFaces());
}

FString AHueReceiverBlock::GetStateText() const
{
	return FString::Printf(TEXT("Holding %s | applied %d | conflicts %d | %s"),
		HueColor::Label(CurrentColor), AppliedHistory.Num(), ConflictCount, *Super::GetStateText());
}

void AHueReceiverBlock::RefreshVisuals()
{
	BaseColor = HueColor::ToLinear(CurrentColor);
	LabelString = FString::Printf(TEXT("RCV %s"), HueColor::Label(CurrentColor));
	LabelColor = TileLabelColor();
	Super::RefreshVisuals();
}

void AHueReceiverBlock::SetAcceptFaces(const TArray<EHueDir>& Faces)
{
	AcceptFaces.Reset();
	for (const EHueDir Face : Faces)
	{
		AcceptFaces.AddUnique(Face);
	}
	RebuildPortMarkers();
}

void AHueReceiverBlock::SetRig(AReactiveRig* InRig)
{
	Rig = InRig;
}

FString AHueReceiverBlock::DescribeAcceptFaces() const
{
	if (AcceptFaces.Num() == 0)
	{
		return TEXT("none");
	}
	TArray<FString> Names;
	for (const EHueDir Face : AcceptFaces)
	{
		Names.Add(HueDir::ToString(Face));
	}
	return FString::Join(Names, TEXT(", "));
}

// =============================================================================================================
// Barrier
// =============================================================================================================

AHueBarrierBlock::AHueBarrierBlock()
{
	bRemovable = false;
	ModuleType = EHueModuleType::None;
	BlockDisplayName = TEXT("Barrier");
	BaseColor = HueFixedBlocksPrivate::BarrierColor;
	BodyScale = HueFixedBlocksPrivate::BarrierTileScale;
	ApplyBodyLayout();
	SetLabelText(FString(), TileLabelColor()); // no label: the dark tile speaks for itself
}

EPulseResponse AHueBarrierBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	// Quiet by design: a barrier is part of the scenery, so absorbing raises no feedback line.
	LastInputText = FString::Printf(TEXT("absorbed %s from %s @%.2fs"),
		HueColor::Label(Pulse.Color), HueDir::ToString(HueDir::Opposite(Pulse.Direction)), HueTime::MsToSeconds(Sim.GetSimTimeMs()));
	return EPulseResponse::Consumed;
}

FString AHueBarrierBlock::GetRuleText() const
{
	return TEXT("Solid obstruction: absorbs every pulse.");
}

FString AHueBarrierBlock::GetStateText() const
{
	return LastInputText.IsEmpty() ? FString(TEXT("Absorbs pulses")) : LastInputText;
}

// =============================================================================================================
// Brick
// =============================================================================================================

AHueBrickBlock::AHueBrickBlock()
{
	bRemovable = false;
	ModuleType = EHueModuleType::None;
	BlockDisplayName = TEXT("Brick");
	BodyScale = HueFixedBlocksPrivate::BrickStandingScale;
	ApplyBodyLayout();
	BaseColor = HueColor::ToLinear(BrickColor);
	SetLabelText(FString::Printf(TEXT("BRICK %s"), HueColor::Label(BrickColor)), TileLabelColor());
}

EPulseResponse AHueBrickBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	const int64 NowMs = Sim.GetSimTimeMs();
	if (bBroken)
	{
		// An open cell: the pulse travels on unchanged.
		NoteInput(Pulse, Sim);
		NoteOutput(FormatOutput(Pulse.Color, Pulse.Direction, NowMs));
		return EPulseResponse::Continue;
	}

	if (Pulse.Color != BrickColor)
	{
		ReportBlocked(Pulse, Sim, bBreakPending
			? FString::Printf(TEXT("the %s pulse arrived in the same instant it broke - blocked"), HueColor::Label(Pulse.Color))
			: FString::Printf(TEXT("only %s (%s) light breaks it - the %s pulse was blocked"),
				HueColor::Label(BrickColor), HueColor::Name(BrickColor), HueColor::Label(Pulse.Color)));
		return EPulseResponse::Consumed;
	}

	// The key is used up on the brick. It opens when this instant ends (OnTimeGroupEnd), so whether another pulse
	// arriving at the very same time passes never depends on the order the queue processes them in.
	NoteInput(Pulse, Sim);
	if (!bBreakPending)
	{
		bBreakPending = true;
		Sim.Report(EHueFeedbackKind::Info,
			FString::Printf(TEXT("%s: broken by %s at %.2fs - that pulse was used up, the way is open"), *DescribeBlock(),
				HueColor::Label(Pulse.Color), HueTime::MsToSeconds(NowMs)),
			Cell);
	}
	NoteOutput(FString::Printf(TEXT("breaking @%.2fs"), HueTime::MsToSeconds(NowMs)));
	return EPulseResponse::Consumed;
}

void AHueBrickBlock::OnTimeGroupEnd(UPulseSubsystem& Sim)
{
	if (!bBreakPending)
	{
		return;
	}
	bBreakPending = false;
	bBroken = true;
	BrokenAtMs = Sim.GetSimTimeMs();
	NoteOutput(FString::Printf(TEXT("broken @%.2fs"), HueTime::MsToSeconds(BrokenAtMs)));
	RefreshVisuals();
}

void AHueBrickBlock::ResetBlock()
{
	bBroken = false;
	bBreakPending = false;
	BrokenAtMs = -1;
	Super::ResetBlock();
}

FString AHueBrickBlock::GetRuleText() const
{
	return FString::Printf(TEXT("A lock: only %s (%s) light breaks it, and that pulse is used up. Other colours are blocked while it stands; once broken, pulses pass straight through. Reset restores it."),
		HueColor::Label(BrickColor), HueColor::Name(BrickColor));
}

FString AHueBrickBlock::GetStateText() const
{
	if (bBroken)
	{
		return FString::Printf(TEXT("Broken at %.2f s - open | %s"), HueTime::MsToSeconds(BrokenAtMs), *Super::GetStateText());
	}
	return FString::Printf(TEXT("Standing - needs %s | %s"), HueColor::Label(BrickColor), *Super::GetStateText());
}

void AHueBrickBlock::RefreshVisuals()
{
	const FVector WantedScale = bBroken ? HueFixedBlocksPrivate::BrickBrokenScale : HueFixedBlocksPrivate::BrickStandingScale;
	if (!BodyScale.Equals(WantedScale))
	{
		BodyScale = WantedScale;
		ApplyBodyLayout();
	}

	const FLinearColor Color = HueColor::ToLinear(BrickColor);
	if (bBroken)
	{
		BaseColor = FLinearColor(Color.R * HueFixedBlocksPrivate::BrickBrokenDim, Color.G * HueFixedBlocksPrivate::BrickBrokenDim,
			Color.B * HueFixedBlocksPrivate::BrickBrokenDim, 1.f);
		LabelString = TEXT("open");
	}
	else
	{
		BaseColor = Color;
		LabelString = FString::Printf(TEXT("BRICK %s"), HueColor::Label(BrickColor));
	}
	LabelColor = ContrastTextColor(BaseColor);
	Super::RefreshVisuals();
}

void AHueBrickBlock::SetBrickColor(EHueColor InColor)
{
	BrickColor = InColor;
	RefreshVisuals();
}
