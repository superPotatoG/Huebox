#include "UI/HueboxHUD.h"

#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Game/ExhibitManager.h"
#include "Grid/HueBoard.h"
#include "Player/HueboxCharacter.h"
#include "Player/HueInteractable.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"
#include "Rig/ReactiveRig.h"
#include "Simulation/PulseSubsystem.h"

/**
 * Canvas helpers. Everything here runs inside DrawHUD only (the canvas is valid for that call); fonts are
 * engine-owned (referenced by GEngine), so the raw UFont pointers are never stored beyond one frame.
 */
namespace HueHUD
{
	static const FLinearColor Backing(0.f, 0.f, 0.f, 0.55f);
	static const FLinearColor TextColor(0.93f, 0.93f, 0.9f);
	static const FLinearColor DimColor(0.62f, 0.62f, 0.6f);
	static const FLinearColor AmberColor(1.f, 0.72f, 0.22f);
	static const FLinearColor GoodColor(0.4f, 0.92f, 0.45f);
	static const FLinearColor WarnColor(1.f, 0.82f, 0.3f);
	static const FLinearColor BadColor(1.f, 0.38f, 0.32f);
	static const FLinearColor InfoColor(0.55f, 0.85f, 1.f);

	static constexpr float Margin = 24.f;
	static constexpr float Pad = 8.f;
	static constexpr float LineGap = 2.f;
	static constexpr float PanelGap = 12.f;
	static constexpr float FeedbackHoldSeconds = 6.f;
	static constexpr float FeedbackFadeSeconds = 2.f;

	struct FLine
	{
		FString Text;
		FLinearColor Color = TextColor;
		bool bHeading = false;
	};

	static UFont* BodyFont()
	{
		return GEngine ? GEngine->GetSmallFont() : nullptr;
	}

	static UFont* HeadingFont()
	{
		return GEngine ? GEngine->GetMediumFont() : nullptr;
	}

	static FLinearColor WithAlpha(FLinearColor Color, float Alpha)
	{
		Color.A *= Alpha;
		return Color;
	}

	static void AddLine(TArray<FLine>& Lines, const FString& Text, const FLinearColor& Color, bool bHeading = false)
	{
		FLine& Line = Lines.AddDefaulted_GetRef();
		Line.Text = Text;
		Line.Color = Color;
		Line.bHeading = bHeading;
	}

	static FVector2f Measure(const AHUD& HUD, const FString& Text, UFont* Font, float Scale)
	{
		float Width = 0.f;
		float Height = 0.f;
		// Empty lines still take one line of height.
		HUD.GetTextSize(Text.IsEmpty() ? FString(TEXT(" ")) : Text, Width, Height, Font, Scale);
		return FVector2f(Width, Height);
	}

	/** Greedy word wrap of Text (may contain '\n') into lines no wider than MaxWidth pixels. */
	static void AddWrapped(const AHUD& HUD, TArray<FLine>& Lines, const FString& Text, const FLinearColor& Color, float MaxWidth, float Scale)
	{
		TArray<FString> Paragraphs;
		Text.ParseIntoArray(Paragraphs, TEXT("\n"), /*InCullEmpty*/ false);

		UFont* Font = BodyFont();
		for (const FString& Paragraph : Paragraphs)
		{
			TArray<FString> Words;
			Paragraph.ParseIntoArrayWS(Words);

			FString Current;
			for (const FString& Word : Words)
			{
				const FString Candidate = Current.IsEmpty() ? Word : Current + TEXT(" ") + Word;
				if (!Current.IsEmpty() && Measure(HUD, Candidate, Font, Scale).X > MaxWidth)
				{
					AddLine(Lines, Current, Color);
					Current = Word;
				}
				else
				{
					Current = Candidate;
				}
			}
			AddLine(Lines, Current, Color);
		}
	}

	/**
	 * Draws Lines inside one backing box. (X, Y) is the top-left corner, or the top-right corner when bAlignRight.
	 * Returns the box size.
	 */
	static FVector2f DrawPanel(AHUD& HUD, const TArray<FLine>& Lines, float X, float Y, float Scale, float MinContentWidth,
		bool bAlignRight, float Alpha = 1.f)
	{
		if (Lines.Num() == 0)
		{
			return FVector2f::ZeroVector;
		}

		const float PadPx = Pad * Scale;
		const float GapPx = LineGap * Scale;

		TArray<FVector2f, TInlineAllocator<48>> Sizes;
		Sizes.Reserve(Lines.Num());
		float ContentWidth = MinContentWidth;
		float ContentHeight = GapPx * (Lines.Num() - 1);
		for (const FLine& Line : Lines)
		{
			const FVector2f Size = Measure(HUD, Line.Text, Line.bHeading ? HeadingFont() : BodyFont(), Scale);
			Sizes.Add(Size);
			ContentWidth = FMath::Max(ContentWidth, static_cast<float>(Size.X));
			ContentHeight += static_cast<float>(Size.Y);
		}

		const float BoxWidth = ContentWidth + 2.f * PadPx;
		const float BoxHeight = ContentHeight + 2.f * PadPx;
		const float Left = bAlignRight ? X - BoxWidth : X;

		HUD.DrawRect(WithAlpha(Backing, Alpha), Left, Y, BoxWidth, BoxHeight);

		float CursorY = Y + PadPx;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FLine& Line = Lines[Index];
			if (!Line.Text.IsEmpty())
			{
				HUD.DrawText(Line.Text, WithAlpha(Line.Color, Alpha), Left + PadPx, CursorY,
					Line.bHeading ? HeadingFont() : BodyFont(), Scale);
			}
			CursorY += static_cast<float>(Sizes[Index].Y) + GapPx;
		}

		return FVector2f(BoxWidth, BoxHeight);
	}

	/** Rectangle outline made of four thin rects. */
	static void DrawFrame(AHUD& HUD, float X, float Y, float W, float H, float Thickness, const FLinearColor& Color)
	{
		HUD.DrawRect(Color, X, Y, W, Thickness);
		HUD.DrawRect(Color, X, Y + H - Thickness, W, Thickness);
		HUD.DrawRect(Color, X, Y + Thickness, Thickness, H - 2.f * Thickness);
		HUD.DrawRect(Color, X + W - Thickness, Y + Thickness, Thickness, H - 2.f * Thickness);
	}

	static FLinearColor FeedbackColor(EHueFeedbackKind Kind)
	{
		switch (Kind)
		{
		case EHueFeedbackKind::Success: return GoodColor;
		case EHueFeedbackKind::Warning: return WarnColor;
		case EHueFeedbackKind::Error:   return BadColor;
		case EHueFeedbackKind::Info:
		default:                        return TextColor;
		}
	}

	/** Small text marker so the kind is readable without relying on colour alone. */
	static const TCHAR* FeedbackPrefix(EHueFeedbackKind Kind)
	{
		switch (Kind)
		{
		case EHueFeedbackKind::Success: return TEXT("+ ");
		case EHueFeedbackKind::Warning: return TEXT("! ");
		case EHueFeedbackKind::Error:   return TEXT("x ");
		case EHueFeedbackKind::Info:
		default:                        return TEXT("- ");
		}
	}

	static const TCHAR* ColorMeaning(EHueColor Color)
	{
		switch (Color)
		{
		case EHueColor::A: return TEXT("repel");
		case EHueColor::B: return TEXT("attract");
		case EHueColor::N:
		default:           return TEXT("brake");
		}
	}

	static const TCHAR* PortKindText(EHuePortKind Kind)
	{
		switch (Kind)
		{
		case EHuePortKind::Input:  return TEXT("in");
		case EHuePortKind::Output: return TEXT("out");
		case EHuePortKind::Both:
		default:                   return TEXT("in/out");
		}
	}

	/** "Ports: West in, East out" for the block's current facing. */
	static FString PortsText(const APulseBlock& Block)
	{
		TArray<FHuePort> Ports;
		Block.GetPorts(Ports);
		if (Ports.Num() == 0)
		{
			return TEXT("No ports (blocks every face)");
		}

		TArray<FString> Parts;
		Parts.Reserve(Ports.Num());
		for (const FHuePort& Port : Ports)
		{
			Parts.Add(FString::Printf(TEXT("%s %s"), HueDir::ToString(Port.Face), PortKindText(Port.Kind)));
		}
		return TEXT("Ports: ") + FString::Join(Parts, TEXT(", "));
	}

	struct FBinding
	{
		const TCHAR* Keys;
		const TCHAR* Action;
	};

	/** Mirrors the default bindings documented in UHueInputConfig. */
	static const FBinding Bindings[] =
	{
		{ TEXT("W A S D"),    TEXT("Move") },
		{ TEXT("Mouse"),      TEXT("Look") },
		{ TEXT("Left mouse"), TEXT("Collect pickup / collect block / place module") },
		{ TEXT("Q / E"),      TEXT("Rotate target block or preview -90 / +90") },
		{ TEXT("Z / X"),      TEXT("Delay of targeted delay block -/+ 0.25 s") },
		{ TEXT("Wheel, 1-6"), TEXT("Select hotbar slot") },
		{ TEXT("G"),          TEXT("Drop one selected module") },
		{ TEXT("Space"),      TEXT("Start / pause / resume the run of this board") },
		{ TEXT("R"),          TEXT("Stop and reset the attempt (layout kept)") },
		{ TEXT("H"),          TEXT("Toggle this help") },
		{ TEXT("Esc / P"),    TEXT("Pause menu (Esc ends Play-In-Editor: use P)") },
	};
}

// ---- Frame --------------------------------------------------------------------------------------------------

void AHueboxHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas || !GEngine)
	{
		return;
	}

	UIScale = FMath::Clamp(Canvas->ClipY / 1080.f, 0.75f, 2.f);
	LeftColumnBottom = HueHUD::Margin * UIScale;

	AHueboxCharacter* Character = Cast<AHueboxCharacter>(GetOwningPawn());
	AExhibitManager* Manager = AExhibitManager::Get(this);

	DrawBoardPanel(Character, Manager);
	DrawFeedback(Manager);

	if (Character)
	{
		DrawTargetPanel(Character);
		DrawHotbar(Character);

		if (Character->IsHelpVisible())
		{
			DrawHelp();
		}
		else
		{
			const FString Hint = TEXT("H  Help");
			const FVector2f Size = HueHUD::Measure(*this, Hint, HueHUD::BodyFont(), UIScale);
			const float PadPx = HueHUD::Pad * UIScale;
			DrawLabel(Hint, Canvas->ClipX - HueHUD::Margin * UIScale - Size.X - 2.f * PadPx,
				Canvas->ClipY - HueHUD::Margin * UIScale - Size.Y - 2.f * PadPx, HueHUD::DimColor, UIScale);
		}

		DrawCrosshairAndPrompt(Character);

		if (Character->IsPauseMenuOpen())
		{
			DrawPauseOverlay();
		}
	}
}

// ---- Crosshair ----------------------------------------------------------------------------------------------

void AHueboxHUD::DrawCrosshairAndPrompt(AHueboxCharacter* Character)
{
	if (!Character)
	{
		return;
	}

	const float S = UIScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const FHueInteractionTarget& Target = Character->GetTarget();

	FLinearColor Color(1.f, 1.f, 1.f, 0.85f);
	FString Prompt;

	switch (Target.Kind)
	{
	case EHueTargetKind::Block:
		if (APulseBlock* Block = Target.Block.Get())
		{
			Color = HueHUD::AmberColor;
			Prompt = IHueInteractable::Execute_GetInteractPrompt(Block, Character);
		}
		break;

	case EHueTargetKind::Pickup:
		if (AHueModulePickup* Pickup = Target.Pickup.Get())
		{
			Color = HueHUD::AmberColor;
			Prompt = IHueInteractable::Execute_GetInteractPrompt(Pickup, Character);
		}
		break;

	case EHueTargetKind::Cell:
	{
		const UHueInventoryComponent* Inventory = Character->GetInventory();
		const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
		if (Target.bCanPlace)
		{
			Color = HueHUD::GoodColor;
			Prompt = FString::Printf(TEXT("LMB  Place %s (facing %s)"), HueModule::DisplayName(Selected),
				HueDir::ToString(Character->GetPreviewFacing()));
		}
		else if (Selected == EHueModuleType::None)
		{
			// Nothing selected is a hint, not an error.
			Color = HueHUD::DimColor;
			Prompt = Target.Reason;
		}
		else
		{
			// Explain blocked placements with a symbol and a brief text (GDD 7).
			Color = HueHUD::BadColor;
			Prompt = TEXT("x  ") + Target.Reason;
		}
		break;
	}

	case EHueTargetKind::None:
	default:
		break;
	}

	const float Length = 7.f * S;
	const float Gap = 3.f * S;
	const float Thickness = FMath::Max(1.f, 2.f * S);
	DrawLine(CX - Gap - Length, CY, CX - Gap, CY, Color, Thickness);
	DrawLine(CX + Gap, CY, CX + Gap + Length, CY, Color, Thickness);
	DrawLine(CX, CY - Gap - Length, CX, CY - Gap, Color, Thickness);
	DrawLine(CX, CY + Gap, CX, CY + Gap + Length, Color, Thickness);
	DrawRect(Color, CX - S, CY - S, 2.f * S, 2.f * S);

	if (!Prompt.IsEmpty())
	{
		const FVector2f Size = HueHUD::Measure(*this, Prompt, HueHUD::BodyFont(), S);
		const float PadPx = HueHUD::Pad * S;
		DrawLabel(Prompt, CX - Size.X * 0.5f - PadPx, CY + 22.f * S, Target.Kind == EHueTargetKind::Cell ? Color : HueHUD::TextColor, S);
	}
}

// ---- Board / objectives -------------------------------------------------------------------------------------

void AHueboxHUD::DrawBoardPanel(AHueboxCharacter* Character, AExhibitManager* Manager)
{
	const float S = UIScale;
	const float X = HueHUD::Margin * S;
	const float Y = HueHUD::Margin * S;
	const float ContentWidth = 440.f * S;

	EHueRunState State = EHueRunState::Build;
	AHueBoard* Board = nullptr;
	if (Manager)
	{
		State = Manager->GetState();
		// While a run exists the panel follows the running board; in Build it follows the player.
		if (State != EHueRunState::Build)
		{
			Board = Manager->GetActiveBoard();
		}
		if (!Board && Character)
		{
			Board = Manager->FindBoardNear(Character->GetActorLocation());
		}
	}

	TArray<HueHUD::FLine> Lines;

	if (!IsValid(Board))
	{
		HueHUD::AddLine(Lines, TEXT("Huebox - prototype table"), HueHUD::AmberColor, true);
		HueHUD::AddLine(Lines, TEXT("Walk up to an exercise board to read its brief."), HueHUD::TextColor);
		const FVector2f Size = HueHUD::DrawPanel(*this, Lines, X, Y, S, ContentWidth, false);
		LeftColumnBottom = Y + Size.Y;
		return;
	}

	// Title + state ------------------------------------------------------------------------------------------
	FString Title = Board->GetBoardName();
	if (Board->IsCompleted())
	{
		Title += TEXT("  (done)");
	}
	HueHUD::AddLine(Lines, Title, Board->IsCompleted() ? HueHUD::GoodColor : HueHUD::AmberColor, true);

	const double RunTime = Manager ? Manager->GetRunTimeSeconds() : 0.0;
	const float Limit = Board->RunTimeLimitSeconds;
	switch (State)
	{
	case EHueRunState::Build:
		HueHUD::AddLine(Lines, TEXT("BUILD    Space: run    R: reset"), HueHUD::TextColor);
		break;
	case EHueRunState::Running:
		HueHUD::AddLine(Lines, FString::Printf(TEXT("RUNNING  %.2f / %.0f s    Space: pause    R: reset"), RunTime, Limit), HueHUD::GoodColor);
		break;
	case EHueRunState::Paused:
		HueHUD::AddLine(Lines, FString::Printf(TEXT("PAUSED  %.2f / %.0f s    Space: resume    R: reset"), RunTime, Limit), HueHUD::WarnColor);
		break;
	case EHueRunState::Ended:
		HueHUD::AddLine(Lines, FString::Printf(TEXT("ENDED at %.2f s    R: reset    Space: run again"), RunTime), HueHUD::BadColor);
		break;
	case EHueRunState::Complete:
		HueHUD::AddLine(Lines, TEXT("COMPLETE!    Space: replay    R: reset"), HueHUD::GoodColor);
		break;
	default:
		break;
	}

	if (State == EHueRunState::Ended || State == EHueRunState::Complete)
	{
		HueHUD::AddLine(Lines, TEXT("Walk to the next board to continue"), HueHUD::InfoColor);
	}

	if (State == EHueRunState::Running || State == EHueRunState::Paused)
	{
		if (const UPulseSubsystem* Sim = UPulseSubsystem::Get(this))
		{
			HueHUD::AddLine(Lines, FString::Printf(TEXT("Signals %d / %d"), Sim->GetActiveSignalCount(), Board->MaxActiveSignals), HueHUD::DimColor);
		}
	}

	// Brief --------------------------------------------------------------------------------------------------
	if (!Board->GetBrief().IsEmpty())
	{
		HueHUD::AddWrapped(*this, Lines, Board->GetBrief(), HueHUD::DimColor, ContentWidth, S);
	}

	// Ordered objectives -------------------------------------------------------------------------------------
	const TArray<FHueObjective>& Objectives = Board->GetObjectives();
	if (Objectives.Num() > 0)
	{
		HueHUD::AddLine(Lines, FString(), HueHUD::TextColor);
		const int32 Current = Board->GetCurrentObjectiveIndex();
		for (int32 Index = 0; Index < Objectives.Num(); ++Index)
		{
			const FHueObjective& Objective = Objectives[Index];
			if (Index < Current)
			{
				HueHUD::AddWrapped(*this, Lines, TEXT("[x] ") + Objective.Description, HueHUD::GoodColor, ContentWidth, S);
			}
			else if (Index == Current)
			{
				FString Text = TEXT("[>] ") + Objective.Description;
				if (Objective.Type == EHueObjectiveType::StopInZone)
				{
					Text += FString::Printf(TEXT(" (hold %.1f/%.1f s)"), Board->GetHoldProgress(), Objective.HoldSeconds);
				}
				HueHUD::AddWrapped(*this, Lines, Text, HueHUD::AmberColor, ContentWidth, S);
			}
			else
			{
				HueHUD::AddWrapped(*this, Lines, TEXT("[ ] ") + Objective.Description, HueHUD::DimColor, ContentWidth, S);
			}
		}
	}

	// Mechanism state ----------------------------------------------------------------------------------------
	if (const AReactiveRig* Rig = Board->GetRig())
	{
		const EHueColor RigColor = Rig->GetCurrentColor();
		HueHUD::AddLine(Lines, FString(), HueHUD::TextColor);
		HueHUD::AddLine(Lines, FString::Printf(TEXT("%s: %s (%s)   %.0f%% of track   %.2f m/s   max %.0f%%"),
			*Rig->GetPerformerName(), HueColor::Label(RigColor), HueHUD::ColorMeaning(RigColor),
			Rig->GetCarriageFraction() * 100.f, Rig->GetCarriageSpeed() / 100.f, Rig->GetAttemptMaxFraction() * 100.f),
			HueColor::ToLinear(RigColor));
	}

	const FVector2f Size = HueHUD::DrawPanel(*this, Lines, X, Y, S, ContentWidth, false);
	LeftColumnBottom = Y + Size.Y;
}

// ---- Feedback -----------------------------------------------------------------------------------------------

void AHueboxHUD::DrawFeedback(AExhibitManager* Manager)
{
	if (!Manager)
	{
		return;
	}

	const TArray<FHueFeedback>& Feedback = Manager->GetRecentFeedback();
	if (Feedback.Num() == 0)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetRealTimeSeconds() : 0.0;

	const float S = UIScale;
	const float X = HueHUD::Margin * S;
	const float MaxWidth = 520.f * S;
	float Y = LeftColumnBottom + HueHUD::PanelGap * S;

	// Oldest first, newest at the bottom; lines hold, then fade out (real time, so they also fade while paused).
	for (const FHueFeedback& Entry : Feedback)
	{
		const double Age = Now - Entry.RealTimeSeconds;
		float Alpha = 1.f;
		if (Age > HueHUD::FeedbackHoldSeconds)
		{
			Alpha = 1.f - static_cast<float>((Age - HueHUD::FeedbackHoldSeconds) / HueHUD::FeedbackFadeSeconds);
		}
		if (Alpha <= 0.01f)
		{
			continue;
		}

		TArray<HueHUD::FLine> Lines;
		HueHUD::AddWrapped(*this, Lines, HueHUD::FeedbackPrefix(Entry.Kind) + Entry.Message, HueHUD::FeedbackColor(Entry.Kind), MaxWidth, S);
		const FVector2f Size = HueHUD::DrawPanel(*this, Lines, X, Y, S, 0.f, false, FMath::Clamp(Alpha, 0.f, 1.f));
		Y += Size.Y + 3.f * S;
	}
}

// ---- Hotbar -------------------------------------------------------------------------------------------------

void AHueboxHUD::DrawHotbar(AHueboxCharacter* Character)
{
	const UHueInventoryComponent* Inventory = Character ? Character->GetInventory() : nullptr;
	if (!Inventory)
	{
		return;
	}

	const AExhibitManager* Manager = AExhibitManager::Get(this);
	const bool bEditing = Manager == nullptr || Manager->IsEditingAllowed();
	// Run HUD: construction controls are dimmed, not hidden, so the player still sees the tray.
	const float Alpha = bEditing ? 1.f : 0.5f;

	const TArray<FHueInventorySlot>& Slots = Inventory->GetSlots();
	const int32 NumSlots = FMath::Max(Slots.Num(), Inventory->GetNumSlots());
	if (NumSlots <= 0)
	{
		return;
	}

	const float S = UIScale;
	const float SlotW = 92.f * S;
	const float SlotH = 62.f * S;
	const float Gap = 8.f * S;
	const float TotalW = NumSlots * SlotW + (NumSlots - 1) * Gap;
	const float X0 = (Canvas->ClipX - TotalW) * 0.5f;
	const float Y0 = Canvas->ClipY - SlotH - HueHUD::Margin * S;
	UFont* Font = HueHUD::BodyFont();

	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		const FHueInventorySlot Slot = Slots.IsValidIndex(Index) ? Slots[Index] : FHueInventorySlot();
		const bool bSelected = Index == Inventory->GetSelectedIndex();
		const float SX = X0 + Index * (SlotW + Gap);

		DrawRect(HueHUD::WithAlpha(HueHUD::Backing, Alpha), SX, Y0, SlotW, SlotH);
		if (bSelected)
		{
			HueHUD::DrawFrame(*this, SX, Y0, SlotW, SlotH, FMath::Max(1.f, 2.f * S), HueHUD::WithAlpha(HueHUD::AmberColor, Alpha));
		}

		DrawText(FString::FromInt(Index + 1), HueHUD::WithAlpha(HueHUD::DimColor, Alpha), SX + 5.f * S, Y0 + 3.f * S, Font, 0.9f * S);

		if (!Slot.IsEmpty())
		{
			// Colour swatch matching the placeholder block body.
			DrawRect(HueHUD::WithAlpha(HueModule::BodyColor(Slot.Type), Alpha), SX + SlotW - 18.f * S, Y0 + 6.f * S, 12.f * S, 12.f * S);

			const FString Name = HueModule::ShortName(Slot.Type);
			const FVector2f NameSize = HueHUD::Measure(*this, Name, Font, S);
			DrawText(Name, HueHUD::WithAlpha(HueHUD::TextColor, Alpha), SX + (SlotW - NameSize.X) * 0.5f, Y0 + SlotH * 0.40f - NameSize.Y * 0.5f, Font, S);

			const FString CountText = FString::Printf(TEXT("x%d"), Slot.Count);
			const FVector2f CountSize = HueHUD::Measure(*this, CountText, Font, S);
			DrawText(CountText, HueHUD::WithAlpha(HueHUD::AmberColor, Alpha), SX + (SlotW - CountSize.X) * 0.5f, Y0 + SlotH * 0.72f - CountSize.Y * 0.5f, Font, S);
		}
		else
		{
			const FString Empty = TEXT("-");
			const FVector2f EmptySize = HueHUD::Measure(*this, Empty, Font, S);
			DrawText(Empty, HueHUD::WithAlpha(HueHUD::DimColor, Alpha), SX + (SlotW - EmptySize.X) * 0.5f, Y0 + (SlotH - EmptySize.Y) * 0.5f, Font, S);
		}
	}

	// Selection / facing line above the slots.
	const EHueModuleType Selected = Inventory->GetSelectedType();
	const TCHAR* FacingName = HueDir::ToString(Character->GetPreviewFacing());
	FString Info;
	FLinearColor InfoColorValue = HueHUD::TextColor;
	if (!bEditing)
	{
		Info = TEXT("Editing is locked during a run - press R to reset");
		InfoColorValue = HueHUD::WarnColor;
	}
	else if (Selected != EHueModuleType::None)
	{
		Info = FString::Printf(TEXT("%s    Q/E rotate: facing %s"), HueModule::DisplayName(Selected), FacingName);
	}
	else
	{
		// Only send the player looking for supplies when the tray really is empty.
		const bool bHasModules = Slots.ContainsByPredicate([](const FHueInventorySlot& Each) { return !Each.IsEmpty(); });
		Info = bHasModules
			? FString::Printf(TEXT("Slot empty - select a module (1-6 / wheel)    Q/E rotate: facing %s"), FacingName)
			: FString::Printf(TEXT("Empty slot - collect modules with LMB    Q/E rotate: facing %s"), FacingName);
		InfoColorValue = HueHUD::DimColor;
	}

	const FVector2f InfoSize = HueHUD::Measure(*this, Info, Font, S);
	const float PadPx = HueHUD::Pad * S;
	DrawLabel(Info, (Canvas->ClipX - InfoSize.X) * 0.5f - PadPx, Y0 - InfoSize.Y - 2.f * PadPx - 6.f * S, InfoColorValue, S);
}

// ---- Target -------------------------------------------------------------------------------------------------

void AHueboxHUD::DrawTargetPanel(AHueboxCharacter* Character)
{
	if (!Character)
	{
		return;
	}

	const FHueInteractionTarget& Target = Character->GetTarget();
	const float S = UIScale;
	const float Right = Canvas->ClipX - HueHUD::Margin * S;
	const float Top = HueHUD::Margin * S;
	const float Width = 380.f * S;

	TArray<HueHUD::FLine> Lines;

	switch (Target.Kind)
	{
	case EHueTargetKind::Block:
	{
		const APulseBlock* Block = Target.Block.Get();
		if (!Block)
		{
			return;
		}

		const FIntPoint Cell = Block->GetCell();
		HueHUD::AddLine(Lines, FString::Printf(TEXT("%s  (%d,%d)"), *Block->GetDisplayName(), Cell.X, Cell.Y), HueHUD::AmberColor, true);
		HueHUD::AddWrapped(*this, Lines, Block->GetRuleText(), HueHUD::TextColor, Width, S);

		const FString StateText = Block->GetStateText();
		if (!StateText.IsEmpty())
		{
			HueHUD::AddWrapped(*this, Lines, StateText, HueHUD::InfoColor, Width, S);
		}

		HueHUD::AddLine(Lines, FString::Printf(TEXT("Facing %s"), HueDir::ToString(Block->GetFacing())), HueHUD::DimColor);
		HueHUD::AddWrapped(*this, Lines, HueHUD::PortsText(*Block), HueHUD::DimColor, Width, S);

		const AExhibitManager* Manager = AExhibitManager::Get(this);
		const bool bCanEdit = !Manager || Manager->IsEditingAllowed();

		if (const AHueDelayBlock* Delay = Cast<AHueDelayBlock>(Block))
		{
			HueHUD::AddLine(Lines, bCanEdit
				? FString::Printf(TEXT("Delay %.2f s    Z / X: -/+ %.2f s"), Delay->GetDelaySeconds(), AHueDelayBlock::DelayStep)
				: FString::Printf(TEXT("Delay %.2f s"), Delay->GetDelaySeconds()), HueHUD::TextColor);
		}

		if (Block->IsRemovable())
		{
			HueHUD::AddLine(Lines, bCanEdit ? TEXT("Removable    LMB collect    Q/E rotate") : TEXT("Removable - press R to reset before editing"),
				bCanEdit ? HueHUD::GoodColor : HueHUD::DimColor);
		}
		else
		{
			HueHUD::AddLine(Lines, TEXT("Fixed unit"), HueHUD::DimColor);
		}
		break;
	}

	case EHueTargetKind::Pickup:
	{
		const AHueModulePickup* Pickup = Target.Pickup.Get();
		if (!Pickup)
		{
			return;
		}
		HueHUD::AddLine(Lines, FString::Printf(TEXT("Supply: %d x %s"), Pickup->GetCount(), HueModule::DisplayName(Pickup->GetModuleType())), HueHUD::AmberColor, true);
		HueHUD::AddLine(Lines, TEXT("LMB  pick up into the hotbar"), HueHUD::TextColor);
		break;
	}

	case EHueTargetKind::Cell:
	{
		HueHUD::AddLine(Lines, FString::Printf(TEXT("Empty cell (%d,%d)"), Target.Cell.X, Target.Cell.Y), HueHUD::AmberColor, true);
		const UHueInventoryComponent* Inventory = Character->GetInventory();
		const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
		if (Target.bCanPlace)
		{
			HueHUD::AddLine(Lines, FString::Printf(TEXT("LMB  place %s facing %s"), HueModule::DisplayName(Selected),
				HueDir::ToString(Character->GetPreviewFacing())), HueHUD::GoodColor);
		}
		else
		{
			HueHUD::AddWrapped(*this, Lines, Target.Reason,
				Selected == EHueModuleType::None ? HueHUD::DimColor : HueHUD::BadColor, Width, S);
		}
		break;
	}

	case EHueTargetKind::None:
	default:
		return;
	}

	HueHUD::DrawPanel(*this, Lines, Right, Top, S, 240.f * S, /*bAlignRight*/ true);
}

// ---- Overlays -----------------------------------------------------------------------------------------------

void AHueboxHUD::DrawHelp()
{
	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const float GapPx = HueHUD::LineGap * S;
	const float ColumnGap = 16.f * S;
	UFont* Font = HueHUD::BodyFont();

	const FString Heading = TEXT("Controls  (H to hide)");
	const FVector2f HeadingSize = HueHUD::Measure(*this, Heading, HueHUD::HeadingFont(), S);

	float KeysWidth = 0.f;
	float ActionWidth = 0.f;
	float RowHeight = 0.f;
	for (const HueHUD::FBinding& Binding : HueHUD::Bindings)
	{
		const FVector2f KeySize = HueHUD::Measure(*this, Binding.Keys, Font, S);
		const FVector2f ActionSize = HueHUD::Measure(*this, Binding.Action, Font, S);
		KeysWidth = FMath::Max(KeysWidth, static_cast<float>(KeySize.X));
		ActionWidth = FMath::Max(ActionWidth, static_cast<float>(ActionSize.X));
		RowHeight = FMath::Max(RowHeight, static_cast<float>(FMath::Max(KeySize.Y, ActionSize.Y)));
	}

	const int32 NumRows = UE_ARRAY_COUNT(HueHUD::Bindings);
	const float BoxWidth = FMath::Max(static_cast<float>(HeadingSize.X), KeysWidth + ColumnGap + ActionWidth) + 2.f * PadPx;
	const float BoxHeight = static_cast<float>(HeadingSize.Y) + GapPx * 2.f + NumRows * (RowHeight + GapPx) + 2.f * PadPx;
	const float Left = Canvas->ClipX - HueHUD::Margin * S - BoxWidth;
	const float Top = Canvas->ClipY - HueHUD::Margin * S - BoxHeight;

	DrawRect(HueHUD::Backing, Left, Top, BoxWidth, BoxHeight);
	DrawText(Heading, HueHUD::AmberColor, Left + PadPx, Top + PadPx, HueHUD::HeadingFont(), S);

	float RowY = Top + PadPx + static_cast<float>(HeadingSize.Y) + GapPx * 2.f;
	for (const HueHUD::FBinding& Binding : HueHUD::Bindings)
	{
		DrawText(Binding.Keys, HueHUD::AmberColor, Left + PadPx, RowY, Font, S);
		DrawText(Binding.Action, HueHUD::TextColor, Left + PadPx + KeysWidth + ColumnGap, RowY, Font, S);
		RowY += RowHeight + GapPx;
	}
}

void AHueboxHUD::DrawPauseOverlay()
{
	const float S = UIScale;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), 0.f, 0.f, Canvas->ClipX, Canvas->ClipY);

	const FString Title = TEXT("PAUSED");
	const float TitleScale = 2.f * S;
	const FVector2f TitleSize = HueHUD::Measure(*this, Title, HueHUD::HeadingFont(), TitleScale);
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	DrawText(Title, HueHUD::AmberColor, CX - TitleSize.X * 0.5f, CY - TitleSize.Y - 20.f * S, HueHUD::HeadingFont(), TitleScale);

	TArray<HueHUD::FLine> Lines;
	HueHUD::AddLine(Lines, TEXT("Esc / P    Resume"), HueHUD::TextColor);
	HueHUD::AddLine(Lines, TEXT("The simulation, the carriage and the run timer are frozen."), HueHUD::DimColor);
	HueHUD::AddLine(Lines, TEXT("Close the window to quit (in the editor: Shift+F1, then Stop)."), HueHUD::DimColor);

	float MaxWidth = 0.f;
	for (const HueHUD::FLine& Line : Lines)
	{
		MaxWidth = FMath::Max(MaxWidth, static_cast<float>(HueHUD::Measure(*this, Line.Text, HueHUD::BodyFont(), S).X));
	}
	const float BoxWidth = MaxWidth + 2.f * HueHUD::Pad * S;
	HueHUD::DrawPanel(*this, Lines, CX - BoxWidth * 0.5f, CY, S, MaxWidth, false);
}

float AHueboxHUD::DrawLabel(const FString& Text, float X, float Y, const FLinearColor& Color, float Scale, bool bBacking)
{
	if (!Canvas || Text.IsEmpty())
	{
		return 0.f;
	}

	TArray<FString> Rows;
	Text.ParseIntoArray(Rows, TEXT("\n"), /*InCullEmpty*/ false);

	UFont* Font = HueHUD::BodyFont();
	const float PadPx = bBacking ? HueHUD::Pad * Scale : 0.f;
	const float GapPx = HueHUD::LineGap * Scale;

	float Width = 0.f;
	float Height = 0.f;
	TArray<float, TInlineAllocator<8>> RowHeights;
	for (const FString& Row : Rows)
	{
		const FVector2f Size = HueHUD::Measure(*this, Row, Font, Scale);
		Width = FMath::Max(Width, static_cast<float>(Size.X));
		RowHeights.Add(static_cast<float>(Size.Y));
		Height += static_cast<float>(Size.Y);
	}
	Height += GapPx * FMath::Max(0, Rows.Num() - 1);

	if (bBacking)
	{
		// The backing fades together with the text.
		DrawRect(HueHUD::WithAlpha(HueHUD::Backing, Color.A), X, Y, Width + 2.f * PadPx, Height + 2.f * PadPx);
	}

	float CursorY = Y + PadPx;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (!Rows[Index].IsEmpty())
		{
			DrawText(Rows[Index], Color, X + PadPx, CursorY, Font, Scale);
		}
		CursorY += RowHeights[Index] + GapPx;
	}

	return Height + 2.f * PadPx;
}
