#include "Game/ExhibitManager.h"

#include "Algo/StableSort.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

#include "Blocks/HueFixedBlocks.h"
#include "Game/HueboxGameMode.h"
#include "Game/HueTutorialBuilder.h"
#include "Grid/HueBoard.h"
#include "Rig/ReactiveRig.h"
#include "Simulation/PulseSubsystem.h"

namespace HueExhibitPrivate
{
	/** Same per-frame clamp as the pulse simulation and Chaos (MaxPhysicsDeltaTime), so objectives, logical time
	 *  and carriage motion advance in step even on a hitch frame. */
	constexpr float MaxStepSeconds = 1.f / 30.f;

	/** An identical feedback line raised again within this window refreshes the old line instead of stacking. */
	constexpr double MergeWindowSeconds = 1.0;

	/** Self-test: delay after BeginPlay (boards are built by then), real-time safety per board. */
	constexpr float SelfTestStartDelaySeconds = 1.0f;
	constexpr float SelfTestSafetySeconds = 25.f;

	/** "20" for whole seconds, "12.5" otherwise. */
	FString FormatSeconds(float Seconds)
	{
		return FMath::IsNearlyEqual(Seconds, FMath::RoundToFloat(Seconds))
			? FString::Printf(TEXT("%.0f"), Seconds)
			: FString::Printf(TEXT("%.1f"), Seconds);
	}

	const TCHAR* StateName(EHueRunState State)
	{
		switch (State)
		{
		case EHueRunState::Build:    return TEXT("Build");
		case EHueRunState::Running:  return TEXT("Running");
		case EHueRunState::Paused:   return TEXT("Paused");
		case EHueRunState::Ended:    return TEXT("Ended");
		case EHueRunState::Complete: return TEXT("Complete");
		default:                     return TEXT("?");
		}
	}
}

AExhibitManager::AExhibitManager()
{
	// Ticks every frame even in Build (cheap): run supervision + self-test.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// Objectives sample the carriage after physics moved it this frame.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetCanBeDamaged(false);
}

AExhibitManager* AExhibitManager::Get(const UObject* WorldContext)
{
	UWorld* World = (WorldContext && GEngine)
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}

	if (const AHueboxGameMode* GameMode = Cast<AHueboxGameMode>(World->GetAuthGameMode()))
	{
		AExhibitManager* Cached = GameMode->GetExhibitManager();
		if (IsValid(Cached))
		{
			return Cached;
		}
	}

	// No Huebox game mode (tests, other modes): fall back to an actor search.
	for (TActorIterator<AExhibitManager> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

// ---- Lifetime -----------------------------------------------------------------------------------------------

void AExhibitManager::BeginPlay()
{
	Super::BeginPlay();

	ResolveSim();

	// Boards that already began play; boards spawned later register themselves from their own BeginPlay.
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AHueBoard> It(World); It; ++It)
		{
			RegisterBoard(*It);
		}
	}
	UpdateHighlights();

	bSelfTest = FParse::Param(FCommandLine::Get(), TEXT("HueSelfTest"));
	if (bSelfTest)
	{
		UE_LOG(LogHuebox, Display, TEXT("HueSelfTest: switch found, validation starts in %.1f s"),
			HueExhibitPrivate::SelfTestStartDelaySeconds);
		GetWorldTimerManager().SetTimer(SelfTestStartTimer, this, &AExhibitManager::BeginSelfTest,
			HueExhibitPrivate::SelfTestStartDelaySeconds, false);
	}
}

void AExhibitManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Removed mid-run while the world keeps playing: nothing could reset the attempt afterwards, so undo all of it
	// now (pulses, block buffers, receiver colour, objectives, carriage) while Sim / ActiveBoard / Boards are still set.
	if (EndPlayReason == EEndPlayReason::Destroyed && State != EHueRunState::Build)
	{
		ResetAttemptInternal(/*bQuiet*/ true);
	}

	if (UPulseSubsystem* PulseSim = Sim.Get())
	{
		PulseSim->OnFeedback.RemoveDynamic(this, &AExhibitManager::HandleSimFeedback);
	}

	GetWorldTimerManager().ClearAllTimersForObject(this);
	OnStateChanged.Clear();

	Sim.Reset();
	SelfTestBoard.Reset();
	bSelfTestActive = false;
	SelfTestBoardIndex = INDEX_NONE;
	ActiveBoard = nullptr;
	Boards.Empty();
	RecentFeedback.Empty();

	Super::EndPlay(EndPlayReason);
}

UPulseSubsystem* AExhibitManager::ResolveSim()
{
	if (UPulseSubsystem* Cached = Sim.Get())
	{
		return Cached;
	}

	UPulseSubsystem* Found = UPulseSubsystem::Get(this);
	if (Found)
	{
		Sim = Found;
		// Unique: ResolveSim may run again after the weak pointer was reset; never bind twice.
		Found->OnFeedback.AddUniqueDynamic(this, &AExhibitManager::HandleSimFeedback);
	}
	return Found;
}

// ---- Tick -----------------------------------------------------------------------------------------------------

void AExhibitManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Any attempt state (also Paused / Ended / Complete, where the simulation does not step and would never notice):
	// a removed board must not leave the run state, frozen pulse visuals or the editing lock behind.
	if (State != EHueRunState::Build && !IsValid(ActiveBoard))
	{
		UE_LOG(LogHuebox, Warning, TEXT("ExhibitManager: the running board was removed - resetting the attempt"));
		ResetAttemptInternal(true); // StopAndClear also recycles the pulse visuals
	}
	else if (State == EHueRunState::Running)
	{
		AHueBoard* Board = ActiveBoard;
		const float Step = FMath::Min(DeltaSeconds, HueExhibitPrivate::MaxStepSeconds);
		RunClockSeconds += Step;

		Board->TickObjectives(Step);
		if (Board->AreObjectivesComplete())
		{
			HandleRunComplete();
		}
		else if (GetRunTimeSeconds() >= Board->RunTimeLimitSeconds)
		{
			HandleRunTimeout();
		}
	}
	else if ((State == EHueRunState::Complete || State == EHueRunState::Ended) && !bSelfTestActive)
	{
		// A finished attempt must not lock the rest of the level: once the player walks up to another board, the
		// old attempt is released (quiet reset), so editing, Space and the HUD follow the player to the new board.
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		AHueBoard* Near = Pawn ? FindBoardNear(Pawn->GetActorLocation()) : nullptr;
		if (IsValid(Near) && Near != ActiveBoard)
		{
			const FString Finished = ActiveBoard->GetBoardName();
			ResetAttemptInternal(/*bQuiet*/ true);
			PushFeedback(EHueFeedbackKind::Info,
				FString::Printf(TEXT("%s released - now at %s (Space to run)"), *Finished, *Near->GetBoardName()), Near);
		}
	}

	TickSelfTest(DeltaSeconds);
}

double AExhibitManager::GetRunTimeSeconds() const
{
	switch (State)
	{
	case EHueRunState::Running:
	case EHueRunState::Paused:
	{
		// Logical simulation time is authoritative while the simulation runs this board.
		const UPulseSubsystem* PulseSim = Sim.Get();
		if (PulseSim && PulseSim->IsRunning() && PulseSim->GetActiveBoard() == ActiveBoard.Get())
		{
			return PulseSim->GetSimTimeSeconds();
		}
		return RunClockSeconds;
	}
	case EHueRunState::Ended:
	case EHueRunState::Complete:
		return FrozenRunTimeSeconds;
	case EHueRunState::Build:
	default:
		return 0.0;
	}
}

// ---- Boards ---------------------------------------------------------------------------------------------------

void AExhibitManager::RegisterBoard(AHueBoard* Board)
{
	if (!IsValid(Board))
	{
		return;
	}

	PruneBoards();
	Boards.AddUnique(Board);

	// Progression order = west to east (Algo::StableSort: TArray::Sort would dereference the TObjectPtrs).
	Algo::StableSort(Boards, [](const TObjectPtr<AHueBoard>& A, const TObjectPtr<AHueBoard>& B)
	{
		const FVector LocA = A->GetActorLocation();
		const FVector LocB = B->GetActorLocation();
		return LocA.X != LocB.X ? LocA.X < LocB.X : LocA.Y < LocB.Y;
	});

	UpdateHighlights();
}

void AExhibitManager::PruneBoards()
{
	Boards.RemoveAll([](const TObjectPtr<AHueBoard>& Board) { return !IsValid(Board); });
}

AHueBoard* AExhibitManager::FindBoardNear(const FVector& Location, float Margin) const
{
	AHueBoard* Best = nullptr;
	double BestDistSq = TNumericLimits<double>::Max();

	for (const TObjectPtr<AHueBoard>& Board : Boards)
	{
		if (!IsValid(Board))
		{
			continue;
		}

		const FBox Bounds = Board->GetWorldBounds(Margin);
		if (!Bounds.IsValid)
		{
			continue;
		}

		// XY containment only: the player's eye height must not matter.
		if (Location.X < Bounds.Min.X || Location.X > Bounds.Max.X || Location.Y < Bounds.Min.Y || Location.Y > Bounds.Max.Y)
		{
			continue;
		}

		const FVector Center = Bounds.GetCenter();
		const double DistSq = FMath::Square(Location.X - Center.X) + FMath::Square(Location.Y - Center.Y);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Board;
		}
	}
	return Best;
}

void AExhibitManager::UpdateHighlights()
{
	// The next exercise to do (first incomplete board in progression order) is highlighted.
	bool bHighlightGiven = false;
	for (const TObjectPtr<AHueBoard>& Board : Boards)
	{
		if (!IsValid(Board))
		{
			continue;
		}
		const bool bHighlight = !bHighlightGiven && !Board->IsCompleted();
		bHighlightGiven |= bHighlight;
		Board->SetHighlighted(bHighlight);
	}
}

// ---- Run control ----------------------------------------------------------------------------------------------

void AExhibitManager::ToggleRun(const FVector& PlayerLocation)
{
	switch (State)
	{
	case EHueRunState::Build:
		if (AHueBoard* Near = FindBoardNear(PlayerLocation))
		{
			StartRun(Near);
		}
		else
		{
			PushFeedback(EHueFeedbackKind::Warning, TEXT("Walk up to an exercise board to run it"));
		}
		break;

	case EHueRunState::Running:
		SetPaused(true);
		break;

	case EHueRunState::Paused:
		SetPaused(false);
		break;

	case EHueRunState::Ended:
	case EHueRunState::Complete:
	{
		// Rerun the board the HUD shows ("Space: replay / run again"); R returns to Build, where the panel and
		// Space follow the player to another board. The board near the player is only a fallback.
		AHueBoard* Target = ActiveBoard;
		if (!IsValid(Target))
		{
			Target = FindBoardNear(PlayerLocation);
		}
		if (IsValid(Target))
		{
			StartRun(Target);
		}
		else
		{
			ResetAttempt();
		}
		break;
	}

	default:
		break;
	}
}

bool AExhibitManager::StartRun(AHueBoard* Board)
{
	if (!IsValid(Board))
	{
		return false;
	}

	UPulseSubsystem* PulseSim = ResolveSim();
	if (!PulseSim)
	{
		PushFeedback(EHueFeedbackKind::Error, TEXT("Pulse simulation unavailable - the run cannot start"), Board);
		return false;
	}

	if (State != EHueRunState::Build)
	{
		ResetAttemptInternal(/*bQuiet*/ true);
	}

	// Boards placed by hand / spawned outside a builder are adopted here.
	RegisterBoard(Board);

	ActiveBoard = Board;
	RunClockSeconds = 0.0;
	FrozenRunTimeSeconds = 0.0;

	Board->ResetAllBlocks();
	Board->ResetObjectives();

	AReactiveRig* Rig = Board->GetRig();
	if (IsValid(Rig))
	{
		Rig->ResetRig();
	}
	else
	{
		PushFeedback(EHueFeedbackKind::Warning,
			FString::Printf(TEXT("%s has no mechanism linked - objectives cannot progress"), *Board->GetBoardName()), Board);
	}

	if (!PulseSim->StartRun(Board))
	{
		PulseSim->StopAndClear();
		ActiveBoard = nullptr;
		PushFeedback(EHueFeedbackKind::Error, FString::Printf(TEXT("%s: the run could not start"), *Board->GetBoardName()), Board);
		return false;
	}

	if (IsValid(Rig))
	{
		Rig->SetMotionActive(true);
	}

	SetState(EHueRunState::Running);
	PushFeedback(EHueFeedbackKind::Info, FString::Printf(TEXT("Run started: %s"), *Board->GetBoardName()), Board);
	return true;
}

void AExhibitManager::SetPaused(bool bInPaused)
{
	AHueBoard* Board = ActiveBoard;
	AReactiveRig* Rig = IsValid(Board) ? Board->GetRig() : nullptr;
	UPulseSubsystem* PulseSim = ResolveSim();

	if (bInPaused && State == EHueRunState::Running)
	{
		if (PulseSim)
		{
			PulseSim->SetPaused(true);
		}
		if (IsValid(Rig))
		{
			Rig->SetMotionPaused(true);
		}
		SetState(EHueRunState::Paused);
		PushFeedback(EHueFeedbackKind::Info, TEXT("Run paused - Space to resume, R to reset"), Board);
	}
	else if (!bInPaused && State == EHueRunState::Paused)
	{
		if (PulseSim)
		{
			PulseSim->SetPaused(false);
		}
		if (IsValid(Rig))
		{
			Rig->SetMotionPaused(false);
		}
		SetState(EHueRunState::Running);
		PushFeedback(EHueFeedbackKind::Info, TEXT("Run resumed"), Board);
	}
}

void AExhibitManager::ResetAttempt()
{
	ResetAttemptInternal(/*bQuiet*/ false);
}

void AExhibitManager::ResetAttemptInternal(bool bQuiet)
{
	// 1. Pulses, held signals and every queued event (delay releases, mixer timeouts) go first, so nothing can
	//    call back into a block while it is being reset.
	if (UPulseSubsystem* PulseSim = ResolveSim())
	{
		PulseSim->StopAndClear();
	}

	AHueBoard* Board = ActiveBoard;
	const bool bHadAttempt = IsValid(Board) || State != EHueRunState::Build;

	if (IsValid(Board))
	{
		// 2. Block buffers / colours, goal flags, carriage. The layout (cells, facings, delays) is kept.
		Board->ResetAllBlocks();
		Board->ResetObjectives();
		if (AReactiveRig* Rig = Board->GetRig(); IsValid(Rig))
		{
			Rig->ResetRig();
		}
	}
	else
	{
		// Nothing to undo: still bring every carriage home, quietly.
		for (const TObjectPtr<AHueBoard>& Each : Boards)
		{
			if (!IsValid(Each))
			{
				continue;
			}
			if (AReactiveRig* Rig = Each->GetRig(); IsValid(Rig))
			{
				Rig->ResetRig();
			}
		}
	}

	ActiveBoard = nullptr;
	RunClockSeconds = 0.0;
	FrozenRunTimeSeconds = 0.0;
	SetState(EHueRunState::Build);

	if (!bQuiet && bHadAttempt)
	{
		PushFeedback(EHueFeedbackKind::Info, TEXT("Reset - layout kept"), Board);
	}
}

void AExhibitManager::SetState(EHueRunState NewState)
{
	if (State == NewState)
	{
		return;
	}

	UE_LOG(LogHuebox, Log, TEXT("ExhibitManager: %s -> %s"),
		HueExhibitPrivate::StateName(State), HueExhibitPrivate::StateName(NewState));
	State = NewState;
	OnStateChanged.Broadcast(NewState);
}

void AExhibitManager::HandleRunComplete()
{
	AHueBoard* Board = ActiveBoard;

	// Capture before the simulation is cleared (it may reset its clock).
	FrozenRunTimeSeconds = GetRunTimeSeconds();
	SetState(EHueRunState::Complete);

	if (UPulseSubsystem* PulseSim = Sim.Get())
	{
		PulseSim->StopAndClear();
	}

	if (IsValid(Board))
	{
		// The carriage is NOT frozen: the apparatus finishes its own performance (GDD 4 "the completed apparatus
		// performs the ending itself") - it coasts on into the end stop or has already braked. Reset restores it.
		Board->SetCompleted(true);
		PushFeedback(EHueFeedbackKind::Success, FString::Printf(TEXT("%s complete!"), *Board->GetBoardName()), Board);
	}

	UpdateHighlights();
}

void AExhibitManager::HandleRunTimeout()
{
	AHueBoard* Board = ActiveBoard;
	AReactiveRig* Rig = IsValid(Board) ? Board->GetRig() : nullptr;
	const float LimitSeconds = IsValid(Board) ? Board->RunTimeLimitSeconds : 20.f;
	const float Fraction = IsValid(Rig) ? Rig->GetCarriageFraction() : 0.f;

	FrozenRunTimeSeconds = GetRunTimeSeconds();
	SetState(EHueRunState::Ended);

	if (UPulseSubsystem* PulseSim = Sim.Get())
	{
		PulseSim->StopAndClear();
	}
	if (IsValid(Rig))
	{
		// Kept visible (not reset) so the player can see how far this attempt got.
		Rig->FreezeMotion();
	}

	PushFeedback(EHueFeedbackKind::Error,
		FString::Printf(TEXT("Time limit (%s s) reached - carriage stopped at %d%% of the track. Press R to reset."),
			*HueExhibitPrivate::FormatSeconds(LimitSeconds), FMath::RoundToInt(Fraction * 100.f)),
		Board);
}

// ---- Feedback -------------------------------------------------------------------------------------------------

void AExhibitManager::PushFeedback(EHueFeedbackKind Kind, const FString& Message, AHueBoard* Board)
{
	FHueFeedback Line;
	Line.Kind = Kind;
	Line.Message = Message;
	Line.Board = Board;
	const UWorld* World = GetWorld();
	Line.RealTimeSeconds = World ? World->GetRealTimeSeconds() : 0.0;

	// Player-facing "errors" are gameplay outcomes, not engine errors: log them as warnings.
	if (Kind == EHueFeedbackKind::Warning || Kind == EHueFeedbackKind::Error)
	{
		UE_LOG(LogHuebox, Warning, TEXT("[Exhibit] %s"), *Message);
	}
	else
	{
		UE_LOG(LogHuebox, Log, TEXT("[Exhibit] %s"), *Message);
	}

	AppendFeedback(Line);
}

void AExhibitManager::HandleSimFeedback(const FHueFeedback& Feedback)
{
	// Already logged by the subsystem; only mirrored into the HUD ring here.
	FHueFeedback Line = Feedback;
	if (Line.RealTimeSeconds <= 0.0)
	{
		if (const UWorld* World = GetWorld())
		{
			Line.RealTimeSeconds = World->GetRealTimeSeconds();
		}
	}
	AppendFeedback(Line);
}

void AExhibitManager::AppendFeedback(const FHueFeedback& Line)
{
	if (RecentFeedback.Num() > 0)
	{
		FHueFeedback& Last = RecentFeedback.Last();
		if (Last.Kind == Line.Kind
			&& Last.Message.Equals(Line.Message, ESearchCase::CaseSensitive)
			&& Line.RealTimeSeconds - Last.RealTimeSeconds <= HueExhibitPrivate::MergeWindowSeconds)
		{
			Last.RealTimeSeconds = Line.RealTimeSeconds;
			return;
		}
	}

	RecentFeedback.Add(Line);

	const int32 MaxLines = FMath::Max(1, MaxFeedbackLines);
	if (RecentFeedback.Num() > MaxLines)
	{
		RecentFeedback.RemoveAt(0, RecentFeedback.Num() - MaxLines, EAllowShrinking::No);
	}
}

// ---- Self-test (graybox validation) ---------------------------------------------------------------------------

void AExhibitManager::BeginSelfTest()
{
	if (bSelfTestActive)
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueSelfTest: already running"));
		return;
	}

	GetWorldTimerManager().ClearTimer(SelfTestStartTimer);
	PruneBoards();

	bSelfTestActive = true;
	SelfTestBoardIndex = INDEX_NONE;
	SelfTestBoard.Reset();
	SelfTestWait = 0.f;
	SelfTestPassed = 0;
	SelfTestFailed = 0;

	UE_LOG(LogHuebox, Display, TEXT("HueSelfTest: validating %d board(s) with their baseline solutions"), Boards.Num());
	if (Boards.Num() == 0)
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueSelfTest: no boards registered - nothing to validate"));
	}

	StartNextSelfTestBoard();
}

void AExhibitManager::StartNextSelfTestBoard()
{
	for (++SelfTestBoardIndex; Boards.IsValidIndex(SelfTestBoardIndex); ++SelfTestBoardIndex)
	{
		AHueBoard* Board = Boards[SelfTestBoardIndex];
		SelfTestBoard = Board;
		SelfTestWait = 0.f;

		if (!IsValid(Board))
		{
			FinishSelfTestBoard(false, TEXT("board is no longer valid"));
			continue;
		}

		ResetAttempt();

		const AHueTutorialBuilder* Builder = AHueTutorialBuilder::FindInWorld(this);
		if (const FHueBoardLayout* Layout = Builder ? Builder->FindLayoutForBoard(Board) : nullptr)
		{
			AHueTutorialBuilder::InstallSolution(Board, *Layout);
		}
		else
		{
			UE_LOG(LogHuebox, Warning, TEXT("HueSelfTest: no layout known for %s - running its current layout"),
				*Board->GetBoardName());
		}

		if (StartRun(Board))
		{
			return; // TickSelfTest waits for the result.
		}
		FinishSelfTestBoard(false, TEXT("the run could not start"));
	}

	FinishSelfTest();
}

void AExhibitManager::TickSelfTest(float /*DeltaSeconds*/)
{
	if (!bSelfTestActive)
	{
		return;
	}

	// Real (undilated, unclamped) frame time for the safety limit.
	SelfTestWait += static_cast<float>(FApp::GetDeltaTime());

	if (!SelfTestBoard.IsValid())
	{
		FinishSelfTestBoard(false, TEXT("board was destroyed during the run"));
		StartNextSelfTestBoard();
		return;
	}

	switch (State)
	{
	case EHueRunState::Complete:
		FinishSelfTestBoard(true, FString());
		StartNextSelfTestBoard();
		break;

	case EHueRunState::Ended:
		FinishSelfTestBoard(false, TEXT("time limit reached before all objectives were met"));
		StartNextSelfTestBoard();
		break;

	case EHueRunState::Build:
		FinishSelfTestBoard(false, TEXT("the run was reset externally"));
		StartNextSelfTestBoard();
		break;

	case EHueRunState::Running:
	case EHueRunState::Paused:
	default:
	{
		const AHueBoard* Board = SelfTestBoard.Get();
		const float Safety = FMath::Max(HueExhibitPrivate::SelfTestSafetySeconds, Board->RunTimeLimitSeconds + 5.f);
		if (SelfTestWait > Safety)
		{
			FinishSelfTestBoard(false, FString::Printf(TEXT("no result after %.0f s real time"), Safety));
			StartNextSelfTestBoard();
		}
		break;
	}
	}
}

void AExhibitManager::FinishSelfTestBoard(bool bPassed, const FString& Reason)
{
	AHueBoard* Board = SelfTestBoard.Get();
	const FString Name = IsValid(Board) ? Board->GetBoardName() : FString(TEXT("<missing board>"));
	const AReactiveRig* Rig = IsValid(Board) ? Board->GetRig() : nullptr;
	const float Fraction = IsValid(Rig) ? Rig->GetCarriageFraction() : 0.f;
	const float MaxFraction = IsValid(Rig) ? Rig->GetAttemptMaxFraction() : 0.f;
	const double RunTime = GetRunTimeSeconds();

	UE_LOG(LogHuebox, Display, TEXT("HueSelfTest: %s %s t=%.2f fraction=%.3f max=%.3f"),
		bPassed ? TEXT("PASS") : TEXT("FAIL"), *Name, RunTime, Fraction, MaxFraction);

	if (!bPassed)
	{
		UE_LOG(LogHuebox, Display, TEXT("HueSelfTest:   reason: %s"), *Reason);

		if (IsValid(Board))
		{
			UE_LOG(LogHuebox, Display, TEXT("HueSelfTest:   objective %d/%d"),
				Board->GetCurrentObjectiveIndex(), Board->GetObjectives().Num());

			if (const AHueReceiverBlock* Receiver = Board->GetReceiver())
			{
				FString History;
				for (const FHueColorEvent& Event : Receiver->GetAppliedHistory())
				{
					History += FString::Printf(TEXT(" %s@%.2fs"), HueColor::Label(Event.Color), Event.TimeSeconds);
				}
				UE_LOG(LogHuebox, Display, TEXT("HueSelfTest:   receiver applied:%s conflicts=%d"),
					History.IsEmpty() ? TEXT(" none") : *History, Receiver->GetConflictCount());
			}
		}

		// The run log survives StopAndClear (it is cleared on the next StartRun).
		if (const UPulseSubsystem* PulseSim = Sim.Get())
		{
			const TArray<FHueFeedback>& FeedbackLog = PulseSim->GetRunFeedbackLog();
			for (int32 Index = FMath::Max(0, FeedbackLog.Num() - 8); Index < FeedbackLog.Num(); ++Index)
			{
				UE_LOG(LogHuebox, Display, TEXT("HueSelfTest:   sim: %s"), *FeedbackLog[Index].Message);
			}
		}
	}

	if (bPassed)
	{
		++SelfTestPassed;
	}
	else
	{
		++SelfTestFailed;
	}

	PushFeedback(bPassed ? EHueFeedbackKind::Success : EHueFeedbackKind::Error,
		FString::Printf(TEXT("Self-test %s: %s"), bPassed ? TEXT("PASS") : TEXT("FAIL"), *Name), Board);

	ResetAttempt();
}

void AExhibitManager::FinishSelfTest()
{
	bSelfTestActive = false;
	SelfTestBoardIndex = INDEX_NONE;
	SelfTestBoard.Reset();

	UE_LOG(LogHuebox, Display, TEXT("HueSelfTest: SUMMARY passed=%d failed=%d"), SelfTestPassed, SelfTestFailed);

	// Only a command-line validation run quits; a manual BeginSelfTest() just reports.
	if (bSelfTest)
	{
		FPlatformMisc::RequestExit(false, TEXT("AExhibitManager::FinishSelfTest"));
	}
}
