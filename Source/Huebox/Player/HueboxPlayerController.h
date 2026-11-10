#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "HueboxPlayerController.generated.h"

class UHueInputConfig;

/** Which menu the controller shows. */
UENUM()
enum class EHueMenu : uint8
{
	None,
	/** Title screen (map Lvl_MainMenu): pick a level, the sandbox, or quit. */
	Main,
	/** Esc / P in a level: the world is paused underneath. */
	Pause
};

/** What choosing a menu entry does. */
UENUM()
enum class EHueMenuAction : uint8
{
	Resume,
	RestartLevel,
	MainMenu,
	Quit,
	OpenTutorial,
	OpenExhibit,
	OpenSandbox
};

/** One menu entry (plain data; the HUD draws them, the controller runs them). */
struct FHueMenuItem
{
	FString Label;
	/** One line under the entries while it is selected. */
	FString Hint;
	EHueMenuAction Action = EHueMenuAction::Resume;
};

/**
 * Input mode follows the possessed pawn:
 *   AHueBuilderPawn (Instruction) - OS cursor visible, game-and-UI input (hover picks cells, clicks stay in game);
 *   AHueboxCharacter (Sandbox)    - game-only input, hidden cursor (screen-centre crosshair aiming).
 * Also owns the menus, the title screen and the pause menu alike: while one is open its keys (W/S, arrows, Enter,
 * Space, LMB, Esc/P) sit in a mapping context above the pawn's and consume them, the cursor shows, and the HUD draws
 * the entries (and reports which one the mouse is over). The pause menu pauses the world.
 */
UCLASS()
class HUEBOX_API AHueboxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AHueboxPlayerController();

	// ---- Menus --------------------------------------------------------------------------------------------------
	void OpenMainMenu();
	/** Pawn Esc / P: opens the pause menu (pauses the world), or closes it. */
	void TogglePauseMenu();
	void CloseMenu();

	EHueMenu GetMenu() const { return Menu; }
	bool IsMenuOpen() const { return Menu != EHueMenu::None; }
	/** Entries of the open menu (empty when none is open). */
	void GetMenuItems(TArray<FHueMenuItem>& OutItems) const;
	/** Entries of Menu: pure (unit-tested). */
	static void GetMenuItemsFor(EHueMenu InMenu, TArray<FHueMenuItem>& OutItems);
	int32 GetMenuSelection() const { return MenuSelection; }
	void SetMenuSelection(int32 Index);
	/** Moves the selection by Delta entries, wrapping. */
	void MoveMenuSelection(int32 Delta);
	/** Runs entry Index of the open menu. */
	void ActivateMenuItem(int32 Index);

	/** Applies the cursor / input mode for InPawn (called on BeginPlay and on every possession). */
	void ApplyInputModeForPawn(const APawn* InPawn);
	/** The open menu's mode if a menu is open, the pawn's otherwise. */
	void ApplyCurrentInputMode();

protected:
	/** Shows the OS cursor and gives the game viewport keyboard focus (build view and menus). */
	void ApplyCursorInputMode();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void SetupInputComponent() override;

	void HandleMenuUp();
	void HandleMenuDown();
	void HandleMenuConfirm();
	void HandleMenuClick();
	void HandleMenuBack();

	/** Opens Menu: menu keys above the pawn's, cursor and UI input; the pause menu also pauses the world. */
	void ShowMenu(EHueMenu NewMenu);
	void EnsureInputConfig();
	void SetMenuContextActive(bool bActive);

	/** Runtime-created input actions + the menu mapping context; owned here so the GC keeps them alive. */
	UPROPERTY(Transient)
	TObjectPtr<UHueInputConfig> InputConfig;

	EHueMenu Menu = EHueMenu::None;
	int32 MenuSelection = 0;
	bool bMenuContextActive = false;
};
