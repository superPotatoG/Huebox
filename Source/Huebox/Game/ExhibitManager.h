#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "ExhibitManager.generated.h"

class AHueBoard;
class UPulseSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHueRunStateChanged, EHueRunState, NewState);

/**
 * Level-wide coordinator (GDD: AExhibitManager). Owns the Build / Running / Paused / Ended / Complete state,
 * starts runs on the board the player is at, tracks ordered objectives through the board, enforces the 20 s run
 * limit and performs the full reset (pulses, buffers, queued events, goal flags, carriage) while keeping the
 * player's layout. Only one board runs at a time.
 */
UCLASS()
class HUEBOX_API AExhibitManager : public AActor
{
	GENERATED_BODY()

public:
	AExhibitManager();

	virtual void Tick(float DeltaSeconds) override;

	/** Finds the manager of the world (cached through the game mode, falls back to an actor search). */
	static AExhibitManager* Get(const UObject* WorldContext);

	// ---- Boards ---------------------------------------------------------------------------------------------
	void RegisterBoard(AHueBoard* Board);
	const TArray<TObjectPtr<AHueBoard>>& GetBoards() const { return Boards; }
	/** Board whose bounds (grown by Margin, covering its rig) contain Location; nearest such board wins. */
	AHueBoard* FindBoardNear(const FVector& Location, float Margin = 700.f) const;

	// ---- Run control ------------------------------------------------------------------------------------------
	/** Space: Build -> start a run on the board near Location; Running <-> Paused; Ended/Complete -> reset + rerun
	 *  of the active board (the board near Location only if the active one is gone). */
	void ToggleRun(const FVector& PlayerLocation);
	bool StartRun(AHueBoard* Board);
	void SetPaused(bool bPaused);
	/** R: stop and reset the attempt. Layout is kept. */
	void ResetAttempt();

	EHueRunState GetState() const { return State; }
	AHueBoard* GetActiveBoard() const { return ActiveBoard; }
	/** Placement / collection / rotation / delay edits are only allowed in Build. */
	bool IsEditingAllowed() const { return State == EHueRunState::Build; }
	double GetRunTimeSeconds() const;
	/** Why the current attempt ended (Ended state only): time limit, ran into a board, ... Empty otherwise. */
	const FString& GetEndReason() const { return EndReason; }

	// ---- Feedback -------------------------------------------------------------------------------------------
	void PushFeedback(EHueFeedbackKind Kind, const FString& Message, AHueBoard* Board = nullptr);
	const TArray<FHueFeedback>& GetRecentFeedback() const { return RecentFeedback; }

	UPROPERTY(BlueprintAssignable, Category = "Hue|Exhibit")
	FOnHueRunStateChanged OnStateChanged;

	/**
	 * When the -HueSelfTest command line switch is present, every board installs its baseline solution and runs.
	 * With -HueSelfTestSweep[=N] (default 2) each delay of each solution is then also run N steps (0.25 s) either side
	 * of its baseline, the others kept, in real physics: the log lists which settings still complete (the timing
	 * windows). Sweep runs never count as failures.
	 */
	void BeginSelfTest();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void SetState(EHueRunState NewState);
	void HandleRunComplete();
	void HandleRunTimeout();
	/** The attempt failed before the time limit (the performer ran into a stage board): Ended at once. */
	void HandleRunFailed(const FString& Reason);
	void UpdateHighlights();

	/** Bound to UPulseSubsystem::OnFeedback (dynamic delegate, removed in EndPlay). */
	UFUNCTION()
	void HandleSimFeedback(const FHueFeedback& Feedback);

	void TickSelfTest(float DeltaSeconds);

	/** All boards of the level, in progression order. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AHueBoard>> Boards;

	UPROPERTY(Transient)
	TObjectPtr<AHueBoard> ActiveBoard;

	/** Ring of the most recent feedback lines for the HUD. */
	UPROPERTY(Transient)
	TArray<FHueFeedback> RecentFeedback;

	UPROPERTY(EditAnywhere, Category = "Hue|Exhibit")
	int32 MaxFeedbackLines = 7;

	EHueRunState State = EHueRunState::Build;

	/** Cached subsystem pointer. Weak: never extend the subsystem's lifetime. */
	TWeakObjectPtr<UPulseSubsystem> Sim;

	// Self-test (graybox validation, GDD section 4/8) -------------------------------------------------------
	/** One self-test run: a board's baseline solution, optionally with one delay changed (a sweep run). */
	struct FSelfTestCase
	{
		int32 BoardIndex = INDEX_NONE;
		/** Sweep runs: the solution delay that is changed, and to what. INDEX_NONE = the baseline run. */
		FIntPoint DelayCell = FIntPoint(INDEX_NONE, INDEX_NONE);
		float DelaySeconds = 0.f;
		bool IsSweep() const { return DelayCell.X != INDEX_NONE; }
	};

	bool bSelfTest = false;
	TArray<FSelfTestCase> SelfTestCases;
	int32 SelfTestCaseIndex = INDEX_NONE;
	float SelfTestWait = 0.f;
	int32 SelfTestPassed = 0;
	int32 SelfTestFailed = 0;
	/** Sweep: "board D(x,y)" -> the delay settings that completed (and the ones tried), in run order. */
	TMap<FString, TArray<FString>> SweepWorking;
	TMap<FString, TArray<FString>> SweepTried;

private:
	/** Returns the cached subsystem, resolving it (and binding OnFeedback once) if needed. */
	UPulseSubsystem* ResolveSim();
	/** Shared by ResetAttempt and StartRun; bQuiet suppresses the "Reset" feedback line. */
	void ResetAttemptInternal(bool bQuiet);
	/** Adds a line to the HUD ring (an identical line raised moments ago is refreshed instead) and trims it. */
	void AppendFeedback(const FHueFeedback& Line);
	/** Drops destroyed boards from Boards. */
	void PruneBoards();

	void StartNextSelfTestBoard();
	void FinishSelfTestBoard(bool bPassed, const FString& Reason);
	void FinishSelfTest();
	/** "board D(x,y)" key of a sweep case. */
	FString SweepKey(const FSelfTestCase& Case) const;

	/** Manager-side run clock (same 1/30 s clamp as the simulation); fallback when the simulation is not running. */
	double RunClockSeconds = 0.0;
	/** Run time captured when an attempt completed / ended (the simulation is cleared at that point). */
	double FrozenRunTimeSeconds = 0.0;
	FString EndReason;

	/** Delayed self-test start (bound to a UObject method, cleared in EndPlay). */
	FTimerHandle SelfTestStartTimer;
	bool bSelfTestActive = false;
	/** Board currently under test. Weak: the self-test never keeps a board alive. */
	TWeakObjectPtr<AHueBoard> SelfTestBoard;

	/** -HueShot visual check (see BeginPlay). */
	void PrepareVisualCheck();
	void TakeVisualCheckShot();
	FString ShotPath;
	FTimerHandle ShotTimer;
};
