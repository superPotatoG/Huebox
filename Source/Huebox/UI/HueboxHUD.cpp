#include "UI/HueboxHUD.h"

#include "Blocks/HueFixedBlocks.h"
#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "GlobalRenderResources.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Game/ExhibitManager.h"
#include "GameFramework/Pawn.h"
#include "Grid/HueBoard.h"
#include "Player/HueBuildToolComponent.h"
#include "Player/HueBuilderPawn.h"
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

	/** Minimap / tray icons use the look of the in-world "2D white props": white tiles, dark letters. */
	static const FLinearColor TileWhite(0.92f, 0.92f, 0.9f);
	static const FLinearColor TileInk(0.08f, 0.08f, 0.08f);
	static const FLinearColor BarrierFill(0.03f, 0.03f, 0.035f);
	static const FLinearColor BarrierEdge(0.42f, 0.42f, 0.42f);
	static const FLinearColor MapFloor(0.17f, 0.15f, 0.13f, 0.92f);
	static const FLinearColor MapLine(0.36f, 0.33f, 0.3f, 0.9f);

	static constexpr float Margin = 24.f;
	static constexpr float Pad = 8.f;
	static constexpr float LineGap = 2.f;
	static constexpr float PanelGap = 12.f;
	static constexpr float FeedbackHoldSeconds = 6.f;
	static constexpr float FeedbackFadeSeconds = 2.f;

	/** Edge of the minimap grid at UIScale 1 (the longer grid side fills it). */
	static constexpr float MinimapSize = 300.f;
	/** Block tile edge as a fraction of the cell (the gap shows the grid lines). */
	static constexpr float MapTileFraction = 0.8f;
	static constexpr int32 CircleSides = 16;

	/** Hint bar spacing between a key and its action, and between two pairs. */
	static constexpr float HintKeyGap = 5.f;
	static constexpr float HintPairGap = 18.f;

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

	/** Widest line of Lines (heading lines measured with the heading font). */
	static float MeasureLinesWidth(const AHUD& HUD, const TArray<FLine>& Lines, float Scale)
	{
		float Width = 0.f;
		for (const FLine& Line : Lines)
		{
			Width = FMath::Max(Width, static_cast<float>(Measure(HUD, Line.Text, Line.bHeading ? HeadingFont() : BodyFont(), Scale).X));
		}
		return Width;
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

	/** Text centred on (CenterX, CenterY), scaled so that its line height is about Height pixels. */
	static void DrawCentredText(AHUD& HUD, const FString& Text, float CenterX, float CenterY, float Height, const FLinearColor& Color)
	{
		if (Text.IsEmpty() || Height < 2.f)
		{
			return;
		}

		UFont* Font = BodyFont();
		const FVector2f Unit = Measure(HUD, Text, Font, 1.f);
		if (Unit.Y <= 0.f)
		{
			return;
		}
		const float Scale = Height / Unit.Y;
		HUD.DrawText(Text, Color, CenterX - Unit.X * Scale * 0.5f, CenterY - Unit.Y * Scale * 0.5f, Font, Scale);
	}

	/** Filled square with a centred letter (tray icons). */
	static void DrawLetterTile(AHUD& HUD, float X, float Y, float Size, const FLinearColor& Fill, const FString& Letter, const FLinearColor& Ink)
	{
		HUD.DrawRect(Fill, X, Y, Size, Size);
		DrawCentredText(HUD, Letter, X + Size * 0.5f, Y + Size * 0.5f, Size * 0.8f, Ink);
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

	/** Short form of HueColor::MotionMeaning for the status line. */
	static const TCHAR* ColorMeaning(EHueColor Color)
	{
		switch (Color)
		{
		case EHueColor::A: return TEXT("pushed away");
		case EHueColor::B: return TEXT("pulled back");
		case EHueColor::N:
		default:           return TEXT("brakes");
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

	/** One-letter module code shared by the tray icons and the minimap. */
	static const TCHAR* ModuleLetter(EHueModuleType Type)
	{
		switch (Type)
		{
		case EHueModuleType::Turn:       return TEXT("T");
		case EHueModuleType::ColorShift: return TEXT("X"); // "C" is cyan light now
		case EHueModuleType::Delay:      return TEXT("D");
		case EHueModuleType::Mixer:      return TEXT("M");
		case EHueModuleType::None:
		default:                         return TEXT("");
		}
	}

	static bool HasAnyModule(const UHueInventoryComponent& Inventory)
	{
		return Inventory.GetSlots().ContainsByPredicate([](const FHueInventorySlot& Slot) { return !Slot.IsEmpty(); });
	}

	/** How one block looks on the minimap. */
	struct FMapTile
	{
		FLinearColor Fill = TileWhite;
		FLinearColor Ink = TileInk;
		FString Letter;
		/** Barrier: dark filled cell, no letter, no ports. */
		bool bSolid = false;
		/** A delay buffer / mixer wait holds a signal (not travelling, so not among the pulse snapshots). */
		bool bHolding = false;
	};

	static FMapTile DescribeMapTile(const APulseBlock& Block)
	{
		FMapTile Tile;
		if (const AHueSourceBlock* Source = Cast<AHueSourceBlock>(&Block))
		{
			Tile.Fill = HueColor::ToLinear(Source->GetEmitColor());
			Tile.Letter = TEXT("S");
		}
		else if (const AHueReceiverBlock* Receiver = Cast<AHueReceiverBlock>(&Block))
		{
			Tile.Fill = HueColor::ToLinear(Receiver->GetCurrentColor());
			Tile.Letter = TEXT("@"); // "R" is red light now
		}
		else if (Block.IsA<AHueBarrierBlock>())
		{
			Tile.Fill = BarrierFill;
			Tile.bSolid = true;
		}
		else if (const AHueBrickBlock* Brick = Cast<AHueBrickBlock>(&Block))
		{
			// Standing: a tile in the brick's colour marked "#" (a source of that colour shows "S"). Broken: dimmed, empty.
			const FLinearColor BrickColor = HueColor::ToLinear(Brick->GetBrickColor());
			if (Brick->IsBroken())
			{
				Tile.Fill = FLinearColor(BrickColor.R * 0.3f, BrickColor.G * 0.3f, BrickColor.B * 0.3f, 1.f);
			}
			else
			{
				Tile.Fill = BrickColor;
				Tile.Letter = TEXT("#");
			}
		}
		else
		{
			Tile.Letter = ModuleLetter(Block.GetModuleType());
			if (Tile.Letter.IsEmpty())
			{
				// Unknown kind (e.g. a future subclass): first letter of its name on a gray tile.
				Tile.Fill = FLinearColor(0.6f, 0.6f, 0.6f);
				Tile.Letter = Block.GetDisplayName().Left(1).ToUpper();
			}

			if (const AHueDelayBlock* Delay = Cast<AHueDelayBlock>(&Block))
			{
				Tile.bHolding = Delay->IsHolding();
			}
			else if (const AHueMixerBlock* Mixer = Cast<AHueMixerBlock>(&Block))
			{
				Tile.bHolding = Mixer->IsWaiting();
			}
		}
		return Tile;
	}

	/** Screen step of a board direction on the top-down map: East = right, North (+Y) = down, South (-Y) = up. */
	static FVector2f MapStep(EHueDir Dir)
	{
		const FIntPoint Offset = HueDir::ToOffset(Dir);
		return FVector2f(static_cast<float>(Offset.X), static_cast<float>(Offset.Y));
	}

	/**
	 * Port mark on a minimap tile centred at (CX, CY): a dark bar just inside the face's edge; outputs and two-way
	 * ports also get a short spout of the tile colour sticking out over the grid, so the exit side reads at a glance.
	 */
	static void DrawPortMark(AHUD& HUD, float CX, float CY, float TileSize, const FHuePort& Port, const FLinearColor& Ink, const FLinearColor& Fill)
	{
		const FVector2f Step = MapStep(Port.Face);
		// North / South faces are horizontal edges on the map.
		const bool bHorizontalEdge = FMath::IsNearlyZero(Step.X);
		const bool bOutput = Port.Kind != EHuePortKind::Input;
		const float Thick = FMath::Max(2.f, TileSize * 0.12f);
		const float Length = TileSize * (bOutput ? 0.42f : 0.3f);

		// Rect Along x Across, oriented to the edge, centred Reach pixels from the tile centre towards the face.
		auto DrawEdgeRect = [&HUD, CX, CY, Step, bHorizontalEdge](float Reach, float Along, float Across, const FLinearColor& Color)
		{
			const float W = bHorizontalEdge ? Along : Across;
			const float H = bHorizontalEdge ? Across : Along;
			HUD.DrawRect(Color, CX + Step.X * Reach - W * 0.5f, CY + Step.Y * Reach - H * 0.5f, W, H);
		};

		DrawEdgeRect(TileSize * 0.5f - Thick * 0.5f, Length, Thick, Ink);
		if (bOutput)
		{
			DrawEdgeRect(TileSize * 0.5f + Thick * 0.5f, Length * 0.5f, Thick, Fill);
		}
	}

	struct FBinding
	{
		const TCHAR* Keys;
		const TCHAR* Action;
	};

	/** Sandbox (first person); mirrors the default bindings documented in UHueInputConfig. */
	static const FBinding SandboxBindings[] =
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

	/** Instruction Build view (UHueInputConfig::BuildContext). */
	static const FBinding BuildBindings[] =
	{
		{ TEXT("Mouse / W A S D"), TEXT("Move the cell cursor (arrow keys too)") },
		{ TEXT("Left mouse"),      TEXT("Place the selected module / collect the block") },
		{ TEXT("Right mouse"),     TEXT("Collect the block back into the tray") },
		{ TEXT("Q / E"),           TEXT("Rotate the block or the placement preview -90 / +90") },
		{ TEXT("Z / X"),           TEXT("Delay of the delay block -/+ 0.25 s") },
		{ TEXT("Wheel, 1-6"),      TEXT("Select tray slot") },
		{ TEXT("Space"),           TEXT("Run this exercise (the view moves to the stage)") },
		{ TEXT("R"),               TEXT("Stop and reset the attempt (layout kept)") },
		{ TEXT("Tab / ]"),         TEXT("Next exercise") },
		{ TEXT("["),               TEXT("Previous exercise") },
		{ TEXT("H"),               TEXT("Toggle this help") },
		{ TEXT("Esc / P"),         TEXT("Pause menu (Esc ends Play-In-Editor: use P)") },
	};

	/** Instruction Watch view (UHueInputConfig::WatchContext). */
	static const FBinding WatchBindings[] =
	{
		{ TEXT("Space"),   TEXT("Pause / resume the run") },
		{ TEXT("R"),       TEXT("Stop, reset and go back to the build view") },
		{ TEXT("Tab / ]"), TEXT("Next exercise (once the run is complete or ended)") },
		{ TEXT("H"),       TEXT("Toggle this help") },
		{ TEXT("Esc / P"), TEXT("Pause menu (Esc ends Play-In-Editor: use P)") },
	};

	/** Watch view once the run is Complete / Ended: Space reruns the board (AExhibitManager::ToggleRun). */
	static const FBinding WatchFinishedBindings[] =
	{
		{ TEXT("Space"),   TEXT("Replay the run from the start") },
		{ TEXT("R"),       TEXT("Reset and go back to the build view") },
		{ TEXT("Tab / ]"), TEXT("Next exercise") },
		{ TEXT("H"),       TEXT("Toggle this help") },
		{ TEXT("Esc / P"), TEXT("Pause menu (Esc ends Play-In-Editor: use P)") },
	};

	static const TCHAR* const BuildHelpFooter = TEXT("Modules come from this board's tray; collected blocks go back into it.");
	static const TCHAR* const WatchHelpFooter = TEXT("Map: S source  @ receiver  T turn  X colour shift  D delay  M mixer  # brick  (dots = pulses, letter = colour)");

	/** Bottom hint bar of the Build view. */
	static const FBinding BuildHints[] =
	{
		{ TEXT("WASD/mouse"), TEXT("select") },
		{ TEXT("LMB"),        TEXT("place/collect") },
		{ TEXT("RMB"),        TEXT("collect") },
		{ TEXT("Q/E"),        TEXT("rotate") },
		{ TEXT("Z/X"),        TEXT("delay") },
		{ TEXT("Space"),      TEXT("run") },
		{ TEXT("R"),          TEXT("reset") },
		{ TEXT("Tab"),        TEXT("next exercise") },
		{ TEXT("H"),          TEXT("help") },
	};

	/** Bottom hint bar of the Watch view. */
	static const FBinding WatchHints[] =
	{
		{ TEXT("Space"), TEXT("pause/resume") },
		{ TEXT("R"),     TEXT("back to build") },
		{ TEXT("Tab"),   TEXT("next exercise (after finishing)") },
		{ TEXT("H"),     TEXT("help") },
	};

	/** Bottom hint bar of the Watch view once the run is Complete / Ended. */
	static const FBinding WatchFinishedHints[] =
	{
		{ TEXT("Space"), TEXT("replay") },
		{ TEXT("R"),     TEXT("back to build") },
		{ TEXT("Tab"),   TEXT("next exercise") },
		{ TEXT("H"),     TEXT("help") },
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
	HintBarTop = Canvas->ClipY - HueHUD::Margin * UIScale;
	HintBarRight = 0.f;

	// Raw pointers below are frame locals: looked up every DrawHUD, never stored.
	APawn* Pawn = GetOwningPawn();
	const EHudContext Context = ResolveContext(Pawn);
	const UHueBuildToolComponent* Tool = Pawn ? Pawn->FindComponentByClass<UHueBuildToolComponent>() : nullptr;
	AExhibitManager* Manager = AExhibitManager::Get(this);
	const EHueRunState RunState = Manager ? Manager->GetState() : EHueRunState::Build;
	bRunFinished = RunState == EHueRunState::Complete || RunState == EHueRunState::Ended;

	DrawBoardPanel(Pawn, Manager, Context);

	switch (Context)
	{
	case EHudContext::Sandbox:
	{
		const AHueboxCharacter* Character = CastChecked<AHueboxCharacter>(Pawn);
		DrawFeedback(Manager);

		if (Tool)
		{
			DrawTargetPanel(Tool, Context);
			DrawHotbar(Tool);
		}

		if (Character->IsHelpVisible())
		{
			DrawHelp(Context);
		}
		else
		{
			const FString Hint = TEXT("H  Help");
			const FVector2f Size = HueHUD::Measure(*this, Hint, HueHUD::BodyFont(), UIScale);
			const float PadPx = HueHUD::Pad * UIScale;
			DrawLabel(Hint, Canvas->ClipX - HueHUD::Margin * UIScale - Size.X - 2.f * PadPx,
				Canvas->ClipY - HueHUD::Margin * UIScale - Size.Y - 2.f * PadPx, HueHUD::DimColor, UIScale);
		}

		if (Tool)
		{
			DrawCrosshairAndPrompt(Tool, Pawn);
		}

		if (Character->IsPauseMenuOpen())
		{
			DrawPauseOverlay();
		}
		break;
	}

	case EHudContext::Build:
	case EHudContext::Watch:
	{
		const AHueBuilderPawn* Builder = CastChecked<AHueBuilderPawn>(Pawn);
		const bool bBuildView = Context == EHudContext::Build;

		// Left column: board panel, (tray), feedback.
		if (bBuildView && Tool)
		{
			DrawTrayPanel(Tool);
		}
		DrawFeedback(Manager);

		// The hint bar goes first: prompt, help, light key and minimap are placed relative to it.
		DrawHintBar(Context);

		if (bBuildView)
		{
			DrawLightKey();
			if (Tool)
			{
				DrawTargetPanel(Tool, Context);
				DrawBuildPrompt(Tool, Pawn);
			}
		}
		else
		{
			DrawMinimap(ResolveBoard(Pawn, Manager));
			DrawRunBanner(Manager);
		}

		if (Builder->IsHelpVisible())
		{
			DrawHelp(Context);
		}

		if (Builder->IsPauseMenuOpen())
		{
			DrawPauseOverlay();
		}
		break;
	}

	case EHudContext::None:
	default:
		DrawFeedback(Manager);
		break;
	}
}

AHueboxHUD::EHudContext AHueboxHUD::ResolveContext(const APawn* Pawn)
{
	if (const AHueBuilderPawn* Builder = Cast<AHueBuilderPawn>(Pawn))
	{
		return Builder->GetViewMode() == EHueViewMode::Watch ? EHudContext::Watch : EHudContext::Build;
	}
	if (Pawn && Pawn->IsA<AHueboxCharacter>())
	{
		return EHudContext::Sandbox;
	}
	return EHudContext::None;
}

AHueBoard* AHueboxHUD::ResolveBoard(const APawn* Pawn, const AExhibitManager* Manager) const
{
	const AHueBuilderPawn* Builder = Cast<AHueBuilderPawn>(Pawn);

	AHueBoard* Board = nullptr;
	if (Manager)
	{
		// While a run exists the panels follow the running board; in Build they follow the player.
		if (Manager->GetState() != EHueRunState::Build)
		{
			Board = Manager->GetActiveBoard();
		}
		// The builder knows its board exactly (also while the camera still glides to it).
		if (!Board && Builder)
		{
			Board = Builder->GetFocusedBoard();
		}
		if (!Board && Pawn)
		{
			Board = Manager->FindBoardNear(Pawn->GetActorLocation());
		}
	}
	else if (Builder)
	{
		Board = Builder->GetFocusedBoard();
	}

	return IsValid(Board) ? Board : nullptr;
}

// ---- Crosshair (Sandbox) ------------------------------------------------------------------------------------

void AHueboxHUD::DrawCrosshairAndPrompt(const UHueBuildToolComponent* Tool, APawn* Pawn)
{
	if (!Tool)
	{
		return;
	}

	const float S = UIScale;
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const FHueInteractionTarget& Target = Tool->GetTarget();

	FLinearColor Color(1.f, 1.f, 1.f, 0.85f);
	FString Prompt;

	switch (Target.Kind)
	{
	case EHueTargetKind::Block:
		if (APulseBlock* Block = Target.Block.Get())
		{
			Color = HueHUD::AmberColor;
			Prompt = IHueInteractable::Execute_GetInteractPrompt(Block, Pawn);
		}
		break;

	case EHueTargetKind::Pickup:
		if (AHueModulePickup* Pickup = Target.Pickup.Get())
		{
			Color = HueHUD::AmberColor;
			Prompt = IHueInteractable::Execute_GetInteractPrompt(Pickup, Pawn);
		}
		break;

	case EHueTargetKind::Cell:
	{
		const UHueInventoryComponent* Inventory = Tool->GetInventory();
		const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
		if (Target.bCanPlace)
		{
			Color = HueHUD::GoodColor;
			Prompt = FString::Printf(TEXT("LMB  Place %s (facing %s)"), HueModule::DisplayName(Selected),
				HueDir::ToString(Tool->GetPreviewFacing()));
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

void AHueboxHUD::DrawBoardPanel(const APawn* Pawn, AExhibitManager* Manager, EHudContext Context)
{
	const float S = UIScale;
	const float X = HueHUD::Margin * S;
	const float Y = HueHUD::Margin * S;
	const float ContentWidth = 440.f * S;
	const bool bInstruction = Context == EHudContext::Build || Context == EHudContext::Watch;

	const EHueRunState State = Manager ? Manager->GetState() : EHueRunState::Build;
	AHueBoard* Board = ResolveBoard(Pawn, Manager);

	TArray<HueHUD::FLine> Lines;

	if (!Board)
	{
		HueHUD::AddLine(Lines, TEXT("Huebox - prototype table"), HueHUD::AmberColor, true);
		HueHUD::AddLine(Lines, bInstruction ? TEXT("Preparing the exercise boards...") : TEXT("Walk up to an exercise board to read its brief."),
			HueHUD::TextColor);
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

	// Instruction progression: which board of how many (boards are kept in progression order; the Level 2 sample shares
	// the table with the exercises for now, so "board", not "exercise").
	if (Context == EHudContext::Build && Manager)
	{
		const int32 Index = Manager->GetBoards().IndexOfByKey(Board);
		const int32 Count = Manager->GetBoards().Num();
		if (Index != INDEX_NONE && Count > 1)
		{
			HueHUD::AddLine(Lines, FString::Printf(TEXT("Board %d of %d    Tab / ]: next    [: previous"), Index + 1, Count), HueHUD::DimColor);
		}
	}

	if (State == EHueRunState::Ended || State == EHueRunState::Complete)
	{
		HueHUD::AddLine(Lines, bInstruction ? TEXT("Tab: next exercise    R: back to build") : TEXT("Walk to the next board to continue"),
			HueHUD::InfoColor);
	}

	if (State == EHueRunState::Running || State == EHueRunState::Paused)
	{
		if (const UPulseSubsystem* Sim = UPulseSubsystem::Get(this))
		{
			HueHUD::AddLine(Lines, FString::Printf(TEXT("Signals %d / %d"), Sim->GetActiveSignalCount(), Board->MaxActiveSignals), HueHUD::DimColor);
		}
	}

	// Brief (the Watch view is about the performance; the brief belongs to building) -------------------------
	if (Context != EHudContext::Watch && !Board->GetBrief().IsEmpty())
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
				else if (Objective.Type == EHueObjectiveType::BreakBoard)
				{
					Text += FString::Printf(TEXT(" (%s from %.0f-%.0f%%, glass at %.0f%%)"), HueColor::Label(Objective.Color),
						Objective.ZoneMin * 100.f, Objective.ZoneMax * 100.f, Objective.TrackFraction * 100.f);
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

// ---- Hotbar (Sandbox) ---------------------------------------------------------------------------------------

void AHueboxHUD::DrawHotbar(const UHueBuildToolComponent* Tool)
{
	const UHueInventoryComponent* Inventory = Tool ? Tool->GetInventory() : nullptr;
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
			// Colour swatch matching the floor pickups (placed modules are all white, so not their body colour).
			DrawRect(HueHUD::WithAlpha(HueModule::AccentColor(Slot.Type), Alpha), SX + SlotW - 18.f * S, Y0 + 6.f * S, 12.f * S, 12.f * S);

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
	const TCHAR* FacingName = HueDir::ToString(Tool->GetPreviewFacing());
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
		const bool bHasModules = HueHUD::HasAnyModule(*Inventory);
		Info = bHasModules
			? FString::Printf(TEXT("Slot empty - select a module (1-6 / wheel)    Q/E rotate: facing %s"), FacingName)
			: FString::Printf(TEXT("Empty slot - collect modules with LMB    Q/E rotate: facing %s"), FacingName);
		InfoColorValue = HueHUD::DimColor;
	}

	const FVector2f InfoSize = HueHUD::Measure(*this, Info, Font, S);
	const float PadPx = HueHUD::Pad * S;
	DrawLabel(Info, (Canvas->ClipX - InfoSize.X) * 0.5f - PadPx, Y0 - InfoSize.Y - 2.f * PadPx - 6.f * S, InfoColorValue, S);
}

// ---- Tray (Build view) --------------------------------------------------------------------------------------

void AHueboxHUD::DrawTrayPanel(const UHueBuildToolComponent* Tool)
{
	const UHueInventoryComponent* Tray = Tool ? Tool->GetInventory() : nullptr;
	if (!Tray)
	{
		return;
	}

	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const float GapPx = HueHUD::LineGap * S;
	const float X = HueHUD::Margin * S;
	const float Y = LeftColumnBottom + HueHUD::PanelGap * S;
	UFont* Font = HueHUD::BodyFont();
	UFont* Heading = HueHUD::HeadingFont();

	// Same rule as the hotbar: outside Build the tray stays readable but dimmed.
	const bool bEditing = Tool->IsEditingAllowed();
	const float Alpha = bEditing ? 1.f : 0.5f;

	const TArray<FHueInventorySlot>& Slots = Tray->GetSlots();
	const int32 NumSlots = FMath::Max(Slots.Num(), Tray->GetNumSlots());
	const bool bHasModules = HueHUD::HasAnyModule(*Tray);

	const FString Title = TEXT("Tray (wheel / 1-6)");
	const FString EmptyText = TEXT("Tray empty - all modules placed");
	const FString Footer = bEditing
		? FString::Printf(TEXT("Facing: %s  (Q/E)"), HueDir::ToString(Tool->GetPreviewFacing()))
		: FString(TEXT("Editing locked - press R to reset"));

	const FVector2f TitleSize = HueHUD::Measure(*this, Title, Heading, S);
	const FVector2f FooterSize = HueHUD::Measure(*this, Footer, Font, S);
	const float LineH = HueHUD::Measure(*this, TEXT("Ag"), Font, S).Y;
	const float RowH = LineH + 6.f * S;
	const float IconSize = LineH;

	// Row columns: slot number | letter tile | short name | count (right-aligned).
	const float NumberW = HueHUD::Measure(*this, TEXT("8"), Font, S).X + 10.f * S;
	const float IconGap = 8.f * S;
	float NameW = 0.f;
	for (const FHueInventorySlot& Slot : Slots)
	{
		if (!Slot.IsEmpty())
		{
			NameW = FMath::Max(NameW, static_cast<float>(HueHUD::Measure(*this, HueModule::ShortName(Slot.Type), Font, S).X));
		}
	}
	const float CountW = HueHUD::Measure(*this, TEXT("x12"), Font, S).X;
	const float RowW = NumberW + IconSize + IconGap + NameW + 16.f * S + CountW;

	float ContentW = FMath::Max3(static_cast<float>(TitleSize.X), static_cast<float>(FooterSize.X), RowW);
	ContentW = FMath::Max(ContentW, 220.f * S);
	if (!bHasModules)
	{
		ContentW = FMath::Max(ContentW, static_cast<float>(HueHUD::Measure(*this, EmptyText, Font, S).X));
	}

	const int32 NumRows = bHasModules ? NumSlots : 1;
	const float BoxW = ContentW + 2.f * PadPx;
	const float BoxH = 2.f * PadPx + TitleSize.Y + 2.f * GapPx + NumRows * RowH + 2.f * GapPx + FooterSize.Y;
	const float ContentX = X + PadPx;

	DrawRect(HueHUD::Backing, X, Y, BoxW, BoxH);
	DrawText(Title, HueHUD::WithAlpha(HueHUD::AmberColor, Alpha), ContentX, Y + PadPx, Heading, S);

	float RowY = Y + PadPx + TitleSize.Y + 2.f * GapPx;
	if (!bHasModules)
	{
		DrawText(EmptyText, HueHUD::WithAlpha(HueHUD::DimColor, Alpha), ContentX, RowY + (RowH - LineH) * 0.5f, Font, S);
		RowY += RowH;
	}
	else
	{
		const int32 SelectedIndex = Tray->GetSelectedIndex();
		for (int32 Index = 0; Index < NumSlots; ++Index)
		{
			const FHueInventorySlot Slot = Slots.IsValidIndex(Index) ? Slots[Index] : FHueInventorySlot();
			const bool bSelected = Index == SelectedIndex;
			const bool bFilled = !Slot.IsEmpty();
			const float TextY = RowY + (RowH - LineH) * 0.5f;

			if (bSelected)
			{
				const float HighlightX = ContentX - 4.f * S;
				const float HighlightW = ContentW + 8.f * S;
				DrawRect(HueHUD::WithAlpha(HueHUD::AmberColor, 0.18f * Alpha), HighlightX, RowY, HighlightW, RowH);
				HueHUD::DrawFrame(*this, HighlightX, RowY, HighlightW, RowH, FMath::Max(1.f, 1.5f * S), HueHUD::WithAlpha(HueHUD::AmberColor, Alpha));
			}

			DrawText(FString::FromInt(Index + 1), HueHUD::WithAlpha(bSelected ? HueHUD::AmberColor : HueHUD::DimColor, Alpha), ContentX, TextY, Font, S);

			const float IconX = ContentX + NumberW;
			const float NameX = IconX + IconSize + IconGap;
			if (bFilled)
			{
				// Same white tile + letter as the module on the board and on the minimap.
				HueHUD::DrawLetterTile(*this, IconX, RowY + (RowH - IconSize) * 0.5f, IconSize, HueHUD::WithAlpha(HueHUD::TileWhite, Alpha),
					HueHUD::ModuleLetter(Slot.Type), HueHUD::WithAlpha(HueHUD::TileInk, Alpha));
				DrawText(HueModule::ShortName(Slot.Type), HueHUD::WithAlpha(bSelected ? HueHUD::AmberColor : HueHUD::TextColor, Alpha), NameX, TextY, Font, S);

				const FString CountText = FString::Printf(TEXT("x%d"), Slot.Count);
				const float CountX = ContentX + ContentW - HueHUD::Measure(*this, CountText, Font, S).X;
				DrawText(CountText, HueHUD::WithAlpha(HueHUD::AmberColor, Alpha), CountX, TextY, Font, S);
			}
			else
			{
				// Empty slots stay listed (dimmed) so the 1-6 keys keep matching the rows.
				DrawText(TEXT("-"), HueHUD::WithAlpha(HueHUD::DimColor, Alpha), NameX, TextY, Font, S);
			}
			RowY += RowH;
		}
	}

	DrawText(Footer, bEditing ? HueHUD::TextColor : HueHUD::WarnColor, ContentX, RowY + 2.f * GapPx, Font, S);
	LeftColumnBottom = Y + BoxH;
}

// ---- Target -------------------------------------------------------------------------------------------------

void AHueboxHUD::DrawTargetPanel(const UHueBuildToolComponent* Tool, EHudContext Context)
{
	if (!Tool)
	{
		return;
	}

	const FHueInteractionTarget& Target = Tool->GetTarget();
	const bool bInstruction = Context != EHudContext::Sandbox;
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

		const bool bCanEdit = Tool->IsEditingAllowed();

		if (const AHueDelayBlock* Delay = Cast<AHueDelayBlock>(Block))
		{
			HueHUD::AddLine(Lines, bCanEdit
				? FString::Printf(TEXT("Delay %.2f s    Z / X: -/+ %.2f s"), Delay->GetDelaySeconds(), AHueDelayBlock::DelayStep)
				: FString::Printf(TEXT("Delay %.2f s"), Delay->GetDelaySeconds()), HueHUD::TextColor);
		}

		if (Block->IsRemovable())
		{
			const TCHAR* EditHint = bInstruction ? TEXT("Removable    LMB / RMB collect    Q/E rotate") : TEXT("Removable    LMB collect    Q/E rotate");
			HueHUD::AddLine(Lines, bCanEdit ? EditHint : TEXT("Removable - press R to reset before editing"),
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
		const UHueInventoryComponent* Inventory = Tool->GetInventory();
		const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
		if (Target.bCanPlace)
		{
			HueHUD::AddLine(Lines, FString::Printf(TEXT("LMB  place %s facing %s"), HueModule::DisplayName(Selected),
				HueDir::ToString(Tool->GetPreviewFacing())), HueHUD::GoodColor);
		}
		else if (bInstruction && Selected == EHueModuleType::None && Inventory && !HueHUD::HasAnyModule(*Inventory))
		{
			HueHUD::AddWrapped(*this, Lines, TEXT("Tray empty - all modules placed. RMB on a placed module collects it back."),
				HueHUD::DimColor, Width, S);
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

// ---- Build view prompt + hint bar ---------------------------------------------------------------------------

void AHueboxHUD::DrawBuildPrompt(const UHueBuildToolComponent* Tool, APawn* Pawn)
{
	if (!Tool)
	{
		return;
	}

	const FHueInteractionTarget& Target = Tool->GetTarget();
	const UHueInventoryComponent* Tray = Tool->GetInventory();
	const EHueModuleType Selected = Tray ? Tray->GetSelectedType() : EHueModuleType::None;

	FString Prompt;
	FLinearColor Color = HueHUD::TextColor;

	switch (Target.Kind)
	{
	case EHueTargetKind::Block:
		if (APulseBlock* Block = Target.Block.Get())
		{
			if (Block->IsRemovable() && Tool->IsEditingAllowed())
			{
				Prompt = FString::Printf(TEXT("LMB / RMB  Collect %s   |   Q/E  Rotate"), *Block->GetDisplayName());
				if (Block->IsA<AHueDelayBlock>())
				{
					Prompt += TEXT("   |   Z/X  Delay");
				}
				Color = HueHUD::AmberColor;
			}
			else
			{
				// Fixed unit or editing locked: the block's own prompt says why.
				Prompt = IHueInteractable::Execute_GetInteractPrompt(Block, Pawn);
				Color = HueHUD::DimColor;
			}
		}
		break;

	case EHueTargetKind::Pickup:
		if (AHueModulePickup* Pickup = Target.Pickup.Get())
		{
			Prompt = IHueInteractable::Execute_GetInteractPrompt(Pickup, Pawn);
		}
		break;

	case EHueTargetKind::Cell:
		if (Target.bCanPlace)
		{
			Prompt = FString::Printf(TEXT("LMB  Place %s (facing %s)   |   Q/E  Rotate"), HueModule::DisplayName(Selected),
				HueDir::ToString(Tool->GetPreviewFacing()));
			Color = HueHUD::GoodColor;
		}
		else if (Selected == EHueModuleType::None)
		{
			// Nothing selected is a hint, not an error.
			Prompt = (Tray && !HueHUD::HasAnyModule(*Tray)) ? FString(TEXT("Tray empty - all modules placed (RMB collects one back)")) : Target.Reason;
			Color = HueHUD::DimColor;
		}
		else
		{
			Prompt = TEXT("x  ") + Target.Reason;
			Color = HueHUD::BadColor;
		}
		break;

	case EHueTargetKind::None:
	default:
		break;
	}

	if (Prompt.IsEmpty())
	{
		return;
	}

	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const FVector2f Size = HueHUD::Measure(*this, Prompt, HueHUD::BodyFont(), S);
	const float Y = HintBarTop - HueHUD::PanelGap * S - Size.Y - 2.f * PadPx;
	DrawLabel(Prompt, (Canvas->ClipX - Size.X) * 0.5f - PadPx, Y, Color, S);
}

void AHueboxHUD::DrawHintBar(EHudContext Context)
{
	TConstArrayView<HueHUD::FBinding> Hints = MakeArrayView(HueHUD::BuildHints);
	if (Context == EHudContext::Watch)
	{
		Hints = bRunFinished ? MakeArrayView(HueHUD::WatchFinishedHints) : MakeArrayView(HueHUD::WatchHints);
	}
	if (Hints.Num() == 0)
	{
		return;
	}

	UFont* Font = HueHUD::BodyFont();

	// Bar content width at a text scale (the gaps scale with it); OutHeight = tallest entry.
	auto MeasureBar = [this, Hints, Font](float Scale, float& OutHeight)
	{
		float Width = 0.f;
		OutHeight = 0.f;
		for (int32 Index = 0; Index < Hints.Num(); ++Index)
		{
			const FVector2f KeySize = HueHUD::Measure(*this, Hints[Index].Keys, Font, Scale);
			const FVector2f ActionSize = HueHUD::Measure(*this, Hints[Index].Action, Font, Scale);
			Width += KeySize.X + HueHUD::HintKeyGap * Scale + ActionSize.X;
			if (Index > 0)
			{
				Width += HueHUD::HintPairGap * Scale;
			}
			OutHeight = FMath::Max(OutHeight, static_cast<float>(FMath::Max(KeySize.Y, ActionSize.Y)));
		}
		return Width;
	};

	const float S = UIScale;
	float Scale = S;
	float Height = 0.f;
	float Width = MeasureBar(Scale, Height);

	// Narrow window: shrink the text instead of clipping the bar.
	const float Available = Canvas->ClipX - 2.f * (HueHUD::Margin + HueHUD::Pad) * S;
	if (Width > Available && Width > 0.f && Available > 0.f)
	{
		Scale *= Available / Width;
		Width = MeasureBar(Scale, Height);
	}

	const float PadPx = HueHUD::Pad * S;
	const float BoxW = Width + 2.f * PadPx;
	const float BoxH = Height + 2.f * PadPx;
	const float Left = (Canvas->ClipX - BoxW) * 0.5f;
	const float Top = Canvas->ClipY - HueHUD::Margin * S - BoxH;

	DrawRect(HueHUD::Backing, Left, Top, BoxW, BoxH);

	float CursorX = Left + PadPx;
	for (const HueHUD::FBinding& Hint : Hints)
	{
		DrawText(Hint.Keys, HueHUD::AmberColor, CursorX, Top + PadPx, Font, Scale);
		CursorX += HueHUD::Measure(*this, Hint.Keys, Font, Scale).X + HueHUD::HintKeyGap * Scale;
		DrawText(Hint.Action, HueHUD::TextColor, CursorX, Top + PadPx, Font, Scale);
		CursorX += HueHUD::Measure(*this, Hint.Action, Font, Scale).X + HueHUD::HintPairGap * Scale;
	}

	HintBarTop = Top;
	HintBarRight = Left + BoxW;
}

// ---- Minimap (Watch view) -----------------------------------------------------------------------------------

void AHueboxHUD::DrawMinimap(const AHueBoard* Board)
{
	if (!IsValid(Board))
	{
		return;
	}

	const FIntPoint Grid = Board->GetGridSize();
	if (Grid.X <= 0 || Grid.Y <= 0)
	{
		return;
	}

	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const float GapPx = HueHUD::LineGap * S;
	const float MapSize = HueHUD::MinimapSize * S;
	const float CellPx = MapSize / static_cast<float>(FMath::Max(Grid.X, Grid.Y));
	const float GridW = CellPx * Grid.X;
	const float GridH = CellPx * Grid.Y;
	const float LineW = FMath::Max(1.f, S);
	UFont* Font = HueHUD::BodyFont();
	UFont* Heading = HueHUD::HeadingFont();

	// Pulses only exist for the board that is running right now.
	const UPulseSubsystem* Sim = UPulseSubsystem::Get(this);
	const bool bLive = Sim && Sim->IsRunning() && Sim->GetActiveBoard() == Board;

	const FString Title = TEXT("Board map");
	const FString Caption = bLive ? FString::Printf(TEXT("t %.2f s"), Sim->GetSimTimeSeconds()) : FString();
	const FVector2f TitleSize = HueHUD::Measure(*this, Title, Heading, S);
	const float HeaderH = TitleSize.Y + 2.f * GapPx;

	const float BoxW = MapSize + 2.f * PadPx;
	const float BoxH = HeaderH + GridH + 2.f * PadPx;
	const float Right = Canvas->ClipX - HueHUD::Margin * S;
	const float Left = Right - BoxW;
	float Bottom = Canvas->ClipY - HueHUD::Margin * S;
	if (Left < HintBarRight + HueHUD::PanelGap * S)
	{
		// Narrow window: the centred hint bar reaches under the map, so stack the map above it.
		Bottom = HintBarTop - HueHUD::PanelGap * S;
	}
	const float Top = Bottom - BoxH;

	DrawRect(HueHUD::Backing, Left, Top, BoxW, BoxH);
	DrawText(Title, HueHUD::AmberColor, Left + PadPx, Top + PadPx, Heading, S);
	if (!Caption.IsEmpty())
	{
		const FVector2f CaptionSize = HueHUD::Measure(*this, Caption, Font, S);
		DrawText(Caption, HueHUD::DimColor, Right - PadPx - CaptionSize.X, Top + PadPx + (TitleSize.Y - CaptionSize.Y) * 0.5f, Font, S);
	}

	// Same picture as the Build view camera: cell (0,0) top-left, +X to the right, +Y (North) downwards, so the
	// stage (behind the grid's y = 0 edge) is above the map.
	const float GridLeft = Left + PadPx + (MapSize - GridW) * 0.5f;
	const float GridTop = Top + PadPx + HeaderH;
	DrawRect(HueHUD::MapFloor, GridLeft, GridTop, GridW, GridH);
	for (int32 Column = 0; Column <= Grid.X; ++Column)
	{
		DrawRect(HueHUD::MapLine, GridLeft + Column * CellPx - LineW * 0.5f, GridTop, LineW, GridH);
	}
	for (int32 Row = 0; Row <= Grid.Y; ++Row)
	{
		DrawRect(HueHUD::MapLine, GridLeft, GridTop + Row * CellPx - LineW * 0.5f, GridW, LineW);
	}

	// Blocks -------------------------------------------------------------------------------------------------
	const float TileSize = CellPx * HueHUD::MapTileFraction;
	const float HoldRing = FMath::Max(2.f, 2.f * S);
	TArray<FHuePort> Ports;
	Board->ForEachBlock([&](APulseBlock& Block)
	{
		const FIntPoint Cell = Block.GetCell();
		if (!Board->IsInside(Cell))
		{
			return;
		}

		const float CX = GridLeft + (Cell.X + 0.5f) * CellPx;
		const float CY = GridTop + (Cell.Y + 0.5f) * CellPx;
		const HueHUD::FMapTile Tile = HueHUD::DescribeMapTile(Block);

		if (Tile.bSolid)
		{
			// Barrier: the whole cell filled dark (a wall, not a tile).
			const float Size = CellPx * 0.92f;
			DrawRect(Tile.Fill, CX - Size * 0.5f, CY - Size * 0.5f, Size, Size);
			HueHUD::DrawFrame(*this, CX - Size * 0.5f, CY - Size * 0.5f, Size, Size, LineW, HueHUD::BarrierEdge);
			return;
		}

		const float TX = CX - TileSize * 0.5f;
		const float TY = CY - TileSize * 0.5f;
		DrawRect(Tile.Fill, TX, TY, TileSize, TileSize);

		Ports.Reset();
		Block.GetPorts(Ports);
		for (const FHuePort& Port : Ports)
		{
			HueHUD::DrawPortMark(*this, CX, CY, TileSize, Port, Tile.Ink, Tile.Fill);
		}

		HueHUD::DrawCentredText(*this, Tile.Letter, CX, CY, TileSize * 0.75f, Tile.Ink);

		if (Tile.bHolding)
		{
			HueHUD::DrawFrame(*this, TX - HoldRing, TY - HoldRing, TileSize + 2.f * HoldRing, TileSize + 2.f * HoldRing, HoldRing, HueHUD::InfoColor);
		}
	});

	// Live pulses --------------------------------------------------------------------------------------------
	if (!bLive)
	{
		return;
	}

	TArray<FHuePulseSnapshot> Snapshots;
	Sim->GetPulseSnapshots(Snapshots);

	const float Radius = FMath::Max(4.f * S, CellPx * 0.2f);
	const float Outline = FMath::Max(1.f, 1.5f * S);
	for (const FHuePulseSnapshot& Pulse : Snapshots)
	{
		// A pulse leaving the board heads for a cell outside the grid; it disappears at the edge.
		const FVector2D& P = Pulse.CellPosition;
		if (P.X < -0.5 || P.X > Grid.X - 0.5 || P.Y < -0.5 || P.Y > Grid.Y - 0.5)
		{
			continue;
		}

		const FVector2D Center(GridLeft + (P.X + 0.5) * CellPx, GridTop + (P.Y + 0.5) * CellPx);
		// Dark rim first so white (N) pulses stay visible on white module tiles.
		Canvas->K2_DrawPolygon(nullptr, Center, FVector2D(Radius + Outline), HueHUD::CircleSides, HueHUD::TileInk);
		Canvas->K2_DrawPolygon(nullptr, Center, FVector2D(Radius), HueHUD::CircleSides, HueColor::ToLinear(Pulse.Color));
		HueHUD::DrawCentredText(*this, HueColor::Label(Pulse.Color), static_cast<float>(Center.X), static_cast<float>(Center.Y),
			Radius * 1.3f, HueHUD::TileInk);
	}
}

void AHueboxHUD::DrawRunBanner(const AExhibitManager* Manager)
{
	if (!Manager)
	{
		return;
	}

	const EHueRunState State = Manager->GetState();
	if (State != EHueRunState::Complete && State != EHueRunState::Ended)
	{
		return;
	}

	TArray<HueHUD::FLine> Lines;
	if (State == EHueRunState::Complete)
	{
		HueHUD::AddLine(Lines, TEXT("Exercise complete!"), HueHUD::GoodColor, true);
		HueHUD::AddLine(Lines, TEXT("Tab  next exercise    Space  replay    R  back to build"), HueHUD::TextColor);
	}
	else
	{
		HueHUD::AddLine(Lines, TEXT("Run ended"), HueHUD::BadColor, true);
		if (!Manager->GetEndReason().IsEmpty())
		{
			HueHUD::AddLine(Lines, Manager->GetEndReason(), HueHUD::BadColor);
		}
		HueHUD::AddLine(Lines, TEXT("R  back to build and adjust    Space  run again    Tab  next exercise"), HueHUD::TextColor);
	}

	const float S = UIScale;
	const float BoxW = HueHUD::MeasureLinesWidth(*this, Lines, S) + 2.f * HueHUD::Pad * S;
	HueHUD::DrawPanel(*this, Lines, (Canvas->ClipX - BoxW) * 0.5f, HueHUD::Margin * S, S, 0.f, false);
}

// ---- Overlays -----------------------------------------------------------------------------------------------

void AHueboxHUD::DrawLightKey()
{
	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const float GapPx = HueHUD::LineGap * S;
	const float ColumnGap = 12.f * S;
	// Circle radius of the diagram; the three centres sit one radius apart (a classic three-set Venn).
	const float Radius = 38.f * S;

	const FString Heading = TEXT("Light = red + green + blue parts");
	TArray<HueHUD::FLine> Lines;
	HueHUD::AddLine(Lines, TEXT("Mix: the parts add up   R + C = W   R + G = Y"), HueHUD::TextColor);
	HueHUD::AddLine(Lines, TEXT("   a part on both sides fails (R + R, R + Y)"), HueHUD::DimColor);
	HueHUD::AddLine(Lines, TEXT("Shift: every part flips   C[-GB] -> R[R--]"), HueHUD::TextColor);
	HueHUD::AddLine(Lines, TEXT("Her partner is red:"), HueHUD::TextColor);
	HueHUD::AddLine(Lines, TEXT("   R  pushed away (same colour)"), HueColor::ToLinear(EHueColor::A));
	HueHUD::AddLine(Lines, TEXT("   C  pulled back (not red)"), HueColor::ToLinear(EHueColor::B));
	HueHUD::AddLine(Lines, TEXT("   W  brakes (red + cyan cancel)"), HueColor::ToLinear(EHueColor::N));
	HueHUD::AddLine(Lines, TEXT("   G, Y ...  keys: she does a move"), HueHUD::DimColor);

	UFont* Font = HueHUD::BodyFont();
	const FVector2f HeadingSize = HueHUD::Measure(*this, Heading, HueHUD::HeadingFont(), S);
	const float TextWidth = HueHUD::MeasureLinesWidth(*this, Lines, S);
	float TextHeight = GapPx * (Lines.Num() - 1);
	TArray<float, TInlineAllocator<8>> LineHeights;
	for (const HueHUD::FLine& Line : Lines)
	{
		LineHeights.Add(static_cast<float>(HueHUD::Measure(*this, Line.Text, Font, S).Y));
		TextHeight += LineHeights.Last();
	}

	const float DiagramWidth = 3.f * Radius;
	const float DiagramHeight = 2.9f * Radius;
	const float BodyHeight = FMath::Max(DiagramHeight, TextHeight);
	const float BoxWidth = FMath::Max(static_cast<float>(HeadingSize.X), DiagramWidth + ColumnGap + TextWidth) + 2.f * PadPx;
	const float BoxHeight = static_cast<float>(HeadingSize.Y) + GapPx * 3.f + BodyHeight + 2.f * PadPx;

	// Bottom-left; above the centred hint bar if the two would meet (as the help panel does).
	const float Left = HueHUD::Margin * S;
	float Top = Canvas->ClipY - HueHUD::Margin * S - BoxHeight;
	const float HintBarLeft = Canvas->ClipX - HintBarRight;
	if (HintBarRight > 0.f && Left + BoxWidth + HueHUD::PanelGap * S > HintBarLeft)
	{
		Top = HintBarTop - HueHUD::PanelGap * S - BoxHeight;
	}

	DrawRect(HueHUD::Backing, Left, Top, BoxWidth, BoxHeight);
	DrawText(Heading, HueHUD::AmberColor, Left + PadPx, Top + PadPx, HueHUD::HeadingFont(), S);
	const float BodyTop = Top + PadPx + static_cast<float>(HeadingSize.Y) + GapPx * 3.f;

	// ---- The diagram: three primaries drawn additively, so where they overlap the screen really shows Y, C, M, W ----
	const FVector2D Center(Left + PadPx + DiagramWidth * 0.5f, BodyTop + BodyHeight * 0.5f);
	// Unit directions (screen space, +Y down) from the diagram centre to the red, green and blue circle centres.
	const FVector2D DirRed(-0.866, -0.5);
	const FVector2D DirGreen(0.866, -0.5);
	const FVector2D DirBlue(0.0, 1.0);
	const double CentreOffset = Radius * 0.577; // centres one radius apart
	struct FPrimaryDisc
	{
		FVector2D Dir;
		FLinearColor Color;
	};
	const FPrimaryDisc Discs[] =
	{
		{ DirRed, FLinearColor(1.f, 0.f, 0.f) },
		{ DirGreen, FLinearColor(0.f, 1.f, 0.f) },
		{ DirBlue, FLinearColor(0.f, 0.f, 1.f) },
	};
	constexpr int32 DiscSides = 36;
	for (const FPrimaryDisc& Disc : Discs)
	{
		const FVector2D DiscCenter = Center + Disc.Dir * CentreOffset;
		TArray<FCanvasUVTri> Tris;
		Tris.Reserve(DiscSides);
		for (int32 Side = 0; Side < DiscSides; ++Side)
		{
			const double A0 = UE_DOUBLE_TWO_PI * Side / DiscSides;
			const double A1 = UE_DOUBLE_TWO_PI * (Side + 1) / DiscSides;
			FCanvasUVTri& Tri = Tris.AddDefaulted_GetRef();
			Tri.V0_Pos = DiscCenter;
			Tri.V1_Pos = DiscCenter + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * Radius;
			Tri.V2_Pos = DiscCenter + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * Radius;
			Tri.V0_Color = Tri.V1_Color = Tri.V2_Color = Disc.Color;
		}
		FCanvasTriangleItem Item(Tris, GWhiteTexture);
		Item.BlendMode = SE_BLEND_Additive;
		Canvas->DrawItem(Item);
	}

	// Region letters: a primary alone outside, two primaries opposite the third circle, all three in the middle.
	struct FRegionLetter
	{
		EHueColor Color;
		FVector2D Offset;
	};
	const double Single = Radius * 1.05;
	const double Pair = Radius * 0.79;
	const FRegionLetter Letters[] =
	{
		{ EHueColor::A, DirRed * Single },
		{ EHueColor::G, DirGreen * Single },
		{ EHueColor::V, DirBlue * Single },
		{ EHueColor::Y, -DirBlue * Pair },
		{ EHueColor::M, -DirGreen * Pair },
		{ EHueColor::B, -DirRed * Pair },
		{ EHueColor::N, FVector2D::ZeroVector },
	};
	for (const FRegionLetter& Letter : Letters)
	{
		// Dark ink on the bright regions, white on blue.
		const FLinearColor Ink = Letter.Color == EHueColor::V ? HueHUD::TextColor : HueHUD::TileInk;
		const FVector2D At = Center + Letter.Offset;
		HueHUD::DrawCentredText(*this, HueColor::Label(Letter.Color), static_cast<float>(At.X), static_cast<float>(At.Y), Radius * 0.45f, Ink);
	}

	// ---- The rules next to it ----------------------------------------------------------------------------------
	const float TextLeft = Left + PadPx + DiagramWidth + ColumnGap;
	float RowY = BodyTop + (BodyHeight - TextHeight) * 0.5f;
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		DrawText(Lines[Index].Text, Lines[Index].Color, TextLeft, RowY, Font, S);
		RowY += LineHeights[Index] + GapPx;
	}
}

void AHueboxHUD::DrawHelp(EHudContext Context)
{
	TConstArrayView<HueHUD::FBinding> Bindings = MakeArrayView(HueHUD::SandboxBindings);
	FString Heading = TEXT("Controls  (H to hide)");
	FString Footer;
	switch (Context)
	{
	case EHudContext::Build:
		Bindings = MakeArrayView(HueHUD::BuildBindings);
		Heading = TEXT("Build view controls  (H to hide)");
		Footer = HueHUD::BuildHelpFooter;
		break;
	case EHudContext::Watch:
		Bindings = bRunFinished ? MakeArrayView(HueHUD::WatchFinishedBindings) : MakeArrayView(HueHUD::WatchBindings);
		Heading = TEXT("Watch view controls  (H to hide)");
		Footer = HueHUD::WatchHelpFooter;
		break;
	case EHudContext::Sandbox:
	case EHudContext::None:
	default:
		break;
	}

	const float S = UIScale;
	const float PadPx = HueHUD::Pad * S;
	const float GapPx = HueHUD::LineGap * S;
	const float ColumnGap = 16.f * S;
	UFont* Font = HueHUD::BodyFont();

	const FVector2f HeadingSize = HueHUD::Measure(*this, Heading, HueHUD::HeadingFont(), S);

	float KeysWidth = 0.f;
	float ActionWidth = 0.f;
	float RowHeight = 0.f;
	for (const HueHUD::FBinding& Binding : Bindings)
	{
		const FVector2f KeySize = HueHUD::Measure(*this, Binding.Keys, Font, S);
		const FVector2f ActionSize = HueHUD::Measure(*this, Binding.Action, Font, S);
		KeysWidth = FMath::Max(KeysWidth, static_cast<float>(KeySize.X));
		ActionWidth = FMath::Max(ActionWidth, static_cast<float>(ActionSize.X));
		RowHeight = FMath::Max(RowHeight, static_cast<float>(FMath::Max(KeySize.Y, ActionSize.Y)));
	}

	const FVector2f FooterSize = Footer.IsEmpty() ? FVector2f::ZeroVector : HueHUD::Measure(*this, Footer, Font, S);
	const float FooterBlock = Footer.IsEmpty() ? 0.f : static_cast<float>(FooterSize.Y) + GapPx * 2.f;

	const int32 NumRows = Bindings.Num();
	const float BoxWidth = FMath::Max3(static_cast<float>(HeadingSize.X), KeysWidth + ColumnGap + ActionWidth, static_cast<float>(FooterSize.X)) + 2.f * PadPx;
	const float BoxHeight = static_cast<float>(HeadingSize.Y) + GapPx * 2.f + NumRows * (RowHeight + GapPx) + FooterBlock + 2.f * PadPx;
	float Left = Canvas->ClipX - HueHUD::Margin * S - BoxWidth;

	float Top = Canvas->ClipY - HueHUD::Margin * S - BoxHeight;
	if (Context == EHudContext::Build)
	{
		// Above the key hint bar (the target panel owns the top-right corner).
		Top = HintBarTop - HueHUD::PanelGap * S - BoxHeight;
	}
	else if (Context == EHudContext::Watch)
	{
		// Bottom-left, over the near floor: the minimap owns the bottom-right corner, and the top band of the stage
		// shot holds the zone label above the backdrop ("STOP HERE"). Above the centred hint bar if they would meet.
		Left = HueHUD::Margin * S;
		const float HintBarLeft = Canvas->ClipX - HintBarRight;
		if (HintBarRight > 0.f && Left + BoxWidth + HueHUD::PanelGap * S > HintBarLeft)
		{
			Top = HintBarTop - HueHUD::PanelGap * S - BoxHeight;
		}
	}

	DrawRect(HueHUD::Backing, Left, Top, BoxWidth, BoxHeight);
	DrawText(Heading, HueHUD::AmberColor, Left + PadPx, Top + PadPx, HueHUD::HeadingFont(), S);

	float RowY = Top + PadPx + static_cast<float>(HeadingSize.Y) + GapPx * 2.f;
	for (const HueHUD::FBinding& Binding : Bindings)
	{
		DrawText(Binding.Keys, HueHUD::AmberColor, Left + PadPx, RowY, Font, S);
		DrawText(Binding.Action, HueHUD::TextColor, Left + PadPx + KeysWidth + ColumnGap, RowY, Font, S);
		RowY += RowHeight + GapPx;
	}

	if (!Footer.IsEmpty())
	{
		DrawText(Footer, HueHUD::DimColor, Left + PadPx, RowY + GapPx, Font, S);
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
