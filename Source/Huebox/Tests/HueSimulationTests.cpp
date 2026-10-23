// Huebox automation tests: direction / colour math (additive light: masks, mixing, complements), the inventory rules,
// the deterministic pulse simulation with exact logical timings (colour bricks included), ordered objectives, the four
// Level 1 tutorial exercises end to end, and object lifetime
// (weak references to removed blocks, world teardown + garbage collection). The dual-view pieces are covered too:
// build-camera height, grid cursor and stage-camera math, minimap pulse snapshots, and the build tool placing from /
// collecting into a board tray, with Instruction-mode trays filled from the layouts.
//
// Run: Session Frontend > Automation, filter "Huebox" - or headless:
//   UnrealEditor-Cmd.exe Huebox.uproject -ExecCmds="Automation RunTests Huebox; Quit" -unattended -nullrhi
//
// Gameplay feedback (blocked ports, mixer failures, limits) is asserted through UPulseSubsystem::GetRunFeedbackLog.
// The subsystem logs those lines at Display verbosity, so provoked failures never count as test errors, while a
// genuine LogHuebox / engine error during a test still fails it.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "Blocks/HueFixedBlocks.h"
#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "Core/HueTypes.h"
#include "Game/ExhibitManager.h"
#include "Game/HueStageCamera.h"
#include "Game/HueTutorialBuilder.h"
#include "Game/HueboxGameMode.h"
#include "Grid/HueBoard.h"
#include "Player/HueBuildToolComponent.h"
#include "Player/HueBuilderPawn.h"
#include "Player/HueInteractable.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"
#include "Rig/ReactiveRig.h"
#include "Simulation/PulseSubsystem.h"

/** Same flag combination the engine uses for its own editor-context world tests (EngineAutomationTests.cpp). */
#define HUEBOX_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Named namespace (not anonymous): unity builds merge several .cpp files into one translation unit.
namespace HueboxTests
{
	const EHueDir AllDirs[] = { EHueDir::East, EHueDir::North, EHueDir::West, EHueDir::South };

	// ---- Test world (RAII) ------------------------------------------------------------------------------------

	/**
	 * A game world that lives exactly as long as one test. Mirrors the engine's FTestWorldWrapper
	 * (Engine/Private/Tests/AutomationCommon.cpp) and CQTest's FActorTestSpawner: CreateWorld + world context +
	 * InitializeActorsForPlay + BeginPlay here, the reverse in the destructor - so a UTEST_* early `return false`
	 * still ends play, destroys the context and the world.
	 *
	 * No UGameInstance on purpose: with one, BeginPlay would spawn the project's default game mode
	 * (AHueboxGameMode), which auto-builds the whole tutorial plus an exhibit manager. Without a game mode
	 * UWorld::BeginPlay does not dispatch actor BeginPlay, so we call AWorldSettings::NotifyBeginPlay ourselves -
	 * exactly what AGameStateBase::HandleBeginPlay does in a real game. Actors spawned afterwards therefore get
	 * BeginPlay inside SpawnActor and EndPlay on teardown (timers cleared, delegates unbound), as in PIE.
	 *
	 * The UWorld is held by a raw pointer on purpose: UWorld::CreateWorld (bAddToRoot = true) adds it to the root
	 * set until DestroyWorld, so the GC cannot collect it while this helper lives. The helper is a plain stack object
	 * the GC never sees, so a TObjectPtr member would add no protection, and a TStrongObjectPtr would only duplicate
	 * the root and blur the explicit teardown order. Actors spawned into the world are kept alive by their ULevel;
	 * tests keep them in locals only within one synchronous RunTest (no GC can run in between) and switch to
	 * TWeakObjectPtr wherever an object may die before the test ends.
	 */
	struct FHueTestWorld
	{
		FHueTestWorld()
		{
			if (!GEngine)
			{
				return;
			}

			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
			if (!World)
			{
				return;
			}
			// Driven manually through UPulseSubsystem::StepSimulation; the engine loop never ticks it.
			World->SetShouldTick(false);

			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);

			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			if (AWorldSettings* Settings = World->GetWorldSettings(/*bCheckStreamingPersistent*/ false, /*bChecked*/ false))
			{
				Settings->NotifyBeginPlay();
			}

			if (UPulseSubsystem* Sim = GetSim())
			{
				Sim->SetVisualsEnabled(false); // logic only: no pooled pulse actors in tests
			}
		}

		~FHueTestWorld()
		{
			// Same guard as FTestWorldWrapper: nothing to tear down during a hard exit.
			if (!World || !UObjectInitialized() || IsEngineExitRequested())
			{
				return;
			}

			if (World->HasBegunPlay())
			{
				World->BeginTearingDown();
				World->EndPlay(EEndPlayReason::Quit); // routes EndPlay to every actor
			}
			if (GEngine)
			{
				GEngine->DestroyWorldContext(World);
			}
			// CleanupWorld deinitializes the subsystems (UPulseSubsystem cancels its events), then the world is unrooted.
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			World = nullptr;
		}

		FHueTestWorld(const FHueTestWorld&) = delete;
		FHueTestWorld& operator=(const FHueTestWorld&) = delete;

		UWorld* Get() const { return World; }
		UPulseSubsystem* GetSim() const { return World ? World->GetSubsystem<UPulseSubsystem>() : nullptr; }

		/** Boards are spawned unrotated (the grid and the rig DOF locks are world-axis aligned). */
		AHueBoard* SpawnBoard(const FVector& Origin, const FIntPoint& GridSize) const
		{
			if (!World)
			{
				return nullptr;
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AHueBoard* Board = World->SpawnActor<AHueBoard>(Origin, FRotator::ZeroRotator, Params);
			if (Board)
			{
				Board->SetGridSize(GridSize);
			}
			return Board;
		}

		/** Deferred spawn so TrackLength is known before construction / BeginPlay lay out the track. */
		AReactiveRig* SpawnRig(const FVector& Location, float TrackLength) const
		{
			if (!World)
			{
				return nullptr;
			}
			const FTransform SpawnTransform(FRotator::ZeroRotator, Location);
			AReactiveRig* Rig = World->SpawnActorDeferred<AReactiveRig>(AReactiveRig::StaticClass(), SpawnTransform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Rig)
			{
				Rig->TrackLength = TrackLength;
				Rig->FinishSpawning(SpawnTransform);
			}
			return Rig;
		}

	private:
		UWorld* World = nullptr;
	};

	/** Several independent boards share one test world; keep them far apart. */
	FVector BoardOrigin(int32 Index)
	{
		return FVector(2000.0 * Index, 0.0, 0.0);
	}

	/**
	 * Drives the logical simulation to absolute run times. Targets are exact binary fractions (0.25 s steps), so
	 * every millisecond boundary is hit exactly. Create one clock per StartRun (StartRun resets logical time).
	 * The subsystem pointer is non-owning: the world owns its subsystem and outlives the clock.
	 */
	struct FSimClock
	{
		explicit FSimClock(UPulseSubsystem* InSim) : Sim(InSim) {}

		void AdvanceTo(double Seconds)
		{
			if (Sim && Seconds > Elapsed)
			{
				Sim->StepSimulation(Seconds - Elapsed);
				Elapsed = Seconds;
			}
		}

	private:
		UPulseSubsystem* Sim = nullptr;
		double Elapsed = 0.0;
	};

	bool StartBoardRun(UPulseSubsystem* Sim, AHueBoard* Board)
	{
		if (!Sim || !Board)
		{
			return false;
		}
		Board->ResetAllBlocks();
		return Sim->StartRun(Board) && Sim->IsRunning();
	}

	// ---- Names / descriptions for readable failures -----------------------------------------------------------

	const EHueColor AllColors[] = { EHueColor::A, EHueColor::B, EHueColor::N, EHueColor::G, EHueColor::Y, EHueColor::M, EHueColor::V };

	const TCHAR* ColorName(EHueColor Color)
	{
		return HueColor::Label(Color);
	}

	const TCHAR* DirName(EHueDir Dir)
	{
		switch (Dir)
		{
		case EHueDir::East:  return TEXT("East");
		case EHueDir::North: return TEXT("North");
		case EHueDir::West:  return TEXT("West");
		default:             return TEXT("South");
		}
	}

	const TCHAR* FeedbackKindLabel(EHueFeedbackKind Kind)
	{
		switch (Kind)
		{
		case EHueFeedbackKind::Success: return TEXT("Success");
		case EHueFeedbackKind::Warning: return TEXT("Warning");
		case EHueFeedbackKind::Error:   return TEXT("Error");
		default:                        return TEXT("Info");
		}
	}

	FString ModuleName(EHueModuleType Type)
	{
		return UEnum::GetValueAsString(Type);
	}

	FHueColorEvent MakeEvent(float TimeSeconds, EHueColor Color)
	{
		FHueColorEvent Event;
		Event.TimeSeconds = TimeSeconds;
		Event.Color = Color;
		return Event;
	}

	FString DescribeHistory(const TArray<FHueColorEvent>& History)
	{
		if (History.Num() == 0)
		{
			return TEXT("(empty)");
		}
		TArray<FString> Parts;
		for (const FHueColorEvent& Event : History)
		{
			Parts.Add(FString::Printf(TEXT("%s@%.3fs"), ColorName(Event.Color), Event.TimeSeconds));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	FString DescribeFeedback(const TArray<FHueFeedback>& Log)
	{
		if (Log.Num() == 0)
		{
			return TEXT("(no feedback)");
		}
		TArray<FString> Lines;
		for (const FHueFeedback& Line : Log)
		{
			Lines.Add(FString::Printf(TEXT("[%s] %s"), FeedbackKindLabel(Line.Kind), *Line.Message));
		}
		return FString::Join(Lines, TEXT(" | "));
	}

	// ---- Assertion helpers ------------------------------------------------------------------------------------

	bool TestColor(FAutomationTestBase& Test, const FString& What, EHueColor Actual, EHueColor Expected)
	{
		if (Actual != Expected)
		{
			Test.AddError(FString::Printf(TEXT("%s: colour is %s, expected %s"), *What, ColorName(Actual), ColorName(Expected)));
			return false;
		}
		return true;
	}

	bool TestDir(FAutomationTestBase& Test, const FString& What, EHueDir Actual, EHueDir Expected)
	{
		if (Actual != Expected)
		{
			Test.AddError(FString::Printf(TEXT("%s: direction is %s, expected %s"), *What, DirName(Actual), DirName(Expected)));
			return false;
		}
		return true;
	}

	/** Exact colour sequence and logical times (integer ms underneath, so a tight tolerance is enough). */
	bool ExpectHistory(FAutomationTestBase& Test, const FString& What, const TArray<FHueColorEvent>& Actual, const TArray<FHueColorEvent>& Expected)
	{
		bool bMatch = Actual.Num() == Expected.Num();
		for (int32 Index = 0; bMatch && Index < Actual.Num(); ++Index)
		{
			bMatch = Actual[Index].Color == Expected[Index].Color
				&& FMath::IsNearlyEqual(Actual[Index].TimeSeconds, Expected[Index].TimeSeconds, 1e-4f);
		}
		if (!bMatch)
		{
			Test.AddError(FString::Printf(TEXT("%s: receiver history is [%s], expected [%s]"),
				*What, *DescribeHistory(Actual), *DescribeHistory(Expected)));
		}
		return bMatch;
	}

	int32 CountFeedback(const TArray<FHueFeedback>& Log, const TCHAR* Substring)
	{
		int32 Count = 0;
		for (const FHueFeedback& Line : Log)
		{
			Count += Line.Message.Contains(Substring) ? 1 : 0; // case-insensitive by default
		}
		return Count;
	}

	int32 CountFeedbackOfKind(const TArray<FHueFeedback>& Log, EHueFeedbackKind Kind)
	{
		int32 Count = 0;
		for (const FHueFeedback& Line : Log)
		{
			Count += Line.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	const FHueFeedback* FindFeedback(const TArray<FHueFeedback>& Log, const TCHAR* Substring)
	{
		for (const FHueFeedback& Line : Log)
		{
			if (Line.Message.Contains(Substring))
			{
				return &Line;
			}
		}
		return nullptr;
	}

	const FHueFeedback* FindFeedbackAt(const TArray<FHueFeedback>& Log, EHueFeedbackKind Kind, const FIntPoint& Cell)
	{
		for (const FHueFeedback& Line : Log)
		{
			if (Line.Kind == Kind && Line.Cell == Cell)
			{
				return &Line;
			}
		}
		return nullptr;
	}

	bool ExpectFeedback(FAutomationTestBase& Test, const FString& What, const TArray<FHueFeedback>& Log, const TCHAR* Substring, bool bExpected)
	{
		const bool bFound = CountFeedback(Log, Substring) > 0;
		if (bFound != bExpected)
		{
			Test.AddError(FString::Printf(TEXT("%s: feedback %s \"%s\". Log: %s"), *What,
				bExpected ? TEXT("should contain") : TEXT("should not contain"), Substring, *DescribeFeedback(Log)));
			return false;
		}
		return true;
	}

	void TestSlot(FAutomationTestBase& Test, const UHueInventoryComponent* Inventory, int32 Index, EHueModuleType Type, int32 Count, const FString& Context)
	{
		const TArray<FHueInventorySlot>& Slots = Inventory->GetSlots();
		if (!Slots.IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(TEXT("%s: slot %d does not exist"), *Context, Index));
			return;
		}
		// Empty slots must read as (None, 0).
		const FHueInventorySlot& Slot = Slots[Index];
		if (Slot.Type != Type || Slot.Count != Count)
		{
			Test.AddError(FString::Printf(TEXT("%s: slot %d holds %s x%d, expected %s x%d"),
				*Context, Index, *ModuleName(Slot.Type), Slot.Count, *ModuleName(Type), Count));
		}
	}

	// ---- Block placement helpers --------------------------------------------------------------------------------

	template <typename TBlock>
	TBlock* AddBlock(FAutomationTestBase& Test, AHueBoard* Board, const FIntPoint& Cell, EHueDir Facing, bool bRemovable = true)
	{
		TBlock* Block = Board ? Cast<TBlock>(Board->SpawnBlock(TBlock::StaticClass(), Cell, Facing, bRemovable)) : nullptr;
		if (!Block)
		{
			Test.AddError(FString::Printf(TEXT("Could not place %s at (%d,%d)"), *TBlock::StaticClass()->GetName(), Cell.X, Cell.Y));
		}
		return Block;
	}

	AHueSourceBlock* AddSource(FAutomationTestBase& Test, AHueBoard* Board, const FIntPoint& Cell, EHueDir Facing, EHueColor Color)
	{
		AHueSourceBlock* Source = AddBlock<AHueSourceBlock>(Test, Board, Cell, Facing, /*bRemovable*/ false);
		if (Source)
		{
			Source->SetEmitColor(Color);
		}
		return Source;
	}

	AHueReceiverBlock* AddReceiver(FAutomationTestBase& Test, AHueBoard* Board, const FIntPoint& Cell, const TArray<EHueDir>& AcceptFaces)
	{
		AHueReceiverBlock* Receiver = AddBlock<AHueReceiverBlock>(Test, Board, Cell, EHueDir::East, /*bRemovable*/ false);
		if (Receiver)
		{
			Receiver->SetAcceptFaces(AcceptFaces);
		}
		return Receiver;
	}

	AHueDelayBlock* AddDelay(FAutomationTestBase& Test, AHueBoard* Board, const FIntPoint& Cell, EHueDir Facing, float Seconds)
	{
		AHueDelayBlock* Delay = AddBlock<AHueDelayBlock>(Test, Board, Cell, Facing);
		if (Delay)
		{
			Delay->SetDelaySeconds(Seconds);
		}
		return Delay;
	}

	/** A pulse handed straight to a block, as the subsystem would on arrival (used where routing cannot reach). */
	FPulseData MakeInjectedPulse(const UPulseSubsystem* Sim, const APulseBlock* Block, EHueColor Color, EHueDir TravelDir)
	{
		FPulseData Pulse;
		Pulse.Color = Color;
		Pulse.Cell = Block->GetCell();
		Pulse.Direction = TravelDir;
		Pulse.ArrivalTimeMs = Sim->GetSimTimeMs();
		Pulse.RemainingOps = Sim->GetMaxOpsPerSignal();
		return Pulse;
	}

	// ---- Mixer scenario -----------------------------------------------------------------------------------------

	/** Raw pointers: stack-only view of actors owned by the test world's level, used within one RunTest. */
	struct FMixerScenario
	{
		AHueBoard* Board = nullptr;
		AHueMixerBlock* Mixer = nullptr;
		AHueReceiverBlock* Receiver = nullptr;
	};

	/**
	 * 9x6 board. Mixer M(4,2) facing North: input 0 = West face (pulses travelling East), input 1 = East face
	 * (travelling West); the N output travels North through (4,3) into R(4,4) (South face).
	 * A source at (x,2) facing East reaches input 0 at (4 - x) * 0.25 s; one at (x,2) facing West reaches input 1 at
	 * (x - 4) * 0.25 s. A primer source at (1,4) turns R to B at 0.75 s, so a later N is always a visible change.
	 */
	bool BuildMixerScenario(FAutomationTestBase& Test, const FHueTestWorld& TestWorld, int32 BoardIndex,
		int32 WestSourceX, EHueColor WestColor, int32 EastSourceX, EHueColor EastColor, FMixerScenario& Out)
	{
		Out = FMixerScenario();
		Out.Board = TestWorld.SpawnBoard(BoardOrigin(BoardIndex), FIntPoint(9, 6));
		if (!Out.Board)
		{
			Test.AddError(TEXT("Mixer scenario: board could not be spawned"));
			return false;
		}
		Out.Mixer = AddBlock<AHueMixerBlock>(Test, Out.Board, FIntPoint(4, 2), EHueDir::North);
		Out.Receiver = AddReceiver(Test, Out.Board, FIntPoint(4, 4), { EHueDir::South, EHueDir::West });
		const bool bPrimer = AddSource(Test, Out.Board, FIntPoint(1, 4), EHueDir::East, EHueColor::B) != nullptr;
		const bool bWest = AddSource(Test, Out.Board, FIntPoint(WestSourceX, 2), EHueDir::East, WestColor) != nullptr;
		const bool bEast = AddSource(Test, Out.Board, FIntPoint(EastSourceX, 2), EHueDir::West, EastColor) != nullptr;
		return Out.Mixer && Out.Receiver && bPrimer && bWest && bEast;
	}

	// ---- Tutorial helpers ---------------------------------------------------------------------------------------

	struct FTutorialFixture
	{
		AHueBoard* Board = nullptr;
		AReactiveRig* Rig = nullptr;
		AHueReceiverBlock* Receiver = nullptr;
	};

	/** What one logic run produced (copied out before StopAndClear). */
	struct FLogicRun
	{
		TArray<FHueColorEvent> History;
		TArray<FHueFeedback> Feedback;
		int32 ConflictCount = 0;
	};

	struct FObjectiveRun
	{
		FHueMotionPrediction Prediction;
		float CompletedAt = -1.f;
		bool bComplete = false;
	};

	EHueModuleType ModuleTypeForKind(EHueBlockKind Kind)
	{
		switch (Kind)
		{
		case EHueBlockKind::Turn:       return EHueModuleType::Turn;
		case EHueBlockKind::ColorShift: return EHueModuleType::ColorShift;
		case EHueBlockKind::Delay:      return EHueModuleType::Delay;
		case EHueBlockKind::Mixer:      return EHueModuleType::Mixer;
		default:                        return EHueModuleType::None;
		}
	}

	/** Graybox data checks: cells inside, one receiver, solution cells free or removable, supplies cover the solution. */
	void CheckTutorialLayoutData(FAutomationTestBase& Test, const FHueBoardLayout& Layout)
	{
		const FString& Name = Layout.Name;
		Test.TestFalse(TEXT("Layout has a name"), Name.IsEmpty());
		Test.TestTrue(FString::Printf(TEXT("%s: has objectives"), *Name), Layout.Objectives.Num() > 0);
		Test.TestTrue(FString::Printf(TEXT("%s: has a baseline solution"), *Name), Layout.SolutionBlocks.Num() > 0);

		auto IsInside = [&Layout](const FIntPoint& Cell)
		{
			return Cell.X >= 0 && Cell.Y >= 0 && Cell.X < Layout.GridSize.X && Cell.Y < Layout.GridSize.Y;
		};

		TMap<FIntPoint, bool> RemovableByCell;
		TMap<EHueModuleType, int32> Supply;
		int32 Sources = 0;
		int32 Receivers = 0;
		for (const FHueBlockSpec& Spec : Layout.Blocks)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: block (%d,%d) inside the grid"), *Name, Spec.Cell.X, Spec.Cell.Y), IsInside(Spec.Cell));
			Test.TestFalse(FString::Printf(TEXT("%s: one block per cell (%d,%d)"), *Name, Spec.Cell.X, Spec.Cell.Y), RemovableByCell.Contains(Spec.Cell));
			RemovableByCell.Add(Spec.Cell, Spec.bRemovable);

			Sources += Spec.Kind == EHueBlockKind::Source ? 1 : 0;
			if (Spec.Kind == EHueBlockKind::Receiver)
			{
				++Receivers;
				Test.TestTrue(FString::Printf(TEXT("%s: receiver accepts at least one face"), *Name), Spec.ReceiverFaces.Num() > 0);
			}
			// A removable pre-placed module can be collected and reused, so it counts as supply.
			const EHueModuleType Module = ModuleTypeForKind(Spec.Kind);
			if (Spec.bRemovable && Module != EHueModuleType::None)
			{
				Supply.FindOrAdd(Module) += 1;
			}
		}
		Test.TestTrue(FString::Printf(TEXT("%s: at least one source"), *Name), Sources > 0);
		Test.TestEqual(FString::Printf(TEXT("%s: exactly one receiver"), *Name), Receivers, 1);

		for (const FHuePickupSpec& Pickup : Layout.Pickups)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: pickup %s has a positive count"), *Name, *ModuleName(Pickup.Type)), Pickup.Count > 0);
			Supply.FindOrAdd(Pickup.Type) += Pickup.Count;
		}

		TMap<EHueModuleType, int32> Needed;
		for (const FHueBlockSpec& Spec : Layout.SolutionBlocks)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: solution (%d,%d) inside the grid"), *Name, Spec.Cell.X, Spec.Cell.Y), IsInside(Spec.Cell));
			if (const bool* bRemovable = RemovableByCell.Find(Spec.Cell))
			{
				Test.TestTrue(FString::Printf(TEXT("%s: solution (%d,%d) only replaces a removable block"), *Name, Spec.Cell.X, Spec.Cell.Y), *bRemovable);
			}
			const EHueModuleType Module = ModuleTypeForKind(Spec.Kind);
			Test.TestTrue(FString::Printf(TEXT("%s: solution (%d,%d) is a placeable module"), *Name, Spec.Cell.X, Spec.Cell.Y), Module != EHueModuleType::None);
			Needed.FindOrAdd(Module) += 1;
		}
		for (const TPair<EHueModuleType, int32>& Need : Needed)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: the player can obtain %d x %s (supply %d)"), *Name, Need.Value, *ModuleName(Need.Key), Supply.FindRef(Need.Key)),
				Supply.FindRef(Need.Key) >= Need.Value);
		}
	}

	/** Board + rig (deferred, TrackLength) + every layout block via SpawnBlockFromSpec + objectives. */
	bool SpawnTutorialBoard(FAutomationTestBase& Test, const FHueTestWorld& TestWorld, const FHueBoardLayout& Layout, FTutorialFixture& Out)
	{
		Out = FTutorialFixture();
		Out.Board = TestWorld.SpawnBoard(Layout.Origin, Layout.GridSize);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: board spawned"), *Layout.Name), Out.Board))
		{
			return false;
		}
		Out.Board->SetTexts(Layout.Name, Layout.Brief);

		Out.Rig = TestWorld.SpawnRig(Layout.Origin + Layout.RigOffset, Layout.TrackLength);
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: rig spawned"), *Layout.Name), Out.Rig))
		{
			return false;
		}
		Test.TestEqual(FString::Printf(TEXT("%s: rig uses the layout track length"), *Layout.Name), Out.Rig->GetTrackLength(), Layout.TrackLength);
		Out.Board->SetRig(Out.Rig);

		bool bAllSpawned = true;
		for (const FHueBlockSpec& Spec : Layout.Blocks)
		{
			if (!AHueTutorialBuilder::SpawnBlockFromSpec(Out.Board, Spec, Out.Rig))
			{
				Test.AddError(FString::Printf(TEXT("%s: block at (%d,%d) could not be spawned"), *Layout.Name, Spec.Cell.X, Spec.Cell.Y));
				bAllSpawned = false;
			}
		}
		Out.Board->SetObjectives(Layout.Objectives);

		Out.Receiver = Out.Board->GetReceiver();
		if (!Test.TestNotNull(FString::Printf(TEXT("%s: receiver present"), *Layout.Name), Out.Receiver))
		{
			return false;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: receiver drives the board's rig"), *Layout.Name), Out.Receiver->GetRig() == Out.Rig);
		return bAllSpawned;
	}

	/** Mirrors AExhibitManager::StartRun (reset blocks, objectives, rig), then runs the logic for the run time limit. */
	bool RunTutorialLogic(FAutomationTestBase& Test, UPulseSubsystem* Sim, const FTutorialFixture& Fixture, double FrameSeconds, FLogicRun& Out)
	{
		Fixture.Board->ResetAllBlocks();
		Fixture.Board->ResetObjectives();
		Fixture.Rig->ResetRig();
		if (!Test.TestTrue(FString::Printf(TEXT("%s: run started"), *Fixture.Board->GetBoardName()), Sim->StartRun(Fixture.Board)))
		{
			return false;
		}

		const double Duration = Fixture.Board->RunTimeLimitSeconds;
		double Elapsed = 0.0;
		while (Elapsed < Duration - UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			const double Step = FMath::Min(FrameSeconds, Duration - Elapsed);
			Sim->StepSimulation(Step);
			Elapsed += Step;
		}

		Out.History = Fixture.Receiver->GetAppliedHistory();
		Out.Feedback = Sim->GetRunFeedbackLog();
		Out.ConflictCount = Fixture.Receiver->GetConflictCount();
		Sim->StopAndClear();
		return true;
	}

	/** Feeds the receiver history into the analytic rig model and evaluates the board objectives every step. */
	FObjectiveRun EvaluateTutorialObjectives(const FTutorialFixture& Fixture, const TArray<FHueColorEvent>& History)
	{
		FObjectiveRun Result;
		AHueBoard* Board = Fixture.Board;
		Board->ResetObjectives();

		float Elapsed = 0.f;
		float CompletedAt = -1.f;
		// TFunctionRef callback, invoked synchronously inside PredictMotion (no timer, nothing outlives this call).
		Result.Prediction = Fixture.Rig->PredictMotion(History, Board->RunTimeLimitSeconds, 1.f / 120.f,
			[Board, &Elapsed, &CompletedAt](const FHueRigSample& Sample, float StepSeconds)
			{
				Elapsed += StepSeconds;
				// Like the exhibit manager: objectives tick every frame until the board completes, then the run ends.
				if (CompletedAt < 0.f && Board->EvaluateObjectives(Sample, StepSeconds))
				{
					CompletedAt = Elapsed;
				}
			});
		Result.CompletedAt = CompletedAt;
		Result.bComplete = Board->AreObjectivesComplete();
		return Result;
	}

	/**
	 * Exhibit manager for run-control tests. Spawn it before the boards (they register from their BeginPlay). No game
	 * mode in the test world, so AExhibitManager::Get finds it through its actor search. The world is never ticked:
	 * tests call Tick themselves after stepping the simulation.
	 */
	AExhibitManager* SpawnManager(const FHueTestWorld& TestWorld)
	{
		UWorld* World = TestWorld.Get();
		if (!World)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<AExhibitManager>(AExhibitManager::StaticClass(), FTransform::Identity, Params);
	}

	AHueMixerBlock* FindMixer(const AHueBoard* Board)
	{
		AHueMixerBlock* Found = nullptr;
		Board->ForEachBlock([&Found](APulseBlock& Block)
		{
			if (!Found)
			{
				Found = Cast<AHueMixerBlock>(&Block);
			}
		});
		return Found;
	}

	const FHueObjective* FindObjective(const FHueBoardLayout& Layout, EHueObjectiveType Type)
	{
		return Layout.Objectives.FindByPredicate([Type](const FHueObjective& Objective) { return Objective.Type == Type; });
	}

	// ---- Objective helpers --------------------------------------------------------------------------------------

	FHueObjective MakeObjective(EHueObjectiveType Type, float TrackFraction)
	{
		FHueObjective Objective;
		Objective.Type = Type;
		Objective.TrackFraction = TrackFraction;
		Objective.Description = UEnum::GetValueAsString(Type);
		return Objective;
	}

	FHueObjective MakeStopObjective(float ZoneMin, float ZoneMax, float MaxSpeedFraction, float HoldSeconds)
	{
		FHueObjective Objective = MakeObjective(EHueObjectiveType::StopInZone, 0.f);
		Objective.ZoneMin = ZoneMin;
		Objective.ZoneMax = ZoneMax;
		Objective.MaxSpeedFraction = MaxSpeedFraction;
		Objective.HoldSeconds = HoldSeconds;
		return Objective;
	}

	FHueObjective MakeColorObjective(EHueColor Color)
	{
		FHueObjective Objective = MakeObjective(EHueObjectiveType::ReceiverColor, 0.f);
		Objective.Color = Color;
		return Objective;
	}

	FHueRigSample MakeSample(float TrackFraction, float SpeedFraction, EHueColor Color = EHueColor::N)
	{
		FHueRigSample Sample;
		Sample.TrackFraction = TrackFraction;
		Sample.SpeedFraction = SpeedFraction;
		Sample.ReceiverColor = Color;
		return Sample;
	}

	// ---- Views / build tool helpers -----------------------------------------------------------------------------

	/** Always-spawn helper for actors without deferred setup (the world has begun play: BeginPlay runs in SpawnActor). */
	template <typename TActor>
	TActor* SpawnTestActor(const FHueTestWorld& TestWorld, const FVector& Location = FVector::ZeroVector)
	{
		UWorld* World = TestWorld.Get();
		if (!World)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<TActor>(TActor::StaticClass(), FTransform(Location), Params);
	}

	/**
	 * A bare actor standing in for a player pawn: a scene root (the tool attaches its placement ghost to the owner's
	 * root) and a UHueBuildToolComponent paying from Inventory. Both components are created at runtime:
	 * AddInstanceComponent lists them in the actor's InstanceComponents UPROPERTY (the GC reference), and
	 * RegisterComponent routes their BeginPlay because the owner has already begun play. They end play with the actor
	 * when the test world tears down (the tool's EndPlay destroys its preview components).
	 */
	UHueBuildToolComponent* SpawnBuildTool(const FHueTestWorld& TestWorld, UHueInventoryComponent* Inventory)
	{
		// Identity transform: an actor without a root component cannot take a spawn location anyway.
		AActor* Owner = SpawnTestActor<AActor>(TestWorld);
		if (!Owner)
		{
			return nullptr;
		}

		USceneComponent* Root = NewObject<USceneComponent>(Owner, TEXT("ToolOwnerRoot"));
		Owner->SetRootComponent(Root);
		Owner->AddInstanceComponent(Root);
		Root->RegisterComponent();

		UHueBuildToolComponent* Tool = NewObject<UHueBuildToolComponent>(Owner, TEXT("BuildTool"));
		Owner->AddInstanceComponent(Tool);
		Tool->RegisterComponent();
		Tool->SetInventory(Inventory);
		return Tool;
	}

	const TCHAR* TargetKindName(EHueTargetKind Kind)
	{
		switch (Kind)
		{
		case EHueTargetKind::Block:  return TEXT("Block");
		case EHueTargetKind::Pickup: return TEXT("Pickup");
		case EHueTargetKind::Cell:   return TEXT("Cell");
		default:                     return TEXT("None");
		}
	}

	/** The tool's current target is of kind Expected (a failure names the actual kind and its reason text). */
	bool ExpectTargetKind(FAutomationTestBase& Test, const FString& What, const UHueBuildToolComponent* Tool, EHueTargetKind Expected)
	{
		const FHueInteractionTarget& Target = Tool->GetTarget();
		if (Target.Kind != Expected)
		{
			Test.AddError(FString::Printf(TEXT("%s: target is %s, expected %s (reason \"%s\")"),
				*What, TargetKindName(Target.Kind), TargetKindName(Expected), *Target.Reason));
			return false;
		}
		return true;
	}

	/** Empty-cell target that accepts (or refuses) the selected module. A refusal must explain itself to the player. */
	bool ExpectCellTarget(FAutomationTestBase& Test, const FString& What, const UHueBuildToolComponent* Tool, bool bCanPlace)
	{
		if (!ExpectTargetKind(Test, What, Tool, EHueTargetKind::Cell))
		{
			return false;
		}
		const FHueInteractionTarget& Target = Tool->GetTarget();
		if (Target.bCanPlace != bCanPlace)
		{
			Test.AddError(FString::Printf(TEXT("%s: bCanPlace is %s, expected %s (reason \"%s\")"), *What,
				Target.bCanPlace ? TEXT("true") : TEXT("false"), bCanPlace ? TEXT("true") : TEXT("false"), *Target.Reason));
			return false;
		}
		if (!bCanPlace && Target.Reason.IsEmpty())
		{
			Test.AddError(FString::Printf(TEXT("%s: placement refused without a reason"), *What));
			return false;
		}
		return true;
	}

	TArray<FHuePulseSnapshot> GetSnapshots(const UPulseSubsystem* Sim)
	{
		TArray<FHuePulseSnapshot> Snapshots;
		if (Sim)
		{
			Sim->GetPulseSnapshots(Snapshots);
		}
		return Snapshots;
	}

	/** Exactly one travelling pulse, at fractional cell coordinates Expected, with Color. */
	bool ExpectSingleSnapshot(FAutomationTestBase& Test, const FString& What, const UPulseSubsystem* Sim, const FVector2D& Expected, EHueColor Color)
	{
		const TArray<FHuePulseSnapshot> Snapshots = GetSnapshots(Sim);
		if (Snapshots.Num() != 1)
		{
			Test.AddError(FString::Printf(TEXT("%s: %d pulse snapshots, expected 1"), *What, Snapshots.Num()));
			return false;
		}
		const FHuePulseSnapshot& Snapshot = Snapshots[0];
		bool bMatch = TestColor(Test, What, Snapshot.Color, Color);
		if (!Snapshot.CellPosition.Equals(Expected, 1e-4))
		{
			Test.AddError(FString::Printf(TEXT("%s: pulse at (%.4f, %.4f), expected (%.4f, %.4f)"), *What,
				Snapshot.CellPosition.X, Snapshot.CellPosition.Y, Expected.X, Expected.Y));
			bMatch = false;
		}
		return bMatch;
	}
}

// =============================================================================================================
// 1. Core math
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxCoreDirectionsTest, "Huebox.Core.Directions", HUEBOX_TEST_FLAGS)
bool FHueboxCoreDirectionsTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// Board-local convention: East = +X, North = +Y.
	TestEqual(TEXT("ToOffset(East)"), HueDir::ToOffset(EHueDir::East), FIntPoint(1, 0));
	TestEqual(TEXT("ToOffset(North)"), HueDir::ToOffset(EHueDir::North), FIntPoint(0, 1));
	TestEqual(TEXT("ToOffset(West)"), HueDir::ToOffset(EHueDir::West), FIntPoint(-1, 0));
	TestEqual(TEXT("ToOffset(South)"), HueDir::ToOffset(EHueDir::South), FIntPoint(0, -1));

	TestDir(*this, TEXT("Opposite(East)"), HueDir::Opposite(EHueDir::East), EHueDir::West);
	TestDir(*this, TEXT("Opposite(North)"), HueDir::Opposite(EHueDir::North), EHueDir::South);
	TestDir(*this, TEXT("RotatePlus90(East)"), HueDir::RotatePlus90(EHueDir::East), EHueDir::North);
	TestDir(*this, TEXT("RotatePlus90(South) wraps"), HueDir::RotatePlus90(EHueDir::South), EHueDir::East);
	TestDir(*this, TEXT("RotateMinus90(East) wraps"), HueDir::RotateMinus90(EHueDir::East), EHueDir::South);
	TestDir(*this, TEXT("RotateMinus90(North)"), HueDir::RotateMinus90(EHueDir::North), EHueDir::East);

	TestEqual(TEXT("ToYaw(East)"), HueDir::ToYaw(EHueDir::East), 0.f);
	TestEqual(TEXT("ToYaw(North)"), HueDir::ToYaw(EHueDir::North), 90.f);
	TestEqual(TEXT("ToYaw(West)"), HueDir::ToYaw(EHueDir::West), 180.f);
	TestEqual(TEXT("ToYaw(South)"), HueDir::ToYaw(EHueDir::South), 270.f);

	for (int32 Index = 0; Index < 4; ++Index)
	{
		const EHueDir Dir = AllDirs[Index];
		const FString Name = DirName(Dir);

		// Enum order matters: index + 1 is a +90 degree turn.
		TestDir(*this, Name + TEXT(": index + 1 is RotatePlus90"), HueDir::RotatePlus90(Dir), AllDirs[(Index + 1) % 4]);
		TestDir(*this, Name + TEXT(": Opposite twice"), HueDir::Opposite(HueDir::Opposite(Dir)), Dir);
		TestDir(*this, Name + TEXT(": Rotate(2) == Opposite"), HueDir::Rotate(Dir, 2), HueDir::Opposite(Dir));
		TestDir(*this, Name + TEXT(": Rotate(0)"), HueDir::Rotate(Dir, 0), Dir);
		TestDir(*this, Name + TEXT(": Rotate(1) == RotatePlus90"), HueDir::Rotate(Dir, 1), HueDir::RotatePlus90(Dir));
		TestDir(*this, Name + TEXT(": Rotate(-1) == RotateMinus90"), HueDir::Rotate(Dir, -1), HueDir::RotateMinus90(Dir));
		TestDir(*this, Name + TEXT(": Rotate(4) is a full turn"), HueDir::Rotate(Dir, 4), Dir);
		TestDir(*this, Name + TEXT(": Rotate(-8) is two full turns"), HueDir::Rotate(Dir, -8), Dir);
		TestDir(*this, Name + TEXT(": Rotate(5) == RotatePlus90"), HueDir::Rotate(Dir, 5), HueDir::RotatePlus90(Dir));
		TestDir(*this, Name + TEXT(": Rotate(-7) == RotatePlus90"), HueDir::Rotate(Dir, -7), HueDir::RotatePlus90(Dir));
		TestDir(*this, Name + TEXT(": Rotate(7) == RotateMinus90"), HueDir::Rotate(Dir, 7), HueDir::RotateMinus90(Dir));
		TestDir(*this, Name + TEXT(": RotatePlus90 undoes RotateMinus90"), HueDir::RotatePlus90(HueDir::RotateMinus90(Dir)), Dir);

		const FIntPoint Offset = HueDir::ToOffset(Dir);
		TestEqual(Name + TEXT(": unit offset"), FMath::Abs(Offset.X) + FMath::Abs(Offset.Y), 1);
		TestEqual(Name + TEXT(": opposite offset is negated"), HueDir::ToOffset(HueDir::Opposite(Dir)), FIntPoint(-Offset.X, -Offset.Y));

		const FVector OffsetVector(static_cast<double>(Offset.X), static_cast<double>(Offset.Y), 0.0);
		TestEqual(Name + TEXT(": ToVector matches ToOffset"), HueDir::ToVector(Dir), OffsetVector, 1e-4f);
		// Yaw and vector name the same direction (blocks rotate their pivot by ToYaw, ports use ToVector).
		TestEqual(Name + TEXT(": yaw turns +X onto ToVector"), FRotator(0.0, HueDir::ToYaw(Dir), 0.0).Vector(), HueDir::ToVector(Dir), 1e-4f);
		TestEqual(Name + TEXT(": RotatePlus90 adds 90 degrees of yaw"), HueDir::ToYaw(HueDir::RotatePlus90(Dir)), FMath::Fmod(HueDir::ToYaw(Dir) + 90.f, 360.f));
		TestFalse(Name + TEXT(": has a display name"), FString(HueDir::ToString(Dir)).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxCoreColorsTest, "Huebox.Core.Colors", HUEBOX_TEST_FLAGS)
bool FHueboxCoreColorsTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// Colour-shift rule (GDD): A <-> B, N unchanged; generalised to the complement (G <-> M, Y <-> V). Applying it
	// twice is the identity.
	TestColor(*this, TEXT("Shift(A)"), HueColor::Shift(EHueColor::A), EHueColor::B);
	TestColor(*this, TEXT("Shift(B)"), HueColor::Shift(EHueColor::B), EHueColor::A);
	TestColor(*this, TEXT("Shift(N)"), HueColor::Shift(EHueColor::N), EHueColor::N);
	TestColor(*this, TEXT("Shift(G)"), HueColor::Shift(EHueColor::G), EHueColor::M);
	TestColor(*this, TEXT("Shift(Y)"), HueColor::Shift(EHueColor::Y), EHueColor::V);
	for (const EHueColor Color : AllColors)
	{
		TestColor(*this, FString::Printf(TEXT("Shift(Shift(%s))"), ColorName(Color)), HueColor::Shift(HueColor::Shift(Color)), Color);
	}

	// Additive light: every colour is a distinct, non-empty set of primaries, and FromMask inverts ToMask.
	for (const EHueColor Color : AllColors)
	{
		const uint8 Mask = HueColor::ToMask(Color);
		TestTrue(FString::Printf(TEXT("%s holds at least one primary"), ColorName(Color)), Mask != 0);
		EHueColor Back = EHueColor::N;
		TestTrue(FString::Printf(TEXT("FromMask(ToMask(%s)) exists"), ColorName(Color)), HueColor::FromMask(Mask, Back));
		TestColor(*this, FString::Printf(TEXT("FromMask(ToMask(%s))"), ColorName(Color)), Back, Color);
	}
	EHueColor NoLight = EHueColor::A;
	TestFalse(TEXT("No primaries is no colour"), HueColor::FromMask(0, NoLight));

	// Mixer rule: disjoint primaries add up; any shared primary fails (matching colours, N, A + Y).
	struct FMixCase { EHueColor First; EHueColor Second; bool bMixes; EHueColor Expected; };
	const FMixCase MixCases[] =
	{
		{ EHueColor::A, EHueColor::B, true, EHueColor::N },		// the Level 1 stop
		{ EHueColor::B, EHueColor::A, true, EHueColor::N },
		{ EHueColor::A, EHueColor::G, true, EHueColor::Y },		// the colour key
		{ EHueColor::G, EHueColor::V, true, EHueColor::B },
		{ EHueColor::A, EHueColor::V, true, EHueColor::M },
		{ EHueColor::Y, EHueColor::V, true, EHueColor::N },
		{ EHueColor::G, EHueColor::M, true, EHueColor::N },
		{ EHueColor::A, EHueColor::A, false, EHueColor::N },
		{ EHueColor::N, EHueColor::A, false, EHueColor::N },
		{ EHueColor::A, EHueColor::Y, false, EHueColor::N },	// both hold red
		{ EHueColor::B, EHueColor::G, false, EHueColor::N },	// both hold green
	};
	for (const FMixCase& Case : MixCases)
	{
		EHueColor Mixed = EHueColor::N;
		const bool bMixes = HueColor::Mix(Case.First, Case.Second, Mixed);
		TestEqual(FString::Printf(TEXT("%s + %s mixes"), ColorName(Case.First), ColorName(Case.Second)), bMixes, Case.bMixes);
		if (Case.bMixes && bMixes)
		{
			TestColor(*this, FString::Printf(TEXT("%s + %s"), ColorName(Case.First), ColorName(Case.Second)), Mixed, Case.Expected);
		}
	}

	// Only the motion signals drive the dancer; the rest are keys.
	TestTrue(TEXT("A drives the rig"), HueColor::DrivesRig(EHueColor::A));
	TestTrue(TEXT("B drives the rig"), HueColor::DrivesRig(EHueColor::B));
	TestTrue(TEXT("N drives the rig"), HueColor::DrivesRig(EHueColor::N));
	TestFalse(TEXT("G is a key"), HueColor::DrivesRig(EHueColor::G));
	TestFalse(TEXT("Y is a key"), HueColor::DrivesRig(EHueColor::Y));

	// A letter always accompanies a colour (accessibility); letters and display colours are pairwise distinct.
	TestEqual(TEXT("Label(A)"), HueColor::Label(EHueColor::A), TEXT("A"));
	TestEqual(TEXT("Label(B)"), HueColor::Label(EHueColor::B), TEXT("B"));
	TestEqual(TEXT("Label(N)"), HueColor::Label(EHueColor::N), TEXT("N"));
	TestEqual(TEXT("Label(Y)"), HueColor::Label(EHueColor::Y), TEXT("Y"));
	for (int32 I = 0; I < UE_ARRAY_COUNT(AllColors); ++I)
	{
		for (int32 J = I + 1; J < UE_ARRAY_COUNT(AllColors); ++J)
		{
			const EHueColor First = AllColors[I];
			const EHueColor Second = AllColors[J];
			TestFalse(FString::Printf(TEXT("%s and %s labels differ"), ColorName(First), ColorName(Second)),
				FCString::Strcmp(HueColor::Label(First), HueColor::Label(Second)) == 0);
			TestFalse(FString::Printf(TEXT("%s and %s display colours differ"), ColorName(First), ColorName(Second)),
				HueColor::ToLinear(First).Equals(HueColor::ToLinear(Second), 0.1f));
		}
	}

	// Logical time is integer milliseconds, so simultaneous arrivals compare exactly.
	TestEqual(TEXT("SecondsToMs(0.25)"), HueTime::SecondsToMs(0.25), static_cast<int64>(250));
	TestEqual(TEXT("SecondsToMs(1.75)"), HueTime::SecondsToMs(1.75), static_cast<int64>(1750));
	TestEqual(TEXT("SecondsToMs(0.1) rounds to the nearest ms"), HueTime::SecondsToMs(0.1), static_cast<int64>(100));
	TestEqual(TEXT("MsToSeconds(1250)"), HueTime::MsToSeconds(1250), 1.25);
	return true;
}

// =============================================================================================================
// 2. Inventory (pure data: NewObject, no world)
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxInventoryRulesTest, "Huebox.Inventory.Rules", HUEBOX_TEST_FLAGS)
bool FHueboxInventoryRulesTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// No actor owns this component, so a local TStrongObjectPtr roots it for the test's scope (a bare pointer would
	// be unprotected if anything triggered a GC) and unroots it on every return path.
	TStrongObjectPtr<UHueInventoryComponent> InventoryRef(NewObject<UHueInventoryComponent>(GetTransientPackage(), NAME_None, RF_Transient));
	UHueInventoryComponent* Inventory = InventoryRef.Get();
	UTEST_NOT_NULL(TEXT("Inventory component"), Inventory);

	const int32 NumSlots = Inventory->GetNumSlots();
	const int32 MaxStack = Inventory->GetMaxStack();
	UTEST_TRUE(TEXT("Scenario needs >= 3 slots and stacks of >= 4"), NumSlots >= 3 && MaxStack >= 4);
	TestEqual(TEXT("Slots are sized to NumSlots"), Inventory->GetSlots().Num(), NumSlots);
	TestTrue(TEXT("Nothing selected in an empty inventory"), Inventory->GetSelectedType() == EHueModuleType::None);

	// Invalid requests add nothing.
	TestEqual(TEXT("AddItem(None) adds nothing"), Inventory->AddItem(EHueModuleType::None, 3), 0);
	TestEqual(TEXT("AddItem(count 0) adds nothing"), Inventory->AddItem(EHueModuleType::Turn, 0), 0);
	TestEqual(TEXT("AddItem(negative count) adds nothing"), Inventory->AddItem(EHueModuleType::Turn, -2), 0);
	TestEqual(TEXT("Still empty"), Inventory->CountOf(EHueModuleType::Turn), 0);

	// Stacking: a type tops up its own stack before it opens a new slot.
	TestEqual(TEXT("Add 2 Turn"), Inventory->AddItem(EHueModuleType::Turn, 2), 2);
	TestEqual(TEXT("Add 1 Colour shift"), Inventory->AddItem(EHueModuleType::ColorShift, 1), 1);
	TestSlot(*this, Inventory, 0, EHueModuleType::Turn, 2, TEXT("First stack"));
	TestSlot(*this, Inventory, 1, EHueModuleType::ColorShift, 1, TEXT("Second type takes the next slot"));
	TestEqual(TEXT("Add MaxStack - 1 Turn"), Inventory->AddItem(EHueModuleType::Turn, MaxStack - 1), MaxStack - 1);
	TestSlot(*this, Inventory, 0, EHueModuleType::Turn, MaxStack, TEXT("Existing stack filled to MaxStack"));
	TestSlot(*this, Inventory, 1, EHueModuleType::ColorShift, 1, TEXT("Other stacks untouched"));
	TestSlot(*this, Inventory, 2, EHueModuleType::Turn, 1, TEXT("Overflow opened the first empty slot"));
	TestEqual(TEXT("CountOf sums all stacks"), Inventory->CountOf(EHueModuleType::Turn), MaxStack + 1);

	// Overflow: capacity is bounded by MaxStack x slots; CanAdd never mutates.
	const int32 TurnRoom = (MaxStack - 1) + (NumSlots - 3) * MaxStack;
	TestTrue(TEXT("CanAdd exactly the remaining room"), Inventory->CanAdd(EHueModuleType::Turn, TurnRoom));
	TestFalse(TEXT("CanAdd one more than the room"), Inventory->CanAdd(EHueModuleType::Turn, TurnRoom + 1));
	TestEqual(TEXT("CanAdd changed nothing"), Inventory->CountOf(EHueModuleType::Turn), MaxStack + 1);
	TestEqual(TEXT("AddItem beyond capacity adds only what fits"), Inventory->AddItem(EHueModuleType::Turn, TurnRoom + 5), TurnRoom);
	TestEqual(TEXT("Turn total after overflow"), Inventory->CountOf(EHueModuleType::Turn), MaxStack + 1 + TurnRoom);
	TestFalse(TEXT("No room for a new type"), Inventory->CanAdd(EHueModuleType::Delay, 1));
	TestEqual(TEXT("New type rejected when every slot is taken"), Inventory->AddItem(EHueModuleType::Delay, 1), 0);
	TestEqual(TEXT("Colour shift only tops up its own stack"), Inventory->AddItem(EHueModuleType::ColorShift, MaxStack), MaxStack - 1);
	TestSlot(*this, Inventory, 1, EHueModuleType::ColorShift, MaxStack, TEXT("Colour shift stack full"));

	// RemoveFromSlot is all-or-nothing; an emptied slot resets to None and accepts another type.
	TestFalse(TEXT("RemoveFromSlot more than the stack fails"), Inventory->RemoveFromSlot(1, MaxStack + 1));
	TestSlot(*this, Inventory, 1, EHueModuleType::ColorShift, MaxStack, TEXT("Failed removal changed nothing"));
	TestTrue(TEXT("RemoveFromSlot the whole stack"), Inventory->RemoveFromSlot(1, MaxStack));
	TestSlot(*this, Inventory, 1, EHueModuleType::None, 0, TEXT("Emptied slot resets to None"));
	TestFalse(TEXT("RemoveFromSlot on an empty slot fails"), Inventory->RemoveFromSlot(1, 1));
	TestFalse(TEXT("RemoveFromSlot with a negative index fails"), Inventory->RemoveFromSlot(-1, 1));
	TestFalse(TEXT("RemoveFromSlot past the last slot fails"), Inventory->RemoveFromSlot(NumSlots, 1));
	TestEqual(TEXT("Freed slot accepts a new type"), Inventory->AddItem(EHueModuleType::Delay, 2), 2);
	TestSlot(*this, Inventory, 1, EHueModuleType::Delay, 2, TEXT("Delay stack in the freed slot"));

	// RemoveItem spans stacks and is all-or-nothing too.
	const int32 TurnTotal = Inventory->CountOf(EHueModuleType::Turn);
	TestFalse(TEXT("RemoveItem more than owned fails"), Inventory->RemoveItem(EHueModuleType::Turn, TurnTotal + 1));
	TestEqual(TEXT("Failed RemoveItem changed nothing"), Inventory->CountOf(EHueModuleType::Turn), TurnTotal);
	TestTrue(TEXT("RemoveItem across two stacks"), Inventory->RemoveItem(EHueModuleType::Turn, MaxStack + 1));
	TestEqual(TEXT("Turn total after RemoveItem"), Inventory->CountOf(EHueModuleType::Turn), TurnTotal - MaxStack - 1);
	TestFalse(TEXT("RemoveItem of a type not owned fails"), Inventory->RemoveItem(EHueModuleType::Mixer, 1));

	// ClearAll empties every slot.
	Inventory->ClearAll();
	for (int32 Index = 0; Index < NumSlots; ++Index)
	{
		TestSlot(*this, Inventory, Index, EHueModuleType::None, 0, TEXT("After ClearAll"));
	}

	// Selection: SelectSlot clamps, CycleSelection (mouse wheel) wraps both ways.
	Inventory->AddItem(EHueModuleType::Turn, 1);
	Inventory->AddItem(EHueModuleType::Mixer, 1);
	Inventory->SelectSlot(1);
	TestEqual(TEXT("SelectSlot(1)"), Inventory->GetSelectedIndex(), 1);
	TestTrue(TEXT("Selected type follows the slot"), Inventory->GetSelectedType() == EHueModuleType::Mixer);
	Inventory->CycleSelection(1);
	TestEqual(TEXT("Cycle +1"), Inventory->GetSelectedIndex(), 2);
	TestTrue(TEXT("Empty slot selects None"), Inventory->GetSelectedType() == EHueModuleType::None);
	Inventory->SelectSlot(0);
	Inventory->CycleSelection(-1);
	TestEqual(TEXT("Cycle -1 from slot 0 wraps to the last slot"), Inventory->GetSelectedIndex(), NumSlots - 1);
	Inventory->CycleSelection(1);
	TestEqual(TEXT("Cycle +1 from the last slot wraps to slot 0"), Inventory->GetSelectedIndex(), 0);
	TestTrue(TEXT("Slot 0 holds Turn"), Inventory->GetSelectedType() == EHueModuleType::Turn);
	for (int32 Step = 0; Step < NumSlots; ++Step)
	{
		Inventory->CycleSelection(1);
	}
	TestEqual(TEXT("A full wheel cycle returns to the start"), Inventory->GetSelectedIndex(), 0);
	Inventory->SelectSlot(NumSlots + 3);
	TestEqual(TEXT("SelectSlot clamps high"), Inventory->GetSelectedIndex(), NumSlots - 1);
	Inventory->SelectSlot(-5);
	TestEqual(TEXT("SelectSlot clamps low"), Inventory->GetSelectedIndex(), 0);
	return true;
}

// =============================================================================================================
// 3-11. Pulse simulation (logic only, exact logical times)
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimStraightRouteTest, "Huebox.Simulation.StraightRoute", HUEBOX_TEST_FLAGS)
bool FHueboxSimStraightRouteTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem of the test world"), Sim);
	Sim->SetVisualsEnabled(false);

	// S(0,2) fires B East; R(4,2) is four cells away = 4 x 250 ms.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	AHueSourceBlock* Source = AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B);
	AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(4, 2), { EHueDir::West });
	UTEST_TRUE(TEXT("Route built"), Source != nullptr && Receiver != nullptr);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	TestEqual(TEXT("One pulse in flight at t = 0"), Sim->GetTravelingPulseCount(), 1);

	FSimClock Clock(Sim);
	Clock.AdvanceTo(0.75);
	TestColor(*this, TEXT("Receiver still N at 0.75 s (pulse one cell short)"), Receiver->GetCurrentColor(), EHueColor::N);
	TestEqual(TEXT("Nothing applied before 1.00 s"), Receiver->GetAppliedHistory().Num(), 0);
	TestEqual(TEXT("Pulse still travelling at 0.75 s"), Sim->GetTravelingPulseCount(), 1);

	Clock.AdvanceTo(1.0);
	TestColor(*this, TEXT("Receiver B at 1.00 s"), Receiver->GetCurrentColor(), EHueColor::B);
	ExpectHistory(*this, TEXT("Straight route"), Receiver->GetAppliedHistory(), { MakeEvent(1.0f, EHueColor::B) });
	TestEqual(TEXT("Logical clock at exactly 1000 ms"), Sim->GetSimTimeMs(), static_cast<int64>(1000));
	TestEqual(TEXT("Receiver consumed the pulse"), Sim->GetTravelingPulseCount(), 0);
	TestEqual(TEXT("No conflict"), Receiver->GetConflictCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimCrossingPulsesTest, "Huebox.Simulation.CrossingPulses", HUEBOX_TEST_FLAGS)
bool FHueboxSimCrossingPulsesTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// B travels East along row 2, A travels North along column 2: both occupy empty cell (2,2) at 0.50 s.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	const bool bSources = AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr
		&& AddSource(*this, Board, FIntPoint(2, 0), EHueDir::North, EHueColor::A) != nullptr;
	AHueReceiverBlock* EastReceiver = AddReceiver(*this, Board, FIntPoint(5, 2), { EHueDir::West });
	AHueReceiverBlock* NorthReceiver = AddReceiver(*this, Board, FIntPoint(2, 5), { EHueDir::South });
	UTEST_TRUE(TEXT("Crossing built"), bSources && EastReceiver != nullptr && NorthReceiver != nullptr);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	FSimClock Clock(Sim);
	Clock.AdvanceTo(0.5);
	TestEqual(TEXT("Both pulses alive in the shared cell"), Sim->GetTravelingPulseCount(), 2);
	Clock.AdvanceTo(1.25);
	ExpectHistory(*this, TEXT("East-bound pulse after crossing"), EastReceiver->GetAppliedHistory(), { MakeEvent(1.25f, EHueColor::B) });
	ExpectHistory(*this, TEXT("North-bound pulse after crossing"), NorthReceiver->GetAppliedHistory(), { MakeEvent(1.25f, EHueColor::A) });
	TestEqual(TEXT("No warnings: crossing pulses never interact"), CountFeedbackOfKind(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Warning), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimColorShiftTest, "Huebox.Simulation.ColorShift", HUEBOX_TEST_FLAGS)
bool FHueboxSimColorShiftTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// S(0,2) East -> C(2,2) -> R(4,2): the receiver sees the shifted colour at 1.00 s (direction preserved).
	struct FShiftCase
	{
		const TCHAR* Name;
		EHueColor SourceColor;
		EHueDir ShiftFacing;
		bool bPasses;
		EHueColor Expected;
	};
	const FShiftCase Cases[] =
	{
		{ TEXT("B through a colour shift"), EHueColor::B, EHueDir::East, true, EHueColor::A },
		{ TEXT("N through a colour shift"), EHueColor::N, EHueDir::East, true, EHueColor::N },
		{ TEXT("Colour shift facing against the pulse"), EHueColor::B, EHueDir::West, false, EHueColor::N },
	};

	int32 BoardIndex = 0;
	for (const FShiftCase& Case : Cases)
	{
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(BoardIndex++), FIntPoint(6, 6));
		AHueSourceBlock* Source = Board ? AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, Case.SourceColor) : nullptr;
		AHueColorShiftBlock* Shift = Board ? AddBlock<AHueColorShiftBlock>(*this, Board, FIntPoint(2, 2), Case.ShiftFacing) : nullptr;
		AHueReceiverBlock* Receiver = Board ? AddReceiver(*this, Board, FIntPoint(4, 2), { EHueDir::West }) : nullptr;
		if (!TestTrue(FString::Printf(TEXT("%s: scenario built"), Case.Name), Source != nullptr && Shift != nullptr && Receiver != nullptr)
			|| !TestTrue(FString::Printf(TEXT("%s: run started"), Case.Name), StartBoardRun(Sim, Board)))
		{
			continue;
		}

		FSimClock Clock(Sim);
		Clock.AdvanceTo(1.5);
		if (Case.bPasses)
		{
			ExpectHistory(*this, Case.Name, Receiver->GetAppliedHistory(), { MakeEvent(1.0f, Case.Expected) });
		}
		else
		{
			// One-way module: entry against Facing is blocked with feedback at the block's cell.
			ExpectHistory(*this, Case.Name, Receiver->GetAppliedHistory(), {});
			TestNotNull(FString::Printf(TEXT("%s: blocked-entry warning at (2,2). Log: %s"), Case.Name, *DescribeFeedback(Sim->GetRunFeedbackLog())),
				FindFeedbackAt(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Warning, FIntPoint(2, 2)));
			TestEqual(FString::Printf(TEXT("%s: pulse consumed"), Case.Name), Sim->GetTravelingPulseCount(), 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimTurnTest, "Huebox.Simulation.TurnPorts", HUEBOX_TEST_FLAGS)
bool FHueboxSimTurnTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Pure port rule over all 16 (facing, travel) pairs: ports Facing + RotatePlus90(Facing); entering one leaves
	// through the other, travelling in that port's face direction.
	{
		AHueBoard* RuleBoard = TestWorld.SpawnBoard(BoardOrigin(0), FIntPoint(6, 6));
		AHueTurnBlock* Probe = RuleBoard ? AddBlock<AHueTurnBlock>(*this, RuleBoard, FIntPoint(2, 2), EHueDir::East) : nullptr;
		UTEST_NOT_NULL(TEXT("Turn probe"), Probe);
		for (const EHueDir Facing : AllDirs)
		{
			Probe->SetFacing(Facing);
			const EHueDir PortA = Facing;
			const EHueDir PortB = HueDir::RotatePlus90(Facing);
			for (const EHueDir Travel : AllDirs)
			{
				const EHueDir EntryFace = HueDir::Opposite(Travel);
				const bool bShouldEnter = EntryFace == PortA || EntryFace == PortB;
				EHueDir ExitDir = EHueDir::East;
				const bool bEntered = Probe->TryTurn(Travel, ExitDir);
				const FString Context = FString::Printf(TEXT("Turn facing %s, pulse travelling %s"), DirName(Facing), DirName(Travel));
				TestTrue(Context + (bShouldEnter ? TEXT(": enters") : TEXT(": is blocked")), bEntered == bShouldEnter);
				if (bEntered && bShouldEnter)
				{
					TestDir(*this, Context + TEXT(": exit"), ExitDir, EntryFace == PortA ? PortB : PortA);
				}
			}
		}

		// Q / E rotation.
		Probe->SetFacing(EHueDir::East);
		Probe->RotateBy(1);
		TestDir(*this, TEXT("RotateBy(+1)"), Probe->GetFacing(), EHueDir::North);
		Probe->RotateBy(-2);
		TestDir(*this, TEXT("RotateBy(-2)"), Probe->GetFacing(), EHueDir::South);
		Probe->RotateBy(5);
		TestDir(*this, TEXT("RotateBy(+5)"), Probe->GetFacing(), EHueDir::East);
	}

	// Routed: T(2,2) facing North has ports North + West.
	{
		// Entry through the West port (travelling East) leaves North: S(0,2) -> T(2,2) @0.50 -> R(2,4) @1.00.
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(1), FIntPoint(6, 6));
		UTEST_NOT_NULL(TEXT("West-port board"), Board);
		const bool bBuilt = AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr
			&& AddBlock<AHueTurnBlock>(*this, Board, FIntPoint(2, 2), EHueDir::North) != nullptr;
		AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(2, 4), { EHueDir::South });
		UTEST_TRUE(TEXT("West-port scenario built"), bBuilt && Receiver != nullptr);
		UTEST_TRUE(TEXT("West-port run started"), StartBoardRun(Sim, Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(1.25);
		ExpectHistory(*this, TEXT("Entry through the West port exits North"), Receiver->GetAppliedHistory(), { MakeEvent(1.0f, EHueColor::B) });
	}
	{
		// Entry through the North port (travelling South) leaves West: S(2,5) -> T(2,2) @0.75 -> R(0,2) @1.25.
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(2), FIntPoint(6, 6));
		UTEST_NOT_NULL(TEXT("North-port board"), Board);
		const bool bBuilt = AddSource(*this, Board, FIntPoint(2, 5), EHueDir::South, EHueColor::B) != nullptr
			&& AddBlock<AHueTurnBlock>(*this, Board, FIntPoint(2, 2), EHueDir::North) != nullptr;
		AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(0, 2), { EHueDir::East });
		UTEST_TRUE(TEXT("North-port scenario built"), bBuilt && Receiver != nullptr);
		UTEST_TRUE(TEXT("North-port run started"), StartBoardRun(Sim, Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(1.5);
		ExpectHistory(*this, TEXT("Entry through the North port exits West"), Receiver->GetAppliedHistory(), { MakeEvent(1.25f, EHueColor::B) });
	}
	{
		// The East face has no port: S(5,2) West -> T(2,2) @0.75 is blocked with "no port" feedback.
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(3), FIntPoint(6, 6));
		UTEST_NOT_NULL(TEXT("Wrong-face board"), Board);
		const bool bBuilt = AddSource(*this, Board, FIntPoint(5, 2), EHueDir::West, EHueColor::B) != nullptr
			&& AddBlock<AHueTurnBlock>(*this, Board, FIntPoint(2, 2), EHueDir::North) != nullptr;
		UTEST_TRUE(TEXT("Wrong-face scenario built"), bBuilt);
		UTEST_TRUE(TEXT("Wrong-face run started"), StartBoardRun(Sim, Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		ExpectFeedback(*this, TEXT("Not blocked before the pulse arrives"), Sim->GetRunFeedbackLog(), TEXT("no port"), false);
		Clock.AdvanceTo(0.75);
		ExpectFeedback(*this, TEXT("Wrong face"), Sim->GetRunFeedbackLog(), TEXT("no port"), true);
		if (const FHueFeedback* Blocked = FindFeedback(Sim->GetRunFeedbackLog(), TEXT("no port")))
		{
			TestTrue(TEXT("Blocked entry is a warning"), Blocked->Kind == EHueFeedbackKind::Warning);
			TestEqual(TEXT("Blocked entry names the turn's cell"), Blocked->Cell, FIntPoint(2, 2));
		}
		TestEqual(TEXT("Blocked pulse consumed"), Sim->GetTravelingPulseCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimDelayTest, "Huebox.Simulation.Delay", HUEBOX_TEST_FLAGS)
bool FHueboxSimDelayTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// S(0,2) East -> D(2,2) 1.0 s -> R(5,2): stored at 0.50, released at 1.50, reaches R at 2.25.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	const bool bSource = AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr;
	AHueDelayBlock* Delay = AddDelay(*this, Board, FIntPoint(2, 2), EHueDir::East, 1.0f);
	AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(5, 2), { EHueDir::West });
	UTEST_TRUE(TEXT("Delay route built"), bSource && Delay != nullptr && Receiver != nullptr);
	TestEqual(TEXT("Delay configured"), Delay->GetDelaySeconds(), 1.0f);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	FSimClock Clock(Sim);
	Clock.AdvanceTo(0.25);
	TestFalse(TEXT("Not holding before the pulse arrives"), Delay->IsHolding());
	Clock.AdvanceTo(0.5);
	TestTrue(TEXT("Holding from 0.50 s"), Delay->IsHolding());
	TestEqual(TEXT("Stored pulse is not travelling"), Sim->GetTravelingPulseCount(), 0);
	TestEqual(TEXT("Stored pulse counts toward the signal cap"), Sim->GetActiveSignalCount(), 1);

	// Every route into D shares its single input path, so a second pulse cannot be routed in while it holds;
	// hand one over exactly as the subsystem would on arrival.
	FPulseData Second = MakeInjectedPulse(Sim, Delay, EHueColor::A, Delay->GetFacing());
	TestTrue(TEXT("Second pulse while holding is consumed"), Delay->ReceivePulse(Second, *Sim) == EPulseResponse::Consumed);
	ExpectFeedback(*this, TEXT("Occupied delay"), Sim->GetRunFeedbackLog(), TEXT("occupied"), true);
	TestTrue(TEXT("Still holding the first pulse"), Delay->IsHolding());
	TestEqual(TEXT("Rejected pulse adds no signal"), Sim->GetActiveSignalCount(), 1);

	Clock.AdvanceTo(1.25);
	TestTrue(TEXT("Still holding at 1.25 s"), Delay->IsHolding());
	Clock.AdvanceTo(1.5);
	TestFalse(TEXT("Released at 1.50 s"), Delay->IsHolding());
	TestEqual(TEXT("Released pulse travels on"), Sim->GetTravelingPulseCount(), 1);
	Clock.AdvanceTo(2.0);
	TestColor(*this, TEXT("Receiver still N at 2.00 s"), Receiver->GetCurrentColor(), EHueColor::N);
	Clock.AdvanceTo(2.25);
	// Same colour as stored (B); the rejected A never got through.
	ExpectHistory(*this, TEXT("Delayed pulse"), Receiver->GetAppliedHistory(), { MakeEvent(2.25f, EHueColor::B) });

	// Tuning rules: 0.25 s steps, clamped to [0, 6] (Z / X keys).
	Delay->SetDelaySeconds(2.5f);
	TestEqual(TEXT("SetDelaySeconds(2.5)"), Delay->GetDelaySeconds(), 2.5f);
	Delay->SetDelaySeconds(1.1f);
	TestEqual(TEXT("SetDelaySeconds snaps to 0.25 s steps"), Delay->GetDelaySeconds(), 1.0f);
	Delay->AdjustDelay(1);
	TestEqual(TEXT("AdjustDelay(+1)"), Delay->GetDelaySeconds(), 1.25f);
	Delay->AdjustDelay(-2);
	TestEqual(TEXT("AdjustDelay(-2)"), Delay->GetDelaySeconds(), 0.75f);
	Delay->SetDelaySeconds(7.f);
	TestEqual(TEXT("SetDelaySeconds clamps to 6 s"), Delay->GetDelaySeconds(), AHueDelayBlock::MaxDelay);
	Delay->AdjustDelay(1);
	TestEqual(TEXT("AdjustDelay stops at 6 s"), Delay->GetDelaySeconds(), AHueDelayBlock::MaxDelay);
	Delay->SetDelaySeconds(-1.f);
	TestEqual(TEXT("SetDelaySeconds clamps to 0 s"), Delay->GetDelaySeconds(), AHueDelayBlock::MinDelay);
	Delay->AdjustDelay(-1);
	TestEqual(TEXT("AdjustDelay stops at 0 s"), Delay->GetDelaySeconds(), AHueDelayBlock::MinDelay);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimMixerTest, "Huebox.Simulation.Mixer", HUEBOX_TEST_FLAGS)
bool FHueboxSimMixerTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Port rule: output = Facing, input 0 = RotatePlus90(Facing), input 1 = RotateMinus90(Facing).
	{
		AHueBoard* RuleBoard = TestWorld.SpawnBoard(BoardOrigin(0), FIntPoint(6, 6));
		AHueMixerBlock* Probe = RuleBoard ? AddBlock<AHueMixerBlock>(*this, RuleBoard, FIntPoint(2, 2), EHueDir::North) : nullptr;
		UTEST_NOT_NULL(TEXT("Mixer probe"), Probe);
		for (const EHueDir Facing : AllDirs)
		{
			Probe->SetFacing(Facing);
			for (const EHueDir Travel : AllDirs)
			{
				const EHueDir EntryFace = HueDir::Opposite(Travel);
				const int32 Expected = EntryFace == HueDir::RotatePlus90(Facing) ? 0
					: (EntryFace == HueDir::RotateMinus90(Facing) ? 1 : INDEX_NONE);
				TestEqual(FString::Printf(TEXT("Mixer facing %s, pulse travelling %s: input index"), DirName(Facing), DirName(Travel)),
					Probe->GetInputIndexForTravel(Travel), Expected);
			}
		}
	}

	// A and B at the same instant (0.50 s) -> N leaves at 0.50 and reaches R at 1.00.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("Same-time scenario built"), BuildMixerScenario(*this, TestWorld, 1, 2, EHueColor::A, 6, EHueColor::B, Case));
		UTEST_TRUE(TEXT("Same-time run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestFalse(TEXT("Same time: both inputs consumed"), Case.Mixer->IsWaiting());
		TestTrue(TEXT("Same time: no mixer failure"), Case.Mixer->GetLastFailure().IsEmpty());
		Clock.AdvanceTo(1.0);
		ExpectHistory(*this, TEXT("A + B at the same time"), Case.Receiver->GetAppliedHistory(),
			{ MakeEvent(0.75f, EHueColor::B), MakeEvent(1.0f, EHueColor::N) });
	}

	// dt exactly 0.5 s (A 0.25, B 0.75): the window is inclusive - the arrival runs before the timeout event.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("Window-edge scenario built"), BuildMixerScenario(*this, TestWorld, 2, 3, EHueColor::A, 7, EHueColor::B, Case));
		UTEST_TRUE(TEXT("Window-edge run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestTrue(TEXT("dt 0.5: A waits at input 0"), Case.Mixer->IsWaiting());
		Clock.AdvanceTo(0.75);
		TestFalse(TEXT("dt 0.5: mixed at the window edge"), Case.Mixer->IsWaiting());
		ExpectFeedback(*this, TEXT("dt 0.5 s is inside the inclusive window"), Sim->GetRunFeedbackLog(), TEXT("timeout"), false);
		Clock.AdvanceTo(1.25);
		ExpectHistory(*this, TEXT("A + B, dt 0.5 s"), Case.Receiver->GetAppliedHistory(),
			{ MakeEvent(0.75f, EHueColor::B), MakeEvent(1.25f, EHueColor::N) });
	}

	// dt 0.75 s (A 0.25, B 1.00): A times out at 0.75 s, the late B starts a new wait that also times out.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("Timeout scenario built"), BuildMixerScenario(*this, TestWorld, 3, 3, EHueColor::A, 8, EHueColor::B, Case));
		UTEST_TRUE(TEXT("Timeout run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestTrue(TEXT("dt 0.75: A waits at input 0"), Case.Mixer->IsWaiting());

		// A second pulse on the already-waiting input is rejected and leaves the wait untouched (routing cannot
		// deliver two pulses to one input here, so it is handed over directly). Travelling East = West face = input 0.
		FPulseData Extra = MakeInjectedPulse(Sim, Case.Mixer, EHueColor::B, EHueDir::East);
		TestTrue(TEXT("Pulse on the waiting input is consumed"), Case.Mixer->ReceivePulse(Extra, *Sim) == EPulseResponse::Consumed);
		TestTrue(TEXT("Rejected pulse did not mix or clear the wait"), Case.Mixer->IsWaiting());
		ExpectFeedback(*this, TEXT("No timeout before 0.75 s"), Sim->GetRunFeedbackLog(), TEXT("timeout"), false);

		Clock.AdvanceTo(0.75);
		TestFalse(TEXT("dt 0.75: the wait timed out at 0.75 s"), Case.Mixer->IsWaiting());
		ExpectFeedback(*this, TEXT("dt 0.75 s"), Sim->GetRunFeedbackLog(), TEXT("timeout"), true);
		TestTrue(TEXT("dt 0.75: failure cause is the timeout"), Case.Mixer->GetLastFailure().Contains(TEXT("timeout")));
		Clock.AdvanceTo(1.0);
		TestTrue(TEXT("dt 0.75: the late B starts a new wait"), Case.Mixer->IsWaiting());
		Clock.AdvanceTo(1.5);
		TestFalse(TEXT("dt 0.75: the second wait timed out too"), Case.Mixer->IsWaiting());
		Clock.AdvanceTo(2.5);
		ExpectHistory(*this, TEXT("Timed-out mix emits no N"), Case.Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
	}

	// A + A at the same instant: matching colours fail, both consumed.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("A+A scenario built"), BuildMixerScenario(*this, TestWorld, 4, 2, EHueColor::A, 6, EHueColor::A, Case));
		UTEST_TRUE(TEXT("A+A run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestFalse(TEXT("A+A: both inputs consumed"), Case.Mixer->IsWaiting());
		TestTrue(FString::Printf(TEXT("A+A: failure names the matching colours (\"%s\")"), *Case.Mixer->GetLastFailure()),
			Case.Mixer->GetLastFailure().Contains(TEXT("A+A")));
		Clock.AdvanceTo(1.5);
		ExpectHistory(*this, TEXT("A + A emits no N"), Case.Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
	}

	// N + A: N cannot be mixed.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("N+A scenario built"), BuildMixerScenario(*this, TestWorld, 5, 2, EHueColor::N, 6, EHueColor::A, Case));
		UTEST_TRUE(TEXT("N+A run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestFalse(TEXT("N+A: both inputs consumed"), Case.Mixer->IsWaiting());
		TestFalse(TEXT("N+A: mixing N reports a failure"), Case.Mixer->GetLastFailure().IsEmpty());
		Clock.AdvanceTo(1.5);
		ExpectHistory(*this, TEXT("N + A emits no N"), Case.Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
	}

	// Additive light beyond the stop: A + G -> Y at 0.50 s. Y is a key, not a motion signal: the receiver absorbs it
	// at 1.00 s (warning) and keeps B.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("A+G scenario built"), BuildMixerScenario(*this, TestWorld, 6, 2, EHueColor::A, 6, EHueColor::G, Case));
		UTEST_TRUE(TEXT("A+G run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestFalse(TEXT("A+G: both inputs consumed"), Case.Mixer->IsWaiting());
		TestTrue(FString::Printf(TEXT("A+G: no mixer failure (\"%s\")"), *Case.Mixer->GetLastFailure()), Case.Mixer->GetLastFailure().IsEmpty());
		ExpectFeedback(*this, TEXT("A + G mixes to Y"), Sim->GetRunFeedbackLog(), TEXT("A + G -> Y"), true);
		Clock.AdvanceTo(1.5);
		ExpectHistory(*this, TEXT("A Y pulse never moves the dancer"), Case.Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
		TestColor(*this, TEXT("The receiver keeps B"), Case.Receiver->GetCurrentColor(), EHueColor::B);
		TestEqual(TEXT("An absorbed key colour is not a conflict"), Case.Receiver->GetConflictCount(), 0);
		ExpectFeedback(*this, TEXT("Y at the receiver"), Sim->GetRunFeedbackLog(), TEXT("only reacts to A, B or N"), true);
	}

	// A + Y both hold red: they cannot add up.
	{
		FMixerScenario Case;
		UTEST_TRUE(TEXT("A+Y scenario built"), BuildMixerScenario(*this, TestWorld, 7, 2, EHueColor::A, 6, EHueColor::Y, Case));
		UTEST_TRUE(TEXT("A+Y run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
		TestFalse(TEXT("A+Y: both inputs consumed"), Case.Mixer->IsWaiting());
		TestTrue(FString::Printf(TEXT("A+Y: failure names the shared primary (\"%s\")"), *Case.Mixer->GetLastFailure()),
			Case.Mixer->GetLastFailure().Contains(TEXT("share red")));
		Clock.AdvanceTo(1.5);
		ExpectHistory(*this, TEXT("A + Y emits nothing"), Case.Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimColorBrickTest, "Huebox.Simulation.ColorBrick", HUEBOX_TEST_FLAGS)
bool FHueboxSimColorBrickTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Every case: 8x5 board, push A from S(0,3) East through brick K(3,3) (reached at 0.75 s) to R(6,3) (West face,
	// reached at 1.50 s if the brick is open). A key source at (3,KeyY) facing North hits K from the South at
	// (3 - KeyY) * 0.25 s.
	struct FBrickCase
	{
		AHueBoard* Board = nullptr;
		AHueColorBrickBlock* Brick = nullptr;
		AHueReceiverBlock* Receiver = nullptr;
	};
	auto BuildCase = [this, &TestWorld](int32 BoardIndex, int32 KeyY, EHueColor KeyColor, FBrickCase& Out)
	{
		Out = FBrickCase();
		Out.Board = TestWorld.SpawnBoard(BoardOrigin(BoardIndex), FIntPoint(8, 5));
		if (!Out.Board)
		{
			AddError(TEXT("Brick case: board could not be spawned"));
			return false;
		}
		Out.Brick = AddBlock<AHueColorBrickBlock>(*this, Out.Board, FIntPoint(3, 3), EHueDir::East, /*bRemovable*/ false);
		if (Out.Brick)
		{
			Out.Brick->SetBrickColor(EHueColor::Y);
		}
		Out.Receiver = AddReceiver(*this, Out.Board, FIntPoint(6, 3), { EHueDir::West });
		bool bSources = AddSource(*this, Out.Board, FIntPoint(0, 3), EHueDir::East, EHueColor::A) != nullptr;
		if (KeyY >= 0)
		{
			bSources &= AddSource(*this, Out.Board, FIntPoint(3, KeyY), EHueDir::North, KeyColor) != nullptr;
		}
		return Out.Brick && Out.Receiver && bSources;
	};

	// No key: the standing brick blocks the push and says which colour it needs.
	{
		FBrickCase Case;
		UTEST_TRUE(TEXT("No-key case built"), BuildCase(0, INDEX_NONE, EHueColor::Y, Case));
		UTEST_TRUE(TEXT("No-key run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(2.0);
		TestFalse(TEXT("No key: the brick still stands"), Case.Brick->IsBroken());
		ExpectHistory(*this, TEXT("No key: nothing reaches the receiver"), Case.Receiver->GetAppliedHistory(), {});
		TestNotNull(FString::Printf(TEXT("No key: blocked warning at the brick. Log: %s"), *DescribeFeedback(Sim->GetRunFeedbackLog())),
			FindFeedbackAt(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Warning, FIntPoint(3, 3)));
		ExpectFeedback(*this, TEXT("No key: the warning names the key"), Sim->GetRunFeedbackLog(), TEXT("only Y (yellow) light breaks it"), true);
	}

	// The wrong key (G) is blocked like any other colour.
	{
		FBrickCase Case;
		UTEST_TRUE(TEXT("Wrong-key case built"), BuildCase(1, 2, EHueColor::G, Case));
		UTEST_TRUE(TEXT("Wrong-key run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(2.0);
		TestFalse(TEXT("Wrong key: the brick still stands"), Case.Brick->IsBroken());
		ExpectHistory(*this, TEXT("Wrong key: nothing reaches the receiver"), Case.Receiver->GetAppliedHistory(), {});
	}

	// Y first (0.25 s), push later (0.75 s): the key is used up, the brick opens, the push passes unchanged.
	{
		FBrickCase Case;
		UTEST_TRUE(TEXT("Key-first case built"), BuildCase(2, 2, EHueColor::Y, Case));
		UTEST_TRUE(TEXT("Key-first run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.25);
		TestTrue(TEXT("Key first: broken at the end of the 0.25 s instant"), Case.Brick->IsBroken());
		TestEqual(TEXT("Key first: broken at 250 ms"), Case.Brick->GetBrokenAtMs(), static_cast<int64>(250));
		Clock.AdvanceTo(2.0);
		ExpectHistory(*this, TEXT("Key first: the push reaches the receiver"), Case.Receiver->GetAppliedHistory(), { MakeEvent(1.5f, EHueColor::A) });
		ExpectFeedback(*this, TEXT("Key first: the break is reported"), Sim->GetRunFeedbackLog(), TEXT("broken by Y"), true);
		TestEqual(FString::Printf(TEXT("Key first: no blocked pulses. Log: %s"), *DescribeFeedback(Sim->GetRunFeedbackLog())),
			CountFeedbackOfKind(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Warning), 0);

		// Bricks are run state: a reset rebuilds it.
		Sim->StopAndClear();
		Case.Board->ResetAllBlocks();
		TestFalse(TEXT("Reset restores the brick"), Case.Brick->IsBroken());
		TestEqual(TEXT("Reset clears the break time"), Case.Brick->GetBrokenAtMs(), static_cast<int64>(-1));
	}

	// Key and push in the same instant (both 0.75 s): the brick opens only when the instant ends, so the push is
	// blocked regardless of queue order.
	{
		FBrickCase Case;
		UTEST_TRUE(TEXT("Same-instant case built"), BuildCase(3, 0, EHueColor::Y, Case));
		UTEST_TRUE(TEXT("Same-instant run started"), StartBoardRun(Sim, Case.Board));
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.75);
		TestTrue(TEXT("Same instant: the brick broke"), Case.Brick->IsBroken());
		Clock.AdvanceTo(2.0);
		ExpectHistory(*this, TEXT("Same instant: the push was still blocked"), Case.Receiver->GetAppliedHistory(), {});
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimReceiverConflictTest, "Huebox.Simulation.ReceiverConflict", HUEBOX_TEST_FLAGS)
bool FHueboxSimReceiverConflictTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// A from the West and B from the South both enter R(3,3) at 0.75 s -> conflict, colour unchanged (N, so any
	// wrongly applied colour would show). A later single A from the East (1.25 s) still applies normally.
	{
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(0), FIntPoint(9, 6));
		UTEST_NOT_NULL(TEXT("Conflict board"), Board);
		const bool bSources = AddSource(*this, Board, FIntPoint(0, 3), EHueDir::East, EHueColor::A) != nullptr
			&& AddSource(*this, Board, FIntPoint(3, 0), EHueDir::North, EHueColor::B) != nullptr
			&& AddSource(*this, Board, FIntPoint(8, 3), EHueDir::West, EHueColor::A) != nullptr;
		AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(3, 3), { EHueDir::West, EHueDir::South, EHueDir::East });
		UTEST_TRUE(TEXT("Conflict scenario built"), bSources && Receiver != nullptr);
		UTEST_TRUE(TEXT("Conflict run started"), StartBoardRun(Sim, Board));

		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.75);
		TestEqual(TEXT("One conflict"), Receiver->GetConflictCount(), 1);
		TestColor(*this, TEXT("Conflict leaves the colour unchanged"), Receiver->GetCurrentColor(), EHueColor::N);
		ExpectHistory(*this, TEXT("Conflict applies nothing"), Receiver->GetAppliedHistory(), {});
		TestNotNull(FString::Printf(TEXT("Conflict warning at the receiver. Log: %s"), *DescribeFeedback(Sim->GetRunFeedbackLog())),
			FindFeedbackAt(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Warning, FIntPoint(3, 3)));

		Clock.AdvanceTo(1.25);
		ExpectHistory(*this, TEXT("Single arrival after a conflict"), Receiver->GetAppliedHistory(), { MakeEvent(1.25f, EHueColor::A) });
		TestEqual(TEXT("Still one conflict"), Receiver->GetConflictCount(), 1);
	}

	// The same colour through two faces at the same time is not a conflict: applied once.
	{
		AHueBoard* Board = TestWorld.SpawnBoard(BoardOrigin(1), FIntPoint(6, 6));
		UTEST_NOT_NULL(TEXT("Agreement board"), Board);
		const bool bSources = AddSource(*this, Board, FIntPoint(0, 3), EHueDir::East, EHueColor::B) != nullptr
			&& AddSource(*this, Board, FIntPoint(3, 0), EHueDir::North, EHueColor::B) != nullptr;
		AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(3, 3), { EHueDir::West, EHueDir::South });
		UTEST_TRUE(TEXT("Agreement scenario built"), bSources && Receiver != nullptr);
		UTEST_TRUE(TEXT("Agreement run started"), StartBoardRun(Sim, Board));

		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.75);
		TestEqual(TEXT("Matching colours do not conflict"), Receiver->GetConflictCount(), 0);
		ExpectHistory(*this, TEXT("Matching simultaneous arrivals apply once"), Receiver->GetAppliedHistory(), { MakeEvent(0.75f, EHueColor::B) });
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimOpsLimitTest, "Huebox.Simulation.OpsLimit", HUEBOX_TEST_FLAGS)
bool FHueboxSimOpsLimitTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Turn, colour shift and delay are injective (each output state has exactly one predecessor), so a pulse coming
	// from a source can never enter a closed loop of turns - it would need a second way in. The op limit is therefore
	// exercised with a path longer than the budget: a serpentine of colour shifts and corner turns on a 12x6 board,
	// one block per cell, so block k on the path is reached at exactly k * 250 ms.
	constexpr int32 Width = 12;
	constexpr int32 Height = 6;
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(Width, Height));
	UTEST_NOT_NULL(TEXT("Board"), Board);

	bool bBuilt = AddSource(*this, Board, FIntPoint(0, 0), EHueDir::East, EHueColor::B) != nullptr;
	int32 PathBlocks = 0;
	for (int32 Row = 0; Row < Height; ++Row)
	{
		const bool bEastbound = (Row % 2) == 0;
		const EHueDir RowDir = bEastbound ? EHueDir::East : EHueDir::West;
		if (Row > 0)
		{
			// Entry corner: arrives travelling North (South port), leaves along the row.
			// South + East ports = facing South; South + West ports = facing West.
			bBuilt &= AddBlock<AHueTurnBlock>(*this, Board, FIntPoint(bEastbound ? 0 : Width - 1, Row), bEastbound ? EHueDir::South : EHueDir::West) != nullptr;
			++PathBlocks;
		}
		for (int32 Step = 0; Step < Width - 2; ++Step)
		{
			const int32 X = bEastbound ? 1 + Step : Width - 2 - Step;
			bBuilt &= AddBlock<AHueColorShiftBlock>(*this, Board, FIntPoint(X, Row), RowDir) != nullptr;
			++PathBlocks;
		}
		if (Row < Height - 1)
		{
			// Exit corner: arrives along the row, leaves travelling North.
			// West + North ports = facing North; East + North ports = facing East.
			bBuilt &= AddBlock<AHueTurnBlock>(*this, Board, FIntPoint(bEastbound ? Width - 1 : 0, Row), bEastbound ? EHueDir::North : EHueDir::East) != nullptr;
			++PathBlocks;
		}
	}
	UTEST_TRUE(TEXT("Serpentine built"), bBuilt);

	const int32 MaxOps = Board->MaxOpsPerSignal;
	UTEST_TRUE(FString::Printf(TEXT("Path (%d blocks) is longer than the op budget (%d)"), PathBlocks, MaxOps), PathBlocks > MaxOps);
	const FString LimitText = FString::FromInt(MaxOps);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	TestEqual(TEXT("Board budget reaches the simulation"), Sim->GetMaxOpsPerSignal(), MaxOps);

	// Blocks 1..64 each spend one op; arriving at block 65 with none left terminates the signal.
	FSimClock Clock(Sim);
	Clock.AdvanceTo(MaxOps * 0.25);
	TestEqual(TEXT("Pulse alive after its last allowed operation"), Sim->GetTravelingPulseCount(), 1);
	ExpectFeedback(*this, TEXT("No termination within the budget"), Sim->GetRunFeedbackLog(), *LimitText, false);

	Clock.AdvanceTo((MaxOps + 1) * 0.25);
	TestEqual(TEXT("Pulse terminated at the next module"), Sim->GetTravelingPulseCount(), 0);
	TestTrue(TEXT("HasFeedbackContaining finds the limit"), Sim->HasFeedbackContaining(LimitText));
	if (const FHueFeedback* Termination = FindFeedback(Sim->GetRunFeedbackLog(), *LimitText))
	{
		TestTrue(TEXT("Termination is an error"), Termination->Kind == EHueFeedbackKind::Error);
	}
	else
	{
		AddError(FString::Printf(TEXT("Termination feedback should mention %s. Log: %s"), *LimitText, *DescribeFeedback(Sim->GetRunFeedbackLog())));
	}

	Clock.AdvanceTo(20.0);
	ExpectFeedback(*this, TEXT("Terminated pulse never left the board"), Sim->GetRunFeedbackLog(), TEXT("left the board"), false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimSignalCapTest, "Huebox.Simulation.SignalCap", HUEBOX_TEST_FLAGS)
bool FHueboxSimSignalCapTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Three sources firing East on rows 0, 2, 4 of an otherwise empty board.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	const bool bSources = AddSource(*this, Board, FIntPoint(0, 0), EHueDir::East, EHueColor::B) != nullptr
		&& AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr
		&& AddSource(*this, Board, FIntPoint(0, 4), EHueDir::East, EHueColor::B) != nullptr;
	UTEST_TRUE(TEXT("Sources placed"), bSources);

	// Cap 2: the third emission fails with an error line.
	Board->MaxActiveSignals = 2;
	Board->ResetAllBlocks();
	Sim->StartRun(Board);
	TestEqual(TEXT("Only two pulses emitted under a cap of 2"), Sim->GetTravelingPulseCount(), 2);
	TestEqual(TEXT("Active signals at the cap"), Sim->GetActiveSignalCount(), 2);
	TestEqual(FString::Printf(TEXT("One error for the refused emission. Log: %s"), *DescribeFeedback(Sim->GetRunFeedbackLog())),
		CountFeedbackOfKind(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Error), 1);

	// Pulses vanish at the board boundary (6 cells -> 1.50 s).
	{
		FSimClock Clock(Sim);
		Clock.AdvanceTo(2.0);
		TestEqual(TEXT("Pulses removed at the boundary"), Sim->GetTravelingPulseCount(), 0);
		TestEqual(TEXT("Each emitted pulse left the board"), CountFeedback(Sim->GetRunFeedbackLog(), TEXT("left the board")), 2);
	}

	// Default cap (16): all three fire, and the new run's feedback log starts clean.
	Board->MaxActiveSignals = 16;
	UTEST_TRUE(TEXT("Second run started"), StartBoardRun(Sim, Board));
	TestEqual(TEXT("All three pulses emitted under the default cap"), Sim->GetTravelingPulseCount(), 3);
	TestEqual(TEXT("No errors under the default cap"), CountFeedbackOfKind(Sim->GetRunFeedbackLog(), EHueFeedbackKind::Error), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimResetTest, "Huebox.Simulation.Reset", HUEBOX_TEST_FLAGS)
bool FHueboxSimResetTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// At 0.75 s every kind of run-time state exists at once:
	//   row 5: S(0,5) B -> R(2,5)            receiver B since 0.50
	//   row 0: S(0,0) B -> D(1,0) 6 s       delay holding since 0.25 (release 6.25)
	//   row 2: S(2,2) A -> M(4,2) North     mixer waiting since 0.50 (timeout 1.00)
	//   row 4: S(5,4) B travelling West     in flight until 1.50
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	const bool bSources = AddSource(*this, Board, FIntPoint(0, 5), EHueDir::East, EHueColor::B) != nullptr
		&& AddSource(*this, Board, FIntPoint(0, 0), EHueDir::East, EHueColor::B) != nullptr
		&& AddSource(*this, Board, FIntPoint(2, 2), EHueDir::East, EHueColor::A) != nullptr
		&& AddSource(*this, Board, FIntPoint(5, 4), EHueDir::West, EHueColor::B) != nullptr;
	AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(2, 5), { EHueDir::West });
	AHueDelayBlock* Delay = AddDelay(*this, Board, FIntPoint(1, 0), EHueDir::East, 6.0f);
	AHueMixerBlock* Mixer = AddBlock<AHueMixerBlock>(*this, Board, FIntPoint(4, 2), EHueDir::North);
	UTEST_TRUE(TEXT("Reset scenario built"), bSources && Receiver != nullptr && Delay != nullptr && Mixer != nullptr);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	{
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.75);
	}
	TestColor(*this, TEXT("Before reset: receiver B"), Receiver->GetCurrentColor(), EHueColor::B);
	TestTrue(TEXT("Before reset: delay holding"), Delay->IsHolding());
	TestTrue(TEXT("Before reset: mixer waiting"), Mixer->IsWaiting());
	TestEqual(TEXT("Before reset: one pulse in flight"), Sim->GetTravelingPulseCount(), 1);
	TestEqual(TEXT("Before reset: travelling + held signals"), Sim->GetActiveSignalCount(), 3);
	TestTrue(TEXT("Before reset: events pending"), Sim->GetPendingEventCount() > 0);

	// Full reset as AExhibitManager::ResetAttempt does it (R key): pulses, queued events, buffers, colours.
	Sim->StopAndClear();
	Board->ResetAllBlocks();
	TestFalse(TEXT("After reset: not running"), Sim->IsRunning());
	TestEqual(TEXT("After reset: no pulses"), Sim->GetTravelingPulseCount(), 0);
	TestEqual(TEXT("After reset: no pending events"), Sim->GetPendingEventCount(), 0);
	TestEqual(TEXT("After reset: no active signals"), Sim->GetActiveSignalCount(), 0);
	TestFalse(TEXT("After reset: delay not holding"), Delay->IsHolding());
	TestFalse(TEXT("After reset: mixer not waiting"), Mixer->IsWaiting());
	TestColor(*this, TEXT("After reset: receiver N"), Receiver->GetCurrentColor(), EHueColor::N);
	TestEqual(TEXT("After reset: history cleared"), Receiver->GetAppliedHistory().Num(), 0);
	TestEqual(TEXT("After reset: conflicts cleared"), Receiver->GetConflictCount(), 0);

	// Nothing of the old attempt may fire later (the delay's 6.25 s release, the mixer's 1.00 s timeout).
	Sim->StepSimulation(7.0);
	TestFalse(TEXT("Old delay release never fires"), Delay->IsHolding());
	TestColor(*this, TEXT("Receiver stays N"), Receiver->GetCurrentColor(), EHueColor::N);

	// The layout survived: a fresh run behaves exactly like the first one.
	UTEST_TRUE(TEXT("Second run started"), StartBoardRun(Sim, Board));
	{
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.5);
	}
	ExpectHistory(*this, TEXT("Second attempt"), Receiver->GetAppliedHistory(), { MakeEvent(0.5f, EHueColor::B) });
	TestTrue(TEXT("Second attempt: delay holding again"), Delay->IsHolding());
	TestTrue(TEXT("Second attempt: mixer waiting again"), Mixer->IsWaiting());
	return true;
}

// =============================================================================================================
// 12. Ordered objectives (pure evaluation on a board, no rig)
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxObjectivesOrderTest, "Huebox.Objectives.Order", HUEBOX_TEST_FLAGS)
bool FHueboxObjectivesOrderTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);

	const FHueRigSample AtStart = MakeSample(0.06f, 0.f);
	const FHueRigSample InZoneSlow = MakeSample(0.40f, 0.f);
	const FHueRigSample InZoneCreepingBack = MakeSample(0.40f, -0.01f);
	const FHueRigSample InZoneFast = MakeSample(0.40f, 0.05f);
	const FHueRigSample PastZoneSlow = MakeSample(0.70f, 0.f);

	// Exercise 3 objectives: LeaveStart 0.12, then StopInZone [0.30, 0.56], |v| <= 2 % / s, hold 1 s.
	Board->SetObjectives({ MakeObjective(EHueObjectiveType::LeaveStart, 0.12f), MakeStopObjective(0.30f, 0.56f, 0.02f, 1.0f) });
	TestEqual(TEXT("Starts at objective 0"), Board->GetCurrentObjectiveIndex(), 0);
	TestFalse(TEXT("Not complete at the start"), Board->AreObjectivesComplete());

	TestFalse(TEXT("At rest at the start nothing completes"), Board->EvaluateObjectives(AtStart, 2.f));
	TestEqual(TEXT("Still on objective 0"), Board->GetCurrentObjectiveIndex(), 0);

	// A StopInZone sample first (in zone, slow, longer than HoldSeconds) while LeaveStart is current: only LeaveStart
	// completes in this call, and the hold starts from zero.
	TestFalse(TEXT("StopInZone sample first does not complete the board"), Board->EvaluateObjectives(InZoneSlow, 2.f));
	TestEqual(TEXT("Only LeaveStart completed"), Board->GetCurrentObjectiveIndex(), 1);
	TestEqual(TEXT("Hold restarts when the objective advances"), Board->GetHoldProgress(), 0.f);

	for (int32 Step = 0; Step < 3; ++Step)
	{
		TestFalse(TEXT("Holding (not yet long enough)"), Board->EvaluateObjectives(Step % 2 == 0 ? InZoneSlow : InZoneCreepingBack, 0.25f));
	}
	TestEqual(TEXT("Hold accumulates (|speed| counts in both directions)"), Board->GetHoldProgress(), 0.75f);

	Board->EvaluateObjectives(InZoneFast, 0.25f);
	TestEqual(TEXT("Moving too fast resets the hold"), Board->GetHoldProgress(), 0.f);
	Board->EvaluateObjectives(InZoneSlow, 0.25f);
	Board->EvaluateObjectives(PastZoneSlow, 0.25f);
	TestEqual(TEXT("Leaving the zone resets the hold"), Board->GetHoldProgress(), 0.f);
	TestEqual(TEXT("Still on StopInZone"), Board->GetCurrentObjectiveIndex(), 1);

	bool bDone = false;
	for (int32 Step = 0; Step < 3; ++Step)
	{
		bDone |= Board->EvaluateObjectives(InZoneSlow, 0.25f);
	}
	TestFalse(TEXT("0.75 s of a 1.0 s hold is not enough"), bDone || Board->AreObjectivesComplete());
	for (int32 Step = 0; Step < 2 && !bDone; ++Step)
	{
		bDone = Board->EvaluateObjectives(InZoneSlow, 0.25f);
	}
	TestTrue(TEXT("After LeaveStart, holding still for HoldSeconds completes the board"), bDone && Board->AreObjectivesComplete());
	TestEqual(TEXT("Index past the last objective"), Board->GetCurrentObjectiveIndex(), 2);

	Board->ResetObjectives();
	TestEqual(TEXT("ResetObjectives rewinds"), Board->GetCurrentObjectiveIndex(), 0);
	TestFalse(TEXT("ResetObjectives clears completion"), Board->AreObjectivesComplete());
	TestEqual(TEXT("ResetObjectives clears the hold"), Board->GetHoldProgress(), 0.f);

	// Order matters: while PassFarMarker is current, a perfect stop earns nothing.
	Board->SetObjectives({ MakeObjective(EHueObjectiveType::PassFarMarker, 0.8f), MakeStopObjective(0.30f, 0.56f, 0.02f, 1.0f) });
	for (int32 Step = 0; Step < 12; ++Step)
	{
		Board->EvaluateObjectives(InZoneSlow, 0.25f);
	}
	TestEqual(TEXT("A stop before the far marker does not count"), Board->GetCurrentObjectiveIndex(), 0);
	TestFalse(TEXT("Not complete before the far marker"), Board->AreObjectivesComplete());
	Board->EvaluateObjectives(MakeSample(0.85f, 0.3f), 0.25f);
	TestEqual(TEXT("Far marker passed"), Board->GetCurrentObjectiveIndex(), 1);
	bool bStopped = false;
	for (int32 Step = 0; Step < 6 && !bStopped; ++Step)
	{
		bStopped = Board->EvaluateObjectives(InZoneSlow, 0.25f);
	}
	TestTrue(TEXT("A stop after the far marker completes"), bStopped);

	// ReceiverColor, then ReturnBelow.
	Board->SetObjectives({ MakeColorObjective(EHueColor::A), MakeObjective(EHueObjectiveType::ReturnBelow, 0.3f) });
	TestFalse(TEXT("Receiver B does not satisfy 'receiver turns A'"), Board->EvaluateObjectives(MakeSample(0.5f, 0.f, EHueColor::B), 0.1f));
	TestEqual(TEXT("Still waiting for colour A"), Board->GetCurrentObjectiveIndex(), 0);
	TestFalse(TEXT("Receiver A completes the colour objective only"), Board->EvaluateObjectives(MakeSample(0.5f, 0.f, EHueColor::A), 0.1f));
	TestEqual(TEXT("Colour objective done"), Board->GetCurrentObjectiveIndex(), 1);
	TestFalse(TEXT("0.35 is not below 0.30"), Board->EvaluateObjectives(MakeSample(0.35f, -0.1f, EHueColor::A), 0.1f));
	TestTrue(TEXT("0.25 is below 0.30"), Board->EvaluateObjectives(MakeSample(0.25f, -0.1f, EHueColor::A), 0.1f));

	Board->SetObjectives(TArray<FHueObjective>());
	TestFalse(TEXT("A board without objectives is never complete"), Board->AreObjectivesComplete());
	return true;
}

// =============================================================================================================
// 13. Tutorial exercises end to end: logic run -> receiver history -> PredictMotion -> objectives
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxTutorialExercise1Test, "Huebox.Tutorial.Exercise1RouteSignal", HUEBOX_TEST_FLAGS)
bool FHueboxTutorialExercise1Test::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);
	const FHueBoardLayout& Layout = Layouts[0];
	CheckTutorialLayoutData(*this, Layout);

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 1 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));

	// As shipped: the barrier absorbs the B pulse, nothing reaches the receiver.
	{
		FLogicRun Unsolved;
		UTEST_TRUE(TEXT("Unsolved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Unsolved));
		ExpectHistory(*this, TEXT("Exercise 1 unsolved"), Unsolved.History, {});
		TestFalse(TEXT("Exercise 1 unsolved: objectives not complete"), EvaluateTutorialObjectives(Fixture, Unsolved.History).bComplete);
	}

	// Baseline solution: T(1,4) West, T(1,1) East, C(2,1) -> receiver turns A at exactly 2.00 s.
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);
	FLogicRun Solved;
	UTEST_TRUE(TEXT("Solved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Solved));
	ExpectHistory(*this, TEXT("Exercise 1 solved"), Solved.History, { MakeEvent(2.0f, EHueColor::A) });
	TestEqual(FString::Printf(TEXT("Exercise 1 solved: no blocked or failed pulses. Log: %s"), *DescribeFeedback(Solved.Feedback)),
		CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Warning) + CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Error), 0);

	const FObjectiveRun Objectives = EvaluateTutorialObjectives(Fixture, Solved.History);
	TestTrue(TEXT("Exercise 1 solved: receiver A, then carriage past the halfway marker"), Objectives.bComplete);
	AddInfo(FString::Printf(TEXT("Exercise 1: history [%s], objectives complete at %.2f s, max fraction %.3f"),
		*DescribeHistory(Solved.History), Objectives.CompletedAt, Objectives.Prediction.MaxFraction));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxTutorialExercise2Test, "Huebox.Tutorial.Exercise2ChangeMotion", HUEBOX_TEST_FLAGS)
bool FHueboxTutorialExercise2Test::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);
	const FHueBoardLayout& Layout = Layouts[1];
	CheckTutorialLayoutData(*this, Layout);

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 2 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));

	// As shipped (delay 0.5 s): the A (route 1) and the delayed B (route 2) both reach R at 1.25 s -> conflict.
	{
		FLogicRun Unsolved;
		UTEST_TRUE(TEXT("Unsolved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Unsolved));
		ExpectHistory(*this, TEXT("Exercise 2 unsolved"), Unsolved.History, {});
		TestEqual(TEXT("Exercise 2 unsolved: same-time arrivals conflict"), Unsolved.ConflictCount, 1);
		TestTrue(TEXT("Exercise 2 unsolved: conflict reported"), CountFeedbackOfKind(Unsolved.Feedback, EHueFeedbackKind::Warning) > 0);
		TestFalse(TEXT("Exercise 2 unsolved: objectives not complete"), EvaluateTutorialObjectives(Fixture, Unsolved.History).bComplete);
	}

	// Baseline solution replaces the removable delay with a 3.0 s one: push (A @1.25), then pull (B @3.75).
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);
	for (const FHueBlockSpec& Spec : Layout.SolutionBlocks)
	{
		if (Spec.Kind == EHueBlockKind::Delay)
		{
			const AHueDelayBlock* Installed = Cast<AHueDelayBlock>(Fixture.Board->GetBlockAt(Spec.Cell));
			if (TestNotNull(TEXT("Solution delay installed"), Installed))
			{
				TestEqual(TEXT("Solution delay replaced the shipped one"), Installed->GetDelaySeconds(), Spec.DelaySeconds);
			}
		}
	}

	FLogicRun Solved;
	UTEST_TRUE(TEXT("Solved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Solved));
	ExpectHistory(*this, TEXT("Exercise 2 solved"), Solved.History, { MakeEvent(1.25f, EHueColor::A), MakeEvent(3.75f, EHueColor::B) });
	TestEqual(TEXT("Exercise 2 solved: no conflict"), Solved.ConflictCount, 0);
	TestEqual(FString::Printf(TEXT("Exercise 2 solved: no blocked or failed pulses. Log: %s"), *DescribeFeedback(Solved.Feedback)),
		CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Warning) + CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Error), 0);

	const FObjectiveRun Objectives = EvaluateTutorialObjectives(Fixture, Solved.History);
	TestTrue(TEXT("Exercise 2 solved: pushed past the far marker, then pulled back past the return marker"), Objectives.bComplete);
	AddInfo(FString::Printf(TEXT("Exercise 2: history [%s], objectives complete at %.2f s, max fraction %.3f"),
		*DescribeHistory(Solved.History), Objectives.CompletedAt, Objectives.Prediction.MaxFraction));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxTutorialExercise3Test, "Huebox.Tutorial.Exercise3SynthesizeStop", HUEBOX_TEST_FLAGS)
bool FHueboxTutorialExercise3Test::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);
	const FHueBoardLayout& Layout = Layouts[2];
	CheckTutorialLayoutData(*this, Layout);

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 3 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));
	AHueMixerBlock* Mixer = FindMixer(Fixture.Board);
	UTEST_NOT_NULL(TEXT("Exercise 3 mixer"), Mixer);

	// As shipped (no delay): A reaches the mixer at 1.00 s, B at 1.75 s -> dt 0.75 s -> timeout, no brake;
	// the carriage keeps accelerating to the far stop.
	{
		FLogicRun Unsolved;
		UTEST_TRUE(TEXT("Unsolved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Unsolved));
		ExpectHistory(*this, TEXT("Exercise 3 unsolved"), Unsolved.History, { MakeEvent(1.0f, EHueColor::A) });
		ExpectFeedback(*this, TEXT("Exercise 3 unsolved: mixer timeout"), Unsolved.Feedback, TEXT("timeout"), true);
		TestFalse(TEXT("Exercise 3 unsolved: mixer left no pulse waiting"), Mixer->IsWaiting());
		const FObjectiveRun UnsolvedObjectives = EvaluateTutorialObjectives(Fixture, Unsolved.History);
		TestFalse(TEXT("Exercise 3 unsolved: objectives not complete"), UnsolvedObjectives.bComplete);
		AddInfo(FString::Printf(TEXT("Exercise 3 unsolved: carriage ends at %.3f of the track"), UnsolvedObjectives.Prediction.FinalFraction));
	}

	// Baseline solution: D(2,1) 1.0 s on the A route -> A at the mixer at 2.00 s, B waiting since 1.75 s
	// (dt 0.25 s) -> N leaves at 2.00 s and brakes the carriage from 2.50 s.
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);
	FLogicRun Solved;
	UTEST_TRUE(TEXT("Solved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Solved));
	const TArray<FHueColorEvent> ExpectedSolved = { MakeEvent(1.0f, EHueColor::A), MakeEvent(2.5f, EHueColor::N) };
	ExpectHistory(*this, TEXT("Exercise 3 solved"), Solved.History, ExpectedSolved);
	ExpectFeedback(*this, TEXT("Exercise 3 solved: inputs meet inside the window"), Solved.Feedback, TEXT("timeout"), false);
	TestEqual(FString::Printf(TEXT("Exercise 3 solved: no blocked or failed pulses. Log: %s"), *DescribeFeedback(Solved.Feedback)),
		CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Warning) + CountFeedbackOfKind(Solved.Feedback, EHueFeedbackKind::Error), 0);

	const FObjectiveRun Objectives = EvaluateTutorialObjectives(Fixture, Solved.History);
	TestTrue(TEXT("Exercise 3 solved: leaves the start, then brakes and holds inside the zone"), Objectives.bComplete);
	if (const FHueObjective* Stop = FindObjective(Layout, EHueObjectiveType::StopInZone))
	{
		TestTrue(FString::Printf(TEXT("Exercise 3 solved: carriage rests inside [%.2f, %.2f] (at %.3f)"), Stop->ZoneMin, Stop->ZoneMax, Objectives.Prediction.FinalFraction),
			Objectives.Prediction.FinalFraction >= Stop->ZoneMin && Objectives.Prediction.FinalFraction <= Stop->ZoneMax);
	}
	AddInfo(FString::Printf(TEXT("Exercise 3: history [%s], objectives complete at %.2f s, final fraction %.3f"),
		*DescribeHistory(Solved.History), Objectives.CompletedAt, Objectives.Prediction.FinalFraction));

	// Determinism: frame length never changes a logical outcome (one 20 s step, 0.25 s frames, 30 fps frames).
	FLogicRun QuarterSecondFrames;
	UTEST_TRUE(TEXT("Replay with 0.25 s frames"), RunTutorialLogic(*this, Sim, Fixture, 0.25, QuarterSecondFrames));
	ExpectHistory(*this, TEXT("Exercise 3 replay, 0.25 s frames"), QuarterSecondFrames.History, ExpectedSolved);
	FLogicRun SingleStep;
	UTEST_TRUE(TEXT("Replay in a single step"), RunTutorialLogic(*this, Sim, Fixture, 20.0, SingleStep));
	ExpectHistory(*this, TEXT("Exercise 3 replay, one 20 s step"), SingleStep.History, ExpectedSolved);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxTutorialExercise4Test, "Huebox.Tutorial.Exercise4ColourKey", HUEBOX_TEST_FLAGS)
bool FHueboxTutorialExercise4Test::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Four tutorial layouts"), Layouts.Num() >= 4);
	const FHueBoardLayout& Layout = Layouts[3];
	CheckTutorialLayoutData(*this, Layout);

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 4 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));

	AHueColorBrickBlock* Brick = nullptr;
	Fixture.Board->ForEachBlock([&Brick](APulseBlock& Block)
	{
		if (!Brick)
		{
			Brick = Cast<AHueColorBrickBlock>(&Block);
		}
	});
	UTEST_NOT_NULL(TEXT("Exercise 4 brick"), Brick);
	TestColor(*this, TEXT("Exercise 4 brick colour"), Brick->GetBrickColor(), EHueColor::Y);

	const FHueBlockSpec* MixerSpec = Layout.SolutionBlocks.FindByPredicate([](const FHueBlockSpec& Spec) { return Spec.Kind == EHueBlockKind::Mixer; });
	const FHueBlockSpec* DelaySpec = Layout.SolutionBlocks.FindByPredicate([](const FHueBlockSpec& Spec) { return Spec.Kind == EHueBlockKind::Delay; });
	UTEST_TRUE(TEXT("Exercise 4 solution: a mixer and a delay"), MixerSpec != nullptr && DelaySpec != nullptr);

	// As shipped: the push hits the standing brick at 0.75 s; the key pulses cross and stop at the opposite sources.
	{
		FLogicRun Unsolved;
		UTEST_TRUE(TEXT("Unsolved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Unsolved));
		ExpectHistory(*this, TEXT("Exercise 4 unsolved"), Unsolved.History, {});
		ExpectFeedback(*this, TEXT("Exercise 4 unsolved: the brick names its key"), Unsolved.Feedback, TEXT("only Y (yellow) light breaks it"), true);
		TestFalse(TEXT("Exercise 4 unsolved: objectives not complete"), EvaluateTutorialObjectives(Fixture, Unsolved.History).bComplete);
	}

	// Mixer only: the Y breaks the brick at 1.25 s, but the push already bounced off it at 0.75 s - order matters.
	UTEST_NOT_NULL(TEXT("Mixer placed"), AHueTutorialBuilder::SpawnBlockFromSpec(Fixture.Board, *MixerSpec, Fixture.Rig));
	{
		FLogicRun KeyOnly;
		UTEST_TRUE(TEXT("Key-only run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, KeyOnly));
		// The feedback names the inputs in arrival order: G (0.50 s) waits, A (0.75 s) completes the mix.
		ExpectFeedback(*this, TEXT("Exercise 4 key only: G + A -> Y"), KeyOnly.Feedback, TEXT("G + A -> Y"), true);
		ExpectFeedback(*this, TEXT("Exercise 4 key only: the brick breaks"), KeyOnly.Feedback, TEXT("broken by Y at 1.25s"), true);
		ExpectHistory(*this, TEXT("Exercise 4 key only: the push came too early"), KeyOnly.History, {});
	}

	// Baseline solution (mixer + 1.0 s delay): the push passes at 1.75 s, the receiver turns A at 2.25 s.
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);
	FLogicRun Solved;
	UTEST_TRUE(TEXT("Solved run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Solved));
	ExpectHistory(*this, TEXT("Exercise 4 solved"), Solved.History, { MakeEvent(2.25f, EHueColor::A) });
	const FObjectiveRun Objectives = EvaluateTutorialObjectives(Fixture, Solved.History);
	TestTrue(TEXT("Exercise 4 solved: pushed past the halfway marker"), Objectives.bComplete);
	AddInfo(FString::Printf(TEXT("Exercise 4: history [%s], objectives complete at %.2f s, max fraction %.3f"),
		*DescribeHistory(Solved.History), Objectives.CompletedAt, Objectives.Prediction.MaxFraction));

	// Delay window: 0.5 s lands in the very instant the brick breaks (still blocked); 0.75 s is the shortest that works.
	AHueDelayBlock* Delay = Cast<AHueDelayBlock>(Fixture.Board->GetBlockAt(DelaySpec->Cell));
	UTEST_NOT_NULL(TEXT("Solution delay installed"), Delay);
	Delay->SetDelaySeconds(0.5f);
	{
		FLogicRun SameInstant;
		UTEST_TRUE(TEXT("0.5 s run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, SameInstant));
		ExpectHistory(*this, TEXT("Exercise 4, delay 0.5 s"), SameInstant.History, {});
	}
	Delay->SetDelaySeconds(0.75f);
	{
		FLogicRun Shortest;
		UTEST_TRUE(TEXT("0.75 s run"), RunTutorialLogic(*this, Sim, Fixture, 1.0 / 30.0, Shortest));
		ExpectHistory(*this, TEXT("Exercise 4, delay 0.75 s"), Shortest.History, { MakeEvent(2.0f, EHueColor::A) });
		TestTrue(TEXT("Exercise 4, delay 0.75 s: complete"), EvaluateTutorialObjectives(Fixture, Shortest.History).bComplete);
	}
	return true;
}

// =============================================================================================================
// Memory / lifetime
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxMemoryRemovedBlockTest, "Huebox.Memory.RemovedBlockIgnoresEvents", HUEBOX_TEST_FLAGS)
bool FHueboxMemoryRemovedBlockTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// S(0,2) -> D(2,2) 1.0 s -> R(5,2). The delay is destroyed while its release event (1.50 s) is still queued.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	const bool bSource = AddSource(*this, Board, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr;
	AHueDelayBlock* Delay = AddDelay(*this, Board, FIntPoint(2, 2), EHueDir::East, 1.0f);
	AHueReceiverBlock* Receiver = AddReceiver(*this, Board, FIntPoint(5, 2), { EHueDir::West });
	UTEST_TRUE(TEXT("Scenario built"), bSource && Delay != nullptr && Receiver != nullptr);

	UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Board));
	FSimClock Clock(Sim);
	Clock.AdvanceTo(0.75);
	TestTrue(TEXT("Delay holding"), Delay->IsHolding());
	TestTrue(TEXT("Release event queued"), Sim->GetPendingEventCount() > 0);

	const TWeakObjectPtr<AHueDelayBlock> WeakDelay(Delay);
	UTEST_TRUE(TEXT("Board removes the holding delay"), Board->RemoveBlock(Delay));
	Delay = nullptr; // destroyed: only the weak pointer may be consulted from here on
	TestFalse(TEXT("Weak pointer reports the destroyed block"), WeakDelay.IsValid());
	TestTrue(TEXT("Cell is free again"), Board->GetBlockAt(FIntPoint(2, 2)) == nullptr && Board->IsCellFree(FIntPoint(2, 2)));
	TestEqual(TEXT("A removed block holds no signal"), Sim->GetActiveSignalCount(), 0);

	// The queued release references the block weakly: it is dropped instead of calling into a dead actor.
	Clock.AdvanceTo(3.0);
	TestEqual(TEXT("Stale event consumed"), Sim->GetPendingEventCount(), 0);
	TestEqual(TEXT("Nothing was released"), Sim->GetTravelingPulseCount(), 0);
	ExpectHistory(*this, TEXT("Receiver after the delay was removed"), Receiver->GetAppliedHistory(), {});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxMemoryWorldTeardownTest, "Huebox.Memory.WorldTeardownReleasesObjects", HUEBOX_TEST_FLAGS)
bool FHueboxMemoryWorldTeardownTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);

	// Only weak pointers survive the scope below; they must all go stale once the world is gone and the GC ran.
	TWeakObjectPtr<UWorld> WeakWorld;
	TWeakObjectPtr<UPulseSubsystem> WeakSim;
	TWeakObjectPtr<AHueBoard> WeakBoard;
	TWeakObjectPtr<AReactiveRig> WeakRig;
	TArray<TWeakObjectPtr<APulseBlock>> WeakBlocks;
	{
		FHueTestWorld TestWorld;
		UPulseSubsystem* Sim = TestWorld.GetSim();
		UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
		Sim->SetVisualsEnabled(false);
		FTutorialFixture Fixture;
		UTEST_TRUE(TEXT("Exercise 3 spawned"), SpawnTutorialBoard(*this, TestWorld, Layouts[2], Fixture));

		// Stop mid-run with live state everywhere: a pulse in flight, a queued arrival, the receiver driving the rig
		// (A since 1.00 s) and a mixer failure flash timer (timeout at 1.50 s) still pending.
		UTEST_TRUE(TEXT("Run started"), StartBoardRun(Sim, Fixture.Board));
		Sim->StepSimulation(1.625);
		TestTrue(TEXT("A pulse is in flight at teardown"), Sim->GetTravelingPulseCount() > 0);
		TestTrue(TEXT("Events are queued at teardown"), Sim->GetPendingEventCount() > 0);
		TestColor(*this, TEXT("Rig driven by the receiver"), Fixture.Rig->GetCurrentColor(), EHueColor::A);

		WeakWorld = TestWorld.Get();
		WeakSim = Sim;
		WeakBoard = Fixture.Board;
		WeakRig = Fixture.Rig;
		Fixture.Board->ForEachBlock([&WeakBlocks](APulseBlock& Block) { WeakBlocks.Emplace(&Block); });
		TestTrue(TEXT("Everything alive while the world exists"), WeakWorld.IsValid() && WeakSim.IsValid() && WeakBoard.IsValid() && WeakRig.IsValid());
		TestTrue(TEXT("Blocks recorded"), WeakBlocks.Num() > 0);
	}
	// The RAII helper ended play (timers cleared), destroyed the world context and the world (unrooted). A full
	// GC must now reclaim everything; a survivor would mean a stray strong reference (root set, static, ...).
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	TestFalse(TEXT("World collected"), WeakWorld.IsValid());
	TestFalse(TEXT("Pulse subsystem collected with its world"), WeakSim.IsValid());
	TestFalse(TEXT("Board collected"), WeakBoard.IsValid());
	TestFalse(TEXT("Rig collected"), WeakRig.IsValid());
	int32 Survivors = 0;
	for (const TWeakObjectPtr<APulseBlock>& WeakBlock : WeakBlocks)
	{
		Survivors += WeakBlock.IsValid() ? 1 : 0;
	}
	TestEqual(TEXT("All blocks collected"), Survivors, 0);
	return true;
}

// =============================================================================================================
// Exhibit manager: run state never outlives its supervisor or its board; Space after a run replays that board
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxManagerDestroyedMidRunTest, "Huebox.Manager.DestroyedMidRunResetsAttempt", HUEBOX_TEST_FLAGS)
bool FHueboxManagerDestroyedMidRunTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);
	const FHueBoardLayout& Layout = Layouts[1];

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	AExhibitManager* Manager = SpawnManager(TestWorld);
	UTEST_NOT_NULL(TEXT("Exhibit manager"), Manager);

	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 2 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);
	AHueDelayBlock* Delay = Cast<AHueDelayBlock>(Fixture.Board->GetBlockAt(FIntPoint(5, 1)));
	UTEST_NOT_NULL(TEXT("Solution delay"), Delay);

	// Mid-run at 2.00 s: the receiver applied A (1.25 s), the 3.0 s delay holds B (since 0.25 s), carriage active.
	UTEST_TRUE(TEXT("Run started through the manager"), Manager->StartRun(Fixture.Board));
	Sim->StepSimulation(2.0);
	TestTrue(TEXT("Manager running"), Manager->GetState() == EHueRunState::Running);
	TestColor(*this, TEXT("Receiver mid-run"), Fixture.Receiver->GetCurrentColor(), EHueColor::A);
	TestColor(*this, TEXT("Rig mid-run"), Fixture.Rig->GetCurrentColor(), EHueColor::A);
	TestTrue(TEXT("Delay holding mid-run"), Delay->IsHolding());
	TestTrue(TEXT("Carriage motion active mid-run"), Fixture.Rig->IsMotionActive());

	// The manager is removed while the world keeps playing: nothing could reset this attempt afterwards.
	Manager->Destroy();
	Manager = nullptr;

	TestFalse(TEXT("Simulation stopped"), Sim->IsRunning());
	TestEqual(TEXT("No queued events"), Sim->GetPendingEventCount(), 0);
	TestEqual(TEXT("No pulses"), Sim->GetTravelingPulseCount(), 0);
	TestFalse(TEXT("Delay released its held pulse"), Delay->IsHolding());
	TestColor(*this, TEXT("Receiver reset"), Fixture.Receiver->GetCurrentColor(), EHueColor::N);
	TestEqual(TEXT("Receiver history cleared"), Fixture.Receiver->GetAppliedHistory().Num(), 0);
	TestColor(*this, TEXT("Rig reset"), Fixture.Rig->GetCurrentColor(), EHueColor::N);
	TestFalse(TEXT("Carriage motion stopped"), Fixture.Rig->IsMotionActive());
	TestEqual(TEXT("Objectives reset"), Fixture.Board->GetCurrentObjectiveIndex(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxManagerBoardRemovedWhilePausedTest, "Huebox.Manager.BoardRemovedWhilePausedResets", HUEBOX_TEST_FLAGS)
bool FHueboxManagerBoardRemovedWhilePausedTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);
	const FHueBoardLayout& Layout = Layouts[0];

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	AExhibitManager* Manager = SpawnManager(TestWorld);
	UTEST_NOT_NULL(TEXT("Exhibit manager"), Manager);

	FTutorialFixture Fixture;
	UTEST_TRUE(TEXT("Exercise 1 spawned"), SpawnTutorialBoard(*this, TestWorld, Layout, Fixture));
	AHueTutorialBuilder::InstallSolution(Fixture.Board, Layout);

	UTEST_TRUE(TEXT("Run started through the manager"), Manager->StartRun(Fixture.Board));
	Sim->StepSimulation(1.0);
	TestTrue(TEXT("A pulse is in flight"), Sim->GetTravelingPulseCount() > 0);
	Manager->SetPaused(true);
	UTEST_TRUE(TEXT("Manager paused"), Manager->GetState() == EHueRunState::Paused);
	TestTrue(TEXT("Simulation paused"), Sim->IsPaused());

	// The board goes away while paused (e.g. the builder is removed): the paused simulation never steps, so the
	// manager must notice on its own tick.
	AddExpectedMessage(TEXT("the running board was removed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, /*IsRegex*/ false);
	Fixture.Board->Destroy();
	Fixture.Board = nullptr;
	Manager->Tick(1.f / 60.f);

	TestTrue(TEXT("Manager back in Build"), Manager->GetState() == EHueRunState::Build);
	TestNull(TEXT("No active board"), Manager->GetActiveBoard());
	TestTrue(TEXT("Editing unlocked"), Manager->IsEditingAllowed());
	TestFalse(TEXT("Simulation stopped"), Sim->IsRunning());
	TestEqual(TEXT("Pulses cleared"), Sim->GetTravelingPulseCount(), 0);
	TestEqual(TEXT("No queued events"), Sim->GetPendingEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxManagerRerunActiveBoardTest, "Huebox.Manager.RerunReplaysActiveBoard", HUEBOX_TEST_FLAGS)
bool FHueboxManagerRerunActiveBoardTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	TArray<FHueBoardLayout> Layouts;
	AHueTutorialBuilder::MakeDefaultTutorialLayouts(Layouts);
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);
	AExhibitManager* Manager = SpawnManager(TestWorld);
	UTEST_NOT_NULL(TEXT("Exhibit manager"), Manager);

	FTutorialFixture First;
	FTutorialFixture Second;
	UTEST_TRUE(TEXT("Exercise 1 spawned"), SpawnTutorialBoard(*this, TestWorld, Layouts[0], First));
	UTEST_TRUE(TEXT("Exercise 2 spawned"), SpawnTutorialBoard(*this, TestWorld, Layouts[1], Second));

	// In front of exercise 2 (well outside exercise 1's bounds).
	const FBox SecondBounds = Second.Board->GetWorldBounds();
	const FVector AtSecond(SecondBounds.GetCenter().X, SecondBounds.Max.Y + 300.0, 100.0);
	UTEST_TRUE(TEXT("The test location is at exercise 2"), Manager->FindBoardNear(AtSecond) == Second.Board);

	// Exercise 1 as shipped cannot finish: run it into the time limit.
	UTEST_TRUE(TEXT("Exercise 1 run started"), Manager->StartRun(First.Board));
	AddExpectedMessage(TEXT("Time limit"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, /*IsRegex*/ false);
	Sim->StepSimulation(First.Board->RunTimeLimitSeconds);
	Manager->Tick(1.f / 60.f);
	UTEST_TRUE(TEXT("Exercise 1 ended at the time limit"), Manager->GetState() == EHueRunState::Ended);
	TestEqual(TEXT("A frozen carriage reports no speed"), First.Rig->GetCarriageSpeed(), 0.f);

	// "ENDED ... Space: run again" with exercise 1 on the panel: Space reruns exercise 1 even when standing at 2.
	Manager->ToggleRun(AtSecond);
	TestTrue(TEXT("Rerun is running"), Manager->GetState() == EHueRunState::Running);
	TestTrue(TEXT("Space after a run replays the board shown on the panel"), Manager->GetActiveBoard() == First.Board);

	// After R (Build) Space follows the player to the board they stand at.
	Manager->ResetAttempt();
	UTEST_TRUE(TEXT("Reset to Build"), Manager->GetState() == EHueRunState::Build);
	Manager->ToggleRun(AtSecond);
	TestTrue(TEXT("Build: Space starts the board near the player"), Manager->GetActiveBoard() == Second.Board);
	Manager->ResetAttempt();
	return true;
}

// =============================================================================================================
// Views: build-camera height, grid cursor, stage camera (pure math, plus one live stage camera)
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxViewTopDownHeightTest, "Huebox.View.TopDownHeight", HUEBOX_TEST_FLAGS)
bool FHueboxViewTopDownHeightTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// Looking straight down from height H, the view spans 2 H tan(hfov / 2) along X and 1 / aspect of that along Y, so
	// H = max(AreaX / (2 tan(hfov / 2)), AreaY / (2 tan(vfov / 2))) with tan(vfov / 2) = tan(hfov / 2) / aspect.
	// At 60 degrees tan(30) = 1 / sqrt(3), hence the sqrt(3) factors.
	const float Sqrt3 = FMath::Sqrt(3.f);
	const float Wide = 16.f / 9.f;
	struct FFitCase
	{
		const TCHAR* Name;
		FVector2D Area;
		float FovDegrees;
		float Aspect;
		float Expected;
		/** Which sides touch the view edge at the computed height (both for an exact fit). */
		bool bTightX;
		bool bTightY;
	};
	const FFitCase Cases[] =
	{
		// 10:3 is wider than the 16:9 view: the width decides.
		{ TEXT("2000 x 600, 60 deg, 16:9 (width-limited)"), FVector2D(2000.0, 600.0), 60.f, Wide, 1000.f * Sqrt3, true, false },
		// 5:3 is narrower than 16:9: the height decides although X is the longer side (the X fit alone is 866 cm).
		{ TEXT("1000 x 600, 60 deg, 16:9 (height-limited)"), FVector2D(1000.0, 600.0), 60.f, Wide, 600.f * Wide * Sqrt3 * 0.5f, false, true },
		{ TEXT("600 x 1000, 60 deg, 16:9 (height-limited)"), FVector2D(600.0, 1000.0), 60.f, Wide, 1000.f * Wide * Sqrt3 * 0.5f, false, true },
		// tan(45) = 1: a square area in a square view fits at half its side, touching all four edges.
		{ TEXT("800 x 800, 90 deg, 1:1 (exact fit)"), FVector2D(800.0, 800.0), 90.f, 1.f, 400.f, true, true },
	};

	for (const FFitCase& Case : Cases)
	{
		const float Height = AHueBuilderPawn::ComputeTopDownHeight(Case.Area, Case.FovDegrees, Case.Aspect);
		TestEqual(FString::Printf(TEXT("%s: camera height"), Case.Name), Height, Case.Expected, 0.05f);

		// Independent of the formula: from that height the whole area is visible and the limiting side just fits.
		const float Tolerance = 1e-3f * Case.Expected;
		const float AreaX = static_cast<float>(Case.Area.X);
		const float AreaY = static_cast<float>(Case.Area.Y);
		const float VisibleX = 2.f * Height * FMath::Tan(FMath::DegreesToRadians(Case.FovDegrees * 0.5f));
		const float VisibleY = VisibleX / Case.Aspect;
		TestTrue(FString::Printf(TEXT("%s: whole area visible (view %.1f x %.1f cm)"), Case.Name, VisibleX, VisibleY),
			VisibleX >= AreaX - Tolerance && VisibleY >= AreaY - Tolerance);
		TestTrue(FString::Printf(TEXT("%s: X edge %s"), Case.Name, Case.bTightX ? TEXT("touches") : TEXT("has room")),
			FMath::IsNearlyEqual(VisibleX, AreaX, Tolerance) == Case.bTightX);
		TestTrue(FString::Printf(TEXT("%s: Y edge %s"), Case.Name, Case.bTightY ? TEXT("touches") : TEXT("has room")),
			FMath::IsNearlyEqual(VisibleY, AreaY, Tolerance) == Case.bTightY);
	}

	const FVector2D Area(1000.0, 600.0);
	TestTrue(TEXT("A wider lens brings the camera closer"),
		AHueBuilderPawn::ComputeTopDownHeight(Area, 90.f, Wide) < AHueBuilderPawn::ComputeTopDownHeight(Area, 55.f, Wide));

	// The pawn's own defaults on a 6 x 6 tutorial grid (1 m cells) give a usable height.
	const AHueBuilderPawn* Defaults = GetDefault<AHueBuilderPawn>();
	const float Side = 6.f * 100.f + 2.f * Defaults->CameraMargin;
	const float TutorialHeight = AHueBuilderPawn::ComputeTopDownHeight(FVector2D(Side, Side), Defaults->CameraFov, Wide);
	TestTrue(FString::Printf(TEXT("Default build view over a 6 x 6 board is finite and positive (%.1f cm)"), TutorialHeight),
		FMath::IsFinite(TutorialHeight) && TutorialHeight > 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxViewClampCursorTest, "Huebox.View.ClampCursor", HUEBOX_TEST_FLAGS)
bool FHueboxViewClampCursorTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// Non-square grid so swapped axes would show: x in [0, 8], y in [0, 5].
	const FIntPoint Grid(9, 6);
	auto ClampAt = [&Grid](int32 X, int32 Y) { return AHueBuilderPawn::ClampCursor(FIntPoint(X, Y), Grid); };

	TestEqual(TEXT("Inside stays"), ClampAt(4, 2), FIntPoint(4, 2));
	TestEqual(TEXT("Origin corner stays"), ClampAt(0, 0), FIntPoint(0, 0));
	TestEqual(TEXT("Far corner stays"), ClampAt(8, 5), FIntPoint(8, 5));

	TestEqual(TEXT("Negative x clamps to 0"), ClampAt(-1, 3), FIntPoint(0, 3));
	TestEqual(TEXT("Negative y clamps to 0"), ClampAt(4, -7), FIntPoint(4, 0));
	TestEqual(TEXT("Both negative"), ClampAt(-3, -3), FIntPoint(0, 0));

	TestEqual(TEXT("x = width clamps to width - 1"), ClampAt(9, 2), FIntPoint(8, 2));
	TestEqual(TEXT("y = height clamps to height - 1"), ClampAt(3, 6), FIntPoint(3, 5));
	TestEqual(TEXT("Far outside clamps to the far corner"), ClampAt(100, 100), FIntPoint(8, 5));
	TestEqual(TEXT("x too small, y too large"), ClampAt(-2, 9), FIntPoint(0, 5));
	TestEqual(TEXT("x too large, y too small"), ClampAt(12, -1), FIntPoint(8, 0));

	// WASD / arrows at the border: a one-cell step from any cell moves exactly one cell, or stays put at the edge.
	int32 Escapes = 0;
	int32 WrongSteps = 0;
	for (int32 Y = 0; Y < Grid.Y; ++Y)
	{
		for (int32 X = 0; X < Grid.X; ++X)
		{
			for (const EHueDir Dir : AllDirs)
			{
				const FIntPoint From(X, Y);
				const FIntPoint Wanted = From + HueDir::ToOffset(Dir);
				const FIntPoint Got = AHueBuilderPawn::ClampCursor(Wanted, Grid);
				const bool bWantedInside = Wanted.X >= 0 && Wanted.Y >= 0 && Wanted.X < Grid.X && Wanted.Y < Grid.Y;
				Escapes += (Got.X >= 0 && Got.Y >= 0 && Got.X < Grid.X && Got.Y < Grid.Y) ? 0 : 1;
				WrongSteps += (Got == (bWantedInside ? Wanted : From)) ? 0 : 1;
			}
		}
	}
	TestEqual(TEXT("The cursor never leaves the grid"), Escapes, 0);
	TestEqual(TEXT("Steps move one cell, or stay at the border"), WrongSteps, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxViewStageCameraMathTest, "Huebox.View.StageCameraMath", HUEBOX_TEST_FLAGS)
bool FHueboxViewStageCameraMathTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	// Track of 1000 cm starting at rig X = 200 with a 150 cm margin: the camera follows inside [50, 1350].
	TestEqual(TEXT("Performer on the track is followed exactly"), AHueStageCamera::ClampFollowX(640.f, 200.f, 1000.f, 150.f), 640.f);
	TestEqual(TEXT("Inside the margin before the anchor end"), AHueStageCamera::ClampFollowX(120.f, 200.f, 1000.f, 150.f), 120.f);
	TestEqual(TEXT("Before the anchor end clamps to RigX - Margin"), AHueStageCamera::ClampFollowX(-400.f, 200.f, 1000.f, 150.f), 50.f);
	TestEqual(TEXT("Past the far end clamps to RigX + TrackLength + Margin"), AHueStageCamera::ClampFollowX(5000.f, 200.f, 1000.f, 150.f), 1350.f);
	TestEqual(TEXT("Lower bound is inclusive"), AHueStageCamera::ClampFollowX(50.f, 200.f, 1000.f, 150.f), 50.f);
	TestEqual(TEXT("Upper bound is inclusive"), AHueStageCamera::ClampFollowX(1350.f, 200.f, 1000.f, 150.f), 1350.f);
	TestEqual(TEXT("No margin: clamps to the anchor end"), AHueStageCamera::ClampFollowX(100.f, 200.f, 1000.f, 0.f), 200.f);
	TestEqual(TEXT("No margin: clamps to the far end"), AHueStageCamera::ClampFollowX(1300.f, 200.f, 1000.f, 0.f), 1200.f);
	TestEqual(TEXT("Rig at negative X"), AHueStageCamera::ClampFollowX(-1000.f, -200.f, 1000.f, 150.f), -350.f);

	// Viewer side: SideDistance towards +Y of the rig, Height above it; X is the followed X, not the rig's.
	const FVector RigOrigin(-200.0, -170.0, 30.0);
	TestEqual(TEXT("Camera location"), AHueStageCamera::ComputeCameraLocation(RigOrigin, 400.f, 650.f, 170.f), FVector(400.0, 480.0, 200.0), 1e-3f);
	TestEqual(TEXT("Only X follows the performer"), AHueStageCamera::ComputeCameraLocation(RigOrigin, 900.f, 650.f, 170.f), FVector(900.0, 480.0, 200.0), 1e-3f);

	// Live camera: SetRig(snap) jumps straight to the (clamped) performer and looks across the track at the stage.
	FHueTestWorld TestWorld;
	AReactiveRig* Rig = TestWorld.SpawnRig(FVector(-200.0, -170.0, 0.0), 1000.f);
	UTEST_NOT_NULL(TEXT("Rig"), Rig);
	AHueStageCamera* StageCamera = SpawnTestActor<AHueStageCamera>(TestWorld);
	UTEST_NOT_NULL(TEXT("Stage camera"), StageCamera);
	UTEST_NOT_NULL(TEXT("Stage camera component"), StageCamera->GetCamera());
	TestEqual(TEXT("Camera component uses the stage FOV"), StageCamera->GetCamera()->FieldOfView, StageCamera->FieldOfView);

	StageCamera->SetRig(Rig, /*bSnap*/ true);
	TestTrue(TEXT("Follows the rig"), StageCamera->GetRig() == Rig);

	const FVector Origin = Rig->GetActorLocation();
	const float FollowX = AHueStageCamera::ClampFollowX(static_cast<float>(Rig->GetPerformerLocation().X), static_cast<float>(Origin.X),
		Rig->GetTrackLength(), StageCamera->TrackMargin);
	const FVector ExpectedLocation = AHueStageCamera::ComputeCameraLocation(Origin, FollowX, StageCamera->SideDistance, StageCamera->Height);
	TestEqual(TEXT("Snapped onto the performer"), StageCamera->GetActorLocation(), ExpectedLocation, 0.1f);
	TestTrue(TEXT("Stands on the viewer side (+Y) of the rig"), ExpectedLocation.Y > Origin.Y);

	// Aim: straight across the track (no X component), towards -Y, at the stage's LookHeight (within ~2.5 degrees).
	const FVector Forward = StageCamera->GetActorForwardVector();
	const FVector ToStage = (FVector(static_cast<double>(FollowX), Origin.Y, Origin.Z + StageCamera->LookHeight) - ExpectedLocation).GetSafeNormal();
	TestTrue(FString::Printf(TEXT("Looks across the track at the stage (forward %s)"), *Forward.ToString()),
		FMath::Abs(Forward.X) < 1e-3 && Forward.Y < 0.0 && FVector::DotProduct(Forward, ToStage) > 0.999);

	StageCamera->SetRig(nullptr, /*bSnap*/ true);
	TestNull(TEXT("SetRig(null) releases the rig"), StageCamera->GetRig());
	return true;
}

// =============================================================================================================
// Minimap: travelling pulses as fractional cell positions
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxSimPulseSnapshotsTest, "Huebox.Sim.PulseSnapshots", HUEBOX_TEST_FLAGS)
bool FHueboxSimPulseSnapshotsTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// Row: S(0,2) fires B East; (1,2) is empty (reached at 0.25 s, the pulse continues); R(2,2) consumes it at 0.50 s.
	AHueBoard* Row = TestWorld.SpawnBoard(BoardOrigin(0), FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Row board"), Row);
	const bool bRowSource = AddSource(*this, Row, FIntPoint(0, 2), EHueDir::East, EHueColor::B) != nullptr;
	AHueReceiverBlock* RowReceiver = AddReceiver(*this, Row, FIntPoint(2, 2), { EHueDir::West });
	UTEST_TRUE(TEXT("Row scenario built"), bRowSource && RowReceiver != nullptr);

	TestEqual(TEXT("No snapshots before a run"), GetSnapshots(Sim).Num(), 0);
	UTEST_TRUE(TEXT("Row run started"), StartBoardRun(Sim, Row));
	{
		FSimClock Clock(Sim);
		ExpectSingleSnapshot(*this, TEXT("t = 0: on the source cell"), Sim, FVector2D(0.0, 2.0), EHueColor::B);
		Clock.AdvanceTo(0.125);
		ExpectSingleSnapshot(*this, TEXT("0.125 s: halfway between the source and its neighbour"), Sim, FVector2D(0.5, 2.0), EHueColor::B);
		Clock.AdvanceTo(0.25);
		ExpectSingleSnapshot(*this, TEXT("0.25 s: on the empty neighbour cell"), Sim, FVector2D(1.0, 2.0), EHueColor::B);
		Clock.AdvanceTo(0.375);
		ExpectSingleSnapshot(*this, TEXT("0.375 s: halfway to the receiver"), Sim, FVector2D(1.5, 2.0), EHueColor::B);
		Clock.AdvanceTo(0.5);
		TestEqual(TEXT("0.50 s: the receiver consumed the pulse - no snapshots"), GetSnapshots(Sim).Num(), 0);
		TestColor(*this, TEXT("Row receiver"), RowReceiver->GetCurrentColor(), EHueColor::B);
	}

	// Column: S(2,0) fires A North into D(2,2) 1.0 s -> R(2,4) (South face). Y interpolates too, and a pulse held
	// in the delay is not travelling, so it has no snapshot (stored 0.50 s, released 1.50 s, consumed 2.00 s).
	AHueBoard* Column = TestWorld.SpawnBoard(BoardOrigin(1), FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Column board"), Column);
	const bool bColumnSource = AddSource(*this, Column, FIntPoint(2, 0), EHueDir::North, EHueColor::A) != nullptr;
	AHueDelayBlock* Delay = AddDelay(*this, Column, FIntPoint(2, 2), EHueDir::North, 1.0f);
	AHueReceiverBlock* ColumnReceiver = AddReceiver(*this, Column, FIntPoint(2, 4), { EHueDir::South });
	UTEST_TRUE(TEXT("Column scenario built"), bColumnSource && Delay != nullptr && ColumnReceiver != nullptr);

	UTEST_TRUE(TEXT("Column run started"), StartBoardRun(Sim, Column));
	{
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.125);
		ExpectSingleSnapshot(*this, TEXT("0.125 s: halfway up the column"), Sim, FVector2D(2.0, 0.5), EHueColor::A);
		Clock.AdvanceTo(0.5);
		TestTrue(TEXT("0.50 s: the delay holds the pulse"), Delay->IsHolding());
		TestEqual(TEXT("A held pulse has no snapshot"), GetSnapshots(Sim).Num(), 0);
		TestEqual(TEXT("... although it is still an active signal"), Sim->GetActiveSignalCount(), 1);
		Clock.AdvanceTo(1.5);
		ExpectSingleSnapshot(*this, TEXT("1.50 s: released on the delay's cell"), Sim, FVector2D(2.0, 2.0), EHueColor::A);
		Clock.AdvanceTo(1.625);
		ExpectSingleSnapshot(*this, TEXT("1.625 s: halfway past the delay"), Sim, FVector2D(2.0, 2.5), EHueColor::A);
		Clock.AdvanceTo(2.0);
		TestEqual(TEXT("2.00 s: consumed - no snapshots"), GetSnapshots(Sim).Num(), 0);
		ExpectHistory(*this, TEXT("Column receiver"), ColumnReceiver->GetAppliedHistory(), { MakeEvent(2.0f, EHueColor::A) });
	}

	// A reset (StopAndClear) mid-flight drops every snapshot.
	UTEST_TRUE(TEXT("Row rerun started"), StartBoardRun(Sim, Row));
	{
		FSimClock Clock(Sim);
		Clock.AdvanceTo(0.125);
		TestEqual(TEXT("Rerun: one pulse in flight"), GetSnapshots(Sim).Num(), 1);
	}
	Sim->StopAndClear();
	TestEqual(TEXT("No snapshots after StopAndClear"), GetSnapshots(Sim).Num(), 0);
	return true;
}

// =============================================================================================================
// Build tool + board trays (Instruction mode)
// =============================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxBuildTrayPlaceAndCollectTest, "Huebox.Build.TrayPlaceAndCollect", HUEBOX_TEST_FLAGS)
bool FHueboxBuildTrayPlaceAndCollectTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// No exhibit manager in this world, so editing is always allowed (the Build-state lock is not exercised here).
	// Fixed source at (0,0); the board's tray holds one Turn, as an Instruction exercise would.
	AHueBoard* Board = TestWorld.SpawnBoard(FVector::ZeroVector, FIntPoint(6, 6));
	UTEST_NOT_NULL(TEXT("Board"), Board);
	UHueInventoryComponent* Tray = Board->GetTray();
	UTEST_NOT_NULL(TEXT("Every board owns a tray"), Tray);
	AHueSourceBlock* Source = AddSource(*this, Board, FIntPoint(0, 0), EHueDir::East, EHueColor::B);
	UTEST_NOT_NULL(TEXT("Fixed source"), Source);
	const TWeakObjectPtr<AHueSourceBlock> WeakSource(Source);
	UTEST_TRUE(TEXT("Tray filled with one Turn"), Tray->AddItem(EHueModuleType::Turn, 1) == 1);
	Tray->SelectSlot(0);

	UHueBuildToolComponent* Tool = SpawnBuildTool(TestWorld, Tray);
	UTEST_NOT_NULL(TEXT("Build tool on a stand-in pawn"), Tool);
	AActor* Player = Tool->GetOwner();
	TestTrue(TEXT("The tool pays from the board's tray"), Tool->GetInventory() == Tray);
	TestTrue(TEXT("No exhibit manager: editing allowed"), Tool->IsEditingAllowed());

	// ---- Place on an empty cell -------------------------------------------------------------------------------------
	const FIntPoint Cell(2, 3);
	Tool->SetTargetCell(Board, Cell);
	ExpectCellTarget(*this, TEXT("Cursor on an empty cell, Turn selected"), Tool, /*bCanPlace*/ true);
	TestTrue(TEXT("Cell target names the board"), Tool->GetTarget().Board.Get() == Board);
	TestEqual(TEXT("Cell target names the cell"), Tool->GetTarget().Cell, Cell);

	// Q / E with nothing rotatable under the cursor turn the placement preview.
	Tool->SetPreviewFacing(EHueDir::East);
	Tool->RotateTargetOrPreview(+1);
	TestDir(*this, TEXT("E on an empty cell rotates the preview"), Tool->GetPreviewFacing(), EHueDir::North);

	TestTrue(TEXT("TryPlaceSelected on the empty cell"), Tool->TryPlaceSelected());
	AHueTurnBlock* Placed = Cast<AHueTurnBlock>(Board->GetBlockAt(Cell));
	UTEST_NOT_NULL(TEXT("A Turn stands on the cell"), Placed);
	TestDir(*this, TEXT("Placed with the preview facing"), Placed->GetFacing(), EHueDir::North);
	TestTrue(TEXT("Player-placed modules are removable"), Placed->IsRemovable());
	TestEqual(TEXT("The tray paid for the placement"), Tray->CountOf(EHueModuleType::Turn), 0);
	TestTrue(TEXT("An empty tray selects nothing"), Tray->GetSelectedType() == EHueModuleType::None);

	// The target is rebuilt from board + cell: the cursor now points at the new block.
	Tool->RefreshTarget();
	ExpectTargetKind(*this, TEXT("Refreshed target after placing"), Tool, EHueTargetKind::Block);
	TestTrue(TEXT("Block target is the placed Turn"), Tool->GetTarget().Block.Get() == Placed);
	TestFalse(TEXT("A removable block offers a prompt"), IHueInteractable::Execute_GetInteractPrompt(Placed, Player).IsEmpty());

	// Q / E on a removable block turn the block; the preview keeps its facing.
	Tool->RotateTargetOrPreview(+1);
	TestDir(*this, TEXT("E on the placed Turn rotates it"), Placed->GetFacing(), EHueDir::West);
	TestDir(*this, TEXT("Preview untouched while a block is targeted"), Tool->GetPreviewFacing(), EHueDir::North);

	// With the tray empty another free cell is refused (with a reason) and nothing is spawned.
	const FIntPoint OtherCell(4, 4);
	Tool->SetTargetCell(Board, OtherCell);
	ExpectCellTarget(*this, TEXT("Free cell, empty tray"), Tool, /*bCanPlace*/ false);
	TestFalse(TEXT("TryPlaceSelected with an empty tray"), Tool->TryPlaceSelected());
	TestNull(TEXT("Nothing placed from an empty tray"), Board->GetBlockAt(OtherCell));

	// ---- RMB collects the block back into the tray ----------------------------------------------------------------
	const TWeakObjectPtr<AHueTurnBlock> WeakPlaced(Placed);
	Placed = nullptr; // destroyed by the collection: only the weak pointer may be consulted from here on
	Tool->SetTargetCell(Board, Cell);
	ExpectTargetKind(*this, TEXT("Cursor on the placed Turn"), Tool, EHueTargetKind::Block);
	Tool->SecondaryAction();
	TestFalse(TEXT("RMB collected (destroyed) the block"), WeakPlaced.IsValid());
	TestTrue(TEXT("Cell free again"), Board->GetBlockAt(Cell) == nullptr && Board->IsCellFree(Cell));
	TestEqual(TEXT("The Turn went back into the tray"), Tray->CountOf(EHueModuleType::Turn), 1);
	TestTrue(TEXT("The collected module is selected again"), Tray->GetSelectedType() == EHueModuleType::Turn);
	Tool->RefreshTarget();
	ExpectCellTarget(*this, TEXT("Refreshed target after collecting"), Tool, /*bCanPlace*/ true);

	// ---- LMB round trip: place, then LMB on the block collects it through IHueInteractable (Interactor = owner) ----
	Tool->PrimaryAction();
	AHueTurnBlock* Replaced = Cast<AHueTurnBlock>(Board->GetBlockAt(Cell));
	TestNotNull(TEXT("LMB on an empty cell placed a Turn"), Replaced);
	TestEqual(TEXT("The LMB placement paid from the tray"), Tray->CountOf(EHueModuleType::Turn), 0);
	ExpectTargetKind(*this, TEXT("PrimaryAction refreshed the target onto the new block"), Tool, EHueTargetKind::Block);
	const TWeakObjectPtr<AHueTurnBlock> WeakReplaced(Replaced);
	Replaced = nullptr;
	Tool->PrimaryAction();
	TestFalse(TEXT("LMB on a removable block collected it"), WeakReplaced.IsValid());
	TestEqual(TEXT("The LMB collection refilled the tray"), Tray->CountOf(EHueModuleType::Turn), 1);
	ExpectCellTarget(*this, TEXT("Cell target again after the LMB collection"), Tool, /*bCanPlace*/ true);

	// ---- Fixed units are refused by every path ----------------------------------------------------------------------
	Tool->SetTargetCell(Board, FIntPoint(0, 0));
	ExpectTargetKind(*this, TEXT("Cursor on the fixed source"), Tool, EHueTargetKind::Block);
	TestTrue(TEXT("Block target is the source"), Tool->GetTarget().Block.Get() == Source);
	Tool->SecondaryAction();
	Tool->PrimaryAction();
	TestFalse(TEXT("TryCollectBlock refuses a fixed unit"), WeakSource.IsValid() && Tool->TryCollectBlock(WeakSource.Get()));
	UTEST_TRUE(TEXT("The source is still on its cell"), WeakSource.IsValid() && Board->GetBlockAt(FIntPoint(0, 0)) == WeakSource.Get());
	TestEqual(TEXT("Refused collections add nothing"), Tray->CountOf(EHueModuleType::Turn), 1);
	Tool->RotateTargetOrPreview(+1);
	TestDir(*this, TEXT("Fixed units do not rotate"), WeakSource->GetFacing(), EHueDir::East);
	TestDir(*this, TEXT("Q / E on a fixed unit leave the preview alone"), Tool->GetPreviewFacing(), EHueDir::North);

	// ---- Owner veto (the sandbox pawn binds its IsStandingOnCell) ---------------------------------------------------
	const FIntPoint VetoCell(3, 3);
	// Captures a value only (no UObject); unbound again below.
	Tool->CellBlockedQuery.BindLambda([VetoCell](const AHueBoard* /*QueryBoard*/, FIntPoint QueryCell) { return QueryCell == VetoCell; });
	Tool->SetTargetCell(Board, VetoCell);
	ExpectCellTarget(*this, TEXT("Vetoed cell"), Tool, /*bCanPlace*/ false);
	TestEqual(TEXT("The veto shows the owner's reason"), Tool->GetTarget().Reason, Tool->CellBlockedReason);
	TestFalse(TEXT("TryPlaceSelected re-checks the veto"), Tool->TryPlaceSelected());
	TestNull(TEXT("Nothing placed on the vetoed cell"), Board->GetBlockAt(VetoCell));
	TestEqual(TEXT("A vetoed placement costs nothing"), Tray->CountOf(EHueModuleType::Turn), 1);
	Tool->SetTargetCell(Board, FIntPoint(4, 3));
	ExpectCellTarget(*this, TEXT("The veto names a single cell"), Tool, /*bCanPlace*/ true);
	Tool->CellBlockedQuery.Unbind();
	Tool->SetTargetCell(Board, VetoCell);
	ExpectCellTarget(*this, TEXT("Veto lifted"), Tool, /*bCanPlace*/ true);

	// ---- Pickups (the sandbox supply) through IHueInteractable ------------------------------------------------------
	AHueModulePickup* Pickup = SpawnTestActor<AHueModulePickup>(TestWorld, FVector(300.0, 900.0, 30.0));
	UTEST_NOT_NULL(TEXT("Pickup"), Pickup);
	Pickup->SetContents(EHueModuleType::Delay, 2);
	const TWeakObjectPtr<AHueModulePickup> WeakPickup(Pickup);
	TestFalse(TEXT("No interactor: the pickup stays"), IHueInteractable::Execute_Interact(Pickup, nullptr));
	TestFalse(TEXT("An interactor without a build tool takes nothing"), IHueInteractable::Execute_Interact(Pickup, Board));
	TestEqual(TEXT("Pickup untouched"), Pickup->GetCount(), 2);
	TestTrue(TEXT("The tool's owner collects the pickup"), IHueInteractable::Execute_Interact(Pickup, Player));
	Pickup = nullptr; // emptied pickups destroy themselves
	TestFalse(TEXT("Emptied pickup destroyed"), WeakPickup.IsValid());
	TestEqual(TEXT("Both delays went into the tray"), Tray->CountOf(EHueModuleType::Delay), 2);
	TestTrue(TEXT("Collecting a pickup keeps the current selection"), Tray->GetSelectedType() == EHueModuleType::Turn);

	// ---- No target: the actions do nothing -------------------------------------------------------------------------
	Tool->ClearTarget();
	ExpectTargetKind(*this, TEXT("ClearTarget"), Tool, EHueTargetKind::None);
	Tool->PrimaryAction();
	Tool->SecondaryAction();
	TestEqual(TEXT("No target: tray unchanged"), Tray->CountOf(EHueModuleType::Turn), 1);
	TestTrue(TEXT("No target: board unchanged"), Board->IsCellFree(Cell) && Board->IsCellFree(VetoCell));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHueboxBuildInstructionTrayTest, "Huebox.Build.InstructionTrayFromLayout", HUEBOX_TEST_FLAGS)
bool FHueboxBuildInstructionTrayTest::RunTest(const FString& Parameters)
{
	using namespace HueboxTests;

	FHueTestWorld TestWorld;
	UWorld* World = TestWorld.Get();
	UTEST_NOT_NULL(TEXT("Test world"), World);
	UPulseSubsystem* Sim = TestWorld.GetSim();
	UTEST_NOT_NULL(TEXT("Pulse subsystem"), Sim);
	Sim->SetVisualsEnabled(false);

	// The test world has no game mode, so the play mode is the default: Instruction.
	UTEST_TRUE(TEXT("No Huebox game mode => Instruction mode"), AHueboxGameMode::GetPlayMode(World) == EHuePlayMode::Instruction);

	// The "level" already has a sun, so the builder adds no placeholder sky / lights (irrelevant here); it still lays
	// its ground slab as on an empty map. Movable before FinishSpawning, as for any light spawned at runtime.
	{
		const FTransform SunTransform(FRotator(-50.f, 40.f, 0.f), FVector(0.0, 0.0, 800.0));
		ADirectionalLight* Sun = World->SpawnActorDeferred<ADirectionalLight>(ADirectionalLight::StaticClass(), SunTransform,
			nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		UTEST_NOT_NULL(TEXT("Sun"), Sun);
		if (ULightComponent* SunLight = Sun->GetLightComponent())
		{
			SunLight->SetMobility(EComponentMobility::Movable);
		}
		Sun->FinishSpawning(SunTransform);
	}

	// The real path: a spawned builder's BeginPlay runs BuildAll -> BuildBoard (protected) for every exercise.
	AHueTutorialBuilder* Builder = SpawnTestActor<AHueTutorialBuilder>(TestWorld);
	UTEST_NOT_NULL(TEXT("Tutorial builder"), Builder);
	const TArray<FHueBoardLayout>& Layouts = Builder->GetLayouts();
	UTEST_TRUE(TEXT("Three tutorial layouts"), Layouts.Num() >= 3);

	// Boards by layout index (FindLayoutForBoard returns an element of GetLayouts()).
	TArray<AHueBoard*> BoardOfLayout;
	BoardOfLayout.Init(nullptr, Layouts.Num());
	int32 BoardCount = 0;
	for (TActorIterator<AHueBoard> It(World); It; ++It)
	{
		AHueBoard* Board = *It;
		if (!IsValid(Board))
		{
			continue;
		}
		++BoardCount;
		const FHueBoardLayout* Layout = Builder->FindLayoutForBoard(Board);
		for (int32 Index = 0; Index < Layouts.Num(); ++Index)
		{
			if (Layout == &Layouts[Index])
			{
				BoardOfLayout[Index] = Board;
			}
		}
	}
	TestEqual(TEXT("One board per layout"), BoardCount, Layouts.Num());

	// Exercise 1 (GDD 4): Turn x2 + Colour shift x1, first slot selected so LMB places a Turn right away.
	AHueBoard* First = BoardOfLayout[0];
	UTEST_NOT_NULL(TEXT("Exercise 1 board"), First);
	UHueInventoryComponent* FirstTray = First->GetTray();
	UTEST_NOT_NULL(TEXT("Exercise 1 tray"), FirstTray);
	TestEqual(TEXT("Exercise 1 tray: Turn x2"), FirstTray->CountOf(EHueModuleType::Turn), 2);
	TestEqual(TEXT("Exercise 1 tray: Colour shift x1"), FirstTray->CountOf(EHueModuleType::ColorShift), 1);
	TestEqual(TEXT("Exercise 1 tray: first slot selected"), FirstTray->GetSelectedIndex(), 0);
	TestTrue(TEXT("Exercise 1 tray: Turn selected"), FirstTray->GetSelectedType() == EHueModuleType::Turn);

	// Every exercise: the tray holds exactly the layout's supply, nothing more (exercise 2 has none).
	const EHueModuleType ModuleTypes[] = { EHueModuleType::Turn, EHueModuleType::ColorShift, EHueModuleType::Delay, EHueModuleType::Mixer };
	for (int32 Index = 0; Index < Layouts.Num(); ++Index)
	{
		const FHueBoardLayout& Layout = Layouts[Index];
		const AHueBoard* Board = BoardOfLayout[Index];
		const UHueInventoryComponent* Tray = Board ? Board->GetTray() : nullptr;
		if (!TestNotNull(FString::Printf(TEXT("%s: board with a tray"), *Layout.Name), Tray))
		{
			continue;
		}
		TMap<EHueModuleType, int32> Supply;
		for (const FHuePickupSpec& Spec : Layout.Pickups)
		{
			Supply.FindOrAdd(Spec.Type) += Spec.Count;
		}
		for (const EHueModuleType Type : ModuleTypes)
		{
			TestEqual(FString::Printf(TEXT("%s: tray holds the layout's %s supply"), *Layout.Name, *ModuleName(Type)),
				Tray->CountOf(Type), Supply.FindRef(Type));
		}
		if (Supply.Num() == 0)
		{
			TestTrue(FString::Printf(TEXT("%s: an empty tray selects nothing"), *Layout.Name), Tray->GetSelectedType() == EHueModuleType::None);
		}
	}

	// Instruction mode: modules come from the trays only, never from the floor.
	int32 FloorPickups = 0;
	for (TActorIterator<AHueModulePickup> It(World); It; ++It)
	{
		FloorPickups += IsValid(*It) ? 1 : 0;
	}
	TestEqual(TEXT("No floor pickups in Instruction mode"), FloorPickups, 0);

	// Trays are layout data, not run state: resetting the exercise keeps them.
	First->ResetAllBlocks();
	First->ResetObjectives();
	TestEqual(TEXT("Tray survives a reset: Turn"), FirstTray->CountOf(EHueModuleType::Turn), 2);
	TestEqual(TEXT("Tray survives a reset: Colour shift"), FirstTray->CountOf(EHueModuleType::ColorShift), 1);
	return true;
}

#undef HUEBOX_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
