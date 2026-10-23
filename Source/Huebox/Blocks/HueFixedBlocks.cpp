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
	/** Colour brick: a thick slab in its colour filling the cell; once broken, a thin dark rim stays as a trace. */
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
	FHuePort Out;
	Out.Face = Facing;
	Out.Kind = EHuePortKind::Output;
	OutPorts.Add(Out);
}

FString AHueSourceBlock::GetRuleText() const
{
	return FString::Printf(TEXT("Emits one %s pulse travelling %s when the run starts (t = 0). Blocks incoming pulses."),
		HueColor::Label(EmitColor), HueDir::ToString(Facing));
}

FString AHueSourceBlock::GetStateText() const
{
	if (LastOutputText.IsEmpty())
	{
		return FString::Printf(TEXT("Ready: fires %s at t = 0"), HueColor::Label(EmitColor));
	}
	return Super::GetStateText();
}

void AHueSourceBlock::RefreshVisuals()
{
	BaseColor = HueColor::ToLinear(EmitColor);
	LabelString = FString::Printf(TEXT("SRC %s"), HueColor::Label(EmitColor));
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

	if (!HueColor::DrivesRig(Pulse.Color))
	{
		// Key colours mean nothing to the dancer: absorbed without joining this instant's colour group, so they
		// can neither move her nor cancel an A / B / N arriving at the same time.
		LastInputText = FString::Printf(TEXT("ignored: %s from %s @%.2fs"),
			HueColor::Label(Pulse.Color), HueDir::ToString(EntryFace), HueTime::MsToSeconds(Sim.GetSimTimeMs()));
		Sim.Report(EHueFeedbackKind::Warning,
			FString::Printf(TEXT("%s: the dancer only reacts to A, B or N - the %s (%s) pulse was absorbed"),
				*DescribeBlock(), HueColor::Label(Pulse.Color), HueColor::Name(Pulse.Color)),
			Cell);
		FlashBlocked(EntryFace);
		return EPulseResponse::Consumed;
	}

	const int64 NowMs = Sim.GetSimTimeMs();
	if (PendingColors.Num() > 0 && PendingTimeMs != NowMs)
	{
		// Defensive: a previous group was never closed; resolve it before starting a new one.
		OnTimeGroupEnd(Sim);
	}

	NoteInput(Pulse, Sim);
	LastEntryFace = EntryFace;
	PendingTimeMs = NowMs;
	PendingColors.Add(Pulse.Color);
	// The colour is applied in OnTimeGroupEnd, once every arrival of this logical time has been seen.
	return EPulseResponse::Consumed;
}

void AHueReceiverBlock::OnTimeGroupEnd(UPulseSubsystem& Sim)
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
	return FString::Printf(TEXT("Drives the dancer: A repels, B attracts, N brakes; other colours are absorbed. Accepts pulses through: %s. Different colours at the same moment cancel."),
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
// Colour brick
// =============================================================================================================

AHueColorBrickBlock::AHueColorBrickBlock()
{
	bRemovable = false;
	ModuleType = EHueModuleType::None;
	BlockDisplayName = TEXT("Colour brick");
	BodyScale = HueFixedBlocksPrivate::BrickStandingScale;
	ApplyBodyLayout();
	BaseColor = HueColor::ToLinear(BrickColor);
	SetLabelText(FString::Printf(TEXT("BRICK %s"), HueColor::Label(BrickColor)), TileLabelColor());
}

EPulseResponse AHueColorBrickBlock::ReceivePulse(FPulseData& Pulse, UPulseSubsystem& Sim)
{
	const int64 NowMs = Sim.GetSimTimeMs();
	if (bBroken)
	{
		// An open cell: the pulse travels on unchanged.
		NoteInput(Pulse, Sim);
		NoteOutput(FormatOutput(Pulse.Color, Pulse.Direction, NowMs));
		return EPulseResponse::Continue;
	}

	if (Pulse.Color == BrickColor)
	{
		// The key is spent on the brick. It opens when this instant ends (OnTimeGroupEnd), so whether another pulse
		// arriving at the very same time passes never depends on the order the queue processes them in.
		NoteInput(Pulse, Sim);
		if (!bBreakPending)
		{
			bBreakPending = true;
			Sim.Report(EHueFeedbackKind::Info,
				FString::Printf(TEXT("%s: broken by %s at %.2fs - the way is open"), *DescribeBlock(), HueColor::Label(Pulse.Color),
					HueTime::MsToSeconds(NowMs)),
				Cell);
		}
		NoteOutput(FString::Printf(TEXT("breaking @%.2fs"), HueTime::MsToSeconds(NowMs)));
		return EPulseResponse::Consumed;
	}

	ReportBlocked(Pulse, Sim, FString::Printf(TEXT("only %s (%s) light breaks it - the %s pulse was blocked"),
		HueColor::Label(BrickColor), HueColor::Name(BrickColor), HueColor::Label(Pulse.Color)));
	return EPulseResponse::Consumed;
}

void AHueColorBrickBlock::OnTimeGroupEnd(UPulseSubsystem& Sim)
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

void AHueColorBrickBlock::ResetBlock()
{
	bBroken = false;
	bBreakPending = false;
	BrokenAtMs = -1;
	Super::ResetBlock();
}

FString AHueColorBrickBlock::GetRuleText() const
{
	return FString::Printf(TEXT("A lock: only a %s (%s) pulse breaks it, and is used up doing so. Other colours are blocked while it stands; once broken, pulses pass straight through. Reset restores it."),
		HueColor::Label(BrickColor), HueColor::Name(BrickColor));
}

FString AHueColorBrickBlock::GetStateText() const
{
	if (bBroken)
	{
		return FString::Printf(TEXT("Broken at %.2f s - open | %s"), HueTime::MsToSeconds(BrokenAtMs), *Super::GetStateText());
	}
	return FString::Printf(TEXT("Standing - needs %s | %s"), HueColor::Label(BrickColor), *Super::GetStateText());
}

void AHueColorBrickBlock::RefreshVisuals()
{
	const FLinearColor Color = HueColor::ToLinear(BrickColor);
	const FVector WantedScale = bBroken ? HueFixedBlocksPrivate::BrickBrokenScale : HueFixedBlocksPrivate::BrickStandingScale;
	if (!BodyScale.Equals(WantedScale))
	{
		BodyScale = WantedScale;
		ApplyBodyLayout();
	}

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

void AHueColorBrickBlock::SetBrickColor(EHueColor InColor)
{
	BrickColor = InColor;
	RefreshVisuals();
}
