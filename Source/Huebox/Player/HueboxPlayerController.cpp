#include "Player/HueboxPlayerController.h"

#include "Core/HueTypes.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "TimerManager.h"
#include "Widgets/SViewport.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Game/HueLevels.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Player/HueBuilderPawn.h"
#include "Player/HueInputConfig.h"
#include "UI/HueboxHUD.h"

namespace HueControllerPrivate
{
	/** Above every pawn context (they use 0), so the menu keys consume the pawn's while a menu is open. */
	constexpr int32 MenuContextPriority = 100;
}

AHueboxPlayerController::AHueboxPlayerController()
{
	// The cursor follows the possessed pawn (ApplyInputModeForPawn). Cells are picked by our own deprojection, not
	// by engine click / hover events.
	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
}

void AHueboxPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// HandleStartingNewPlayer (the title screen opens its menu there) runs before BeginPlay, so this must respect an
	// open menu: applying the pawn mode here hid the title screen's cursor (no pawn = first-person mode).
	ApplyCurrentInputMode();
	// Again next frame: Play-In-Editor sets focus / mouse capture itself once the world has started.
	GetWorldTimerManager().SetTimerForNextTick(this, &AHueboxPlayerController::ApplyCurrentInputMode);
}

void AHueboxPlayerController::ApplyCurrentInputMode()
{
	if (IsMenuOpen())
	{
		ApplyCursorInputMode();
	}
	else
	{
		ApplyInputModeForPawn(GetPawn());
	}
}

void AHueboxPlayerController::ApplyCursorInputMode()
{
	if (!IsLocalController())
	{
		return;
	}
	// Clicks are captured only while held, so LMB / RMB still reach the input actions and the cursor never vanishes
	// or gets trapped in the window.
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	// Game-and-UI only moves keyboard focus when given a widget. Without one the keys kept going wherever focus was
	// (the editor, or the previous level's menu) until the player clicked into the game.
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (LocalPlayer && LocalPlayer->ViewportClient)
	{
		InputMode.SetWidgetToFocus(LocalPlayer->ViewportClient->GetGameViewportWidget());
	}
	SetInputMode(InputMode);
	bShowMouseCursor = true;
	UE_LOG(LogHuebox, Verbose, TEXT("%s: cursor input mode (menu %d, viewport focus %d)"), *GetName(),
		static_cast<int32>(Menu), LocalPlayer && LocalPlayer->ViewportClient && LocalPlayer->ViewportClient->GetGameViewportWidget().IsValid() ? 1 : 0);
}

void AHueboxPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The local player outlives this controller across a level change: take our context off its input stack.
	SetMenuContextActive(false);
	Super::EndPlay(EndPlayReason);
}

void AHueboxPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (!IsMenuOpen())
	{
		ApplyInputModeForPawn(InPawn);
	}
}

void AHueboxPlayerController::ApplyInputModeForPawn(const APawn* InPawn)
{
	if (!IsLocalController())
	{
		return;
	}

	if (InPawn && InPawn->IsA<AHueBuilderPawn>())
	{
		// Instruction / top-down: the OS cursor hovers cells.
		ApplyCursorInputMode();
	}
	else
	{
		// Sandbox / first person: the mouse looks around, aiming uses the screen-centre crosshair.
		FInputModeGameOnly InputMode;
		InputMode.SetConsumeCaptureMouseDown(true);
		SetInputMode(InputMode);
		bShowMouseCursor = false;
	}
}

// ---- Input --------------------------------------------------------------------------------------------------

void AHueboxPlayerController::EnsureInputConfig()
{
	if (!InputConfig)
	{
		// Outer = this controller; the UPROPERTY is the GC reference that keeps actions and context alive.
		InputConfig = NewObject<UHueInputConfig>(this);
	}
	InputConfig->BuildDefaults();
}

void AHueboxPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	EnsureInputConfig();
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (!Input)
	{
		UE_LOG(LogHuebox, Error, TEXT("%s: expected an UEnhancedInputComponent (check DefaultInputComponentClass in DefaultInput.ini)"), *GetNameSafe(this));
		return;
	}

	// The actions only fire while the menu context is on the stack (a menu is open).
	Input->BindAction(InputConfig->MenuUp, ETriggerEvent::Started, this, &AHueboxPlayerController::HandleMenuUp);
	Input->BindAction(InputConfig->MenuDown, ETriggerEvent::Started, this, &AHueboxPlayerController::HandleMenuDown);
	Input->BindAction(InputConfig->MenuConfirm, ETriggerEvent::Started, this, &AHueboxPlayerController::HandleMenuConfirm);
	Input->BindAction(InputConfig->MenuClick, ETriggerEvent::Started, this, &AHueboxPlayerController::HandleMenuClick);
	Input->BindAction(InputConfig->MenuBack, ETriggerEvent::Started, this, &AHueboxPlayerController::HandleMenuBack);
}

void AHueboxPlayerController::SetMenuContextActive(bool bActive)
{
	if (bActive == bMenuContextActive || !InputConfig || !InputConfig->MenuContext)
	{
		return;
	}
	UEnhancedInputLocalPlayerSubsystem* Subsystem = GetLocalPlayer()
		? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer())
		: nullptr;
	if (!Subsystem)
	{
		return;
	}
	if (bActive)
	{
		Subsystem->AddMappingContext(InputConfig->MenuContext, HueControllerPrivate::MenuContextPriority);
	}
	else
	{
		Subsystem->RemoveMappingContext(InputConfig->MenuContext);
	}
	bMenuContextActive = bActive;
}

void AHueboxPlayerController::HandleMenuUp()
{
	MoveMenuSelection(-1);
}

void AHueboxPlayerController::HandleMenuDown()
{
	MoveMenuSelection(+1);
}

void AHueboxPlayerController::HandleMenuConfirm()
{
	ActivateMenuItem(MenuSelection);
}

void AHueboxPlayerController::HandleMenuClick()
{
	// Only a click on an entry counts (the HUD knows where it drew them).
	float MouseX = 0.f;
	float MouseY = 0.f;
	const AHueboxHUD* HUD = GetHUD<AHueboxHUD>();
	if (HUD && GetMousePosition(MouseX, MouseY))
	{
		const int32 Index = HUD->GetMenuItemAt(FVector2D(MouseX, MouseY));
		if (Index != INDEX_NONE)
		{
			ActivateMenuItem(Index);
		}
	}
}

void AHueboxPlayerController::HandleMenuBack()
{
	// Esc / P closes the pause menu; the title screen has nowhere to go back to.
	if (Menu == EHueMenu::Pause)
	{
		CloseMenu();
	}
}

// ---- Menus --------------------------------------------------------------------------------------------------

void AHueboxPlayerController::GetMenuItemsFor(EHueMenu InMenu, TArray<FHueMenuItem>& OutItems)
{
	OutItems.Reset();
	switch (InMenu)
	{
	case EHueMenu::Main:
		OutItems.Add({ TEXT("Level 1 · The Prototype Table"), TEXT("Four short exercises: routing, push and pull, a stop, a colour key."), EHueMenuAction::OpenTutorial });
		OutItems.Add({ TEXT("Level 2 · The Exhibition Box"), TEXT("The whole performance from one activation."), EHueMenuAction::OpenExhibit });
		OutItems.Add({ TEXT("Sandbox"), TEXT("Free building in first person: every block, unlimited, any skin."), EHueMenuAction::OpenSandbox });
		OutItems.Add({ TEXT("Quit"), TEXT("Close the game."), EHueMenuAction::Quit });
		break;
	case EHueMenu::Pause:
		OutItems.Add({ TEXT("Resume"), TEXT("Back to the level (Esc / P)."), EHueMenuAction::Resume });
		OutItems.Add({ TEXT("Restart level"), TEXT("Starts this level again - every block you placed is cleared."), EHueMenuAction::RestartLevel });
		OutItems.Add({ TEXT("Main menu"), TEXT("Leave this level for the title screen."), EHueMenuAction::MainMenu });
		OutItems.Add({ TEXT("Quit"), TEXT("Close the game."), EHueMenuAction::Quit });
		break;
	case EHueMenu::None:
	default:
		break;
	}
}

void AHueboxPlayerController::GetMenuItems(TArray<FHueMenuItem>& OutItems) const
{
	GetMenuItemsFor(Menu, OutItems);
}

void AHueboxPlayerController::SetMenuSelection(int32 Index)
{
	TArray<FHueMenuItem> Items;
	GetMenuItems(Items);
	if (Items.IsValidIndex(Index))
	{
		MenuSelection = Index;
	}
}

void AHueboxPlayerController::MoveMenuSelection(int32 Delta)
{
	TArray<FHueMenuItem> Items;
	GetMenuItems(Items);
	const int32 Num = Items.Num();
	if (Num > 0)
	{
		MenuSelection = ((MenuSelection + Delta) % Num + Num) % Num;
	}
}

void AHueboxPlayerController::OpenMainMenu()
{
	ShowMenu(EHueMenu::Main);
}

void AHueboxPlayerController::TogglePauseMenu()
{
	if (Menu == EHueMenu::Pause)
	{
		CloseMenu();
	}
	else if (Menu == EHueMenu::None)
	{
		ShowMenu(EHueMenu::Pause);
	}
}

void AHueboxPlayerController::ShowMenu(EHueMenu NewMenu)
{
	EnsureInputConfig();
	Menu = NewMenu;
	MenuSelection = 0;
	SetMenuContextActive(true);

	// The menu is clicked with the OS cursor, also over the first-person view (which hides it otherwise).
	ApplyCursorInputMode();

	if (NewMenu == EHueMenu::Pause && !UGameplayStatics::SetGamePaused(this, true))
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueboxPlayerController: could not pause the game"));
	}
}

void AHueboxPlayerController::CloseMenu()
{
	if (Menu == EHueMenu::None)
	{
		return;
	}
	const bool bWasPause = Menu == EHueMenu::Pause;
	Menu = EHueMenu::None;
	SetMenuContextActive(false);
	if (bWasPause)
	{
		UGameplayStatics::SetGamePaused(this, false);
	}
	ApplyInputModeForPawn(GetPawn());
}

void AHueboxPlayerController::ActivateMenuItem(int32 Index)
{
	TArray<FHueMenuItem> Items;
	GetMenuItems(Items);
	if (!Items.IsValidIndex(Index))
	{
		return;
	}
	MenuSelection = Index;

	// Leaving the level: the next world starts unpaused and without a menu (the title screen opens its own).
	auto Travel = [this](const FString& Map, const FString& Options)
	{
		CloseMenu();
		UGameplayStatics::OpenLevel(this, FName(*Map), /*bAbsolute*/ true, Options);
	};

	switch (Items[Index].Action)
	{
	case EHueMenuAction::Resume:
		CloseMenu();
		break;
	case EHueMenuAction::RestartLevel:
	{
		// Same map, same URL options (e.g. ?HueMode=Sandbox); -HueSandbox on the command line still applies.
		const UWorld* World = GetWorld();
		const FString MapName = World ? World->GetOutermost()->GetName() : FString(HueLevels::Tutorial);
		FString Options;
		if (const AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr)
		{
			// OpenLevel adds the '?' itself.
			Options = GameMode->OptionsString;
			Options.RemoveFromStart(TEXT("?"));
		}
		// The PIE package carries a UEDPIE_<n>_ prefix that OpenLevel must not see.
		Travel(UWorld::RemovePIEPrefix(MapName), Options);
		break;
	}
	case EHueMenuAction::MainMenu:
		Travel(HueLevels::MainMenu, FString());
		break;
	case EHueMenuAction::OpenTutorial:
		Travel(HueLevels::Tutorial, FString());
		break;
	case EHueMenuAction::OpenExhibit:
		Travel(HueLevels::Exhibit, FString());
		break;
	case EHueMenuAction::OpenSandbox:
		Travel(HueLevels::Sandbox, FString());
		break;
	case EHueMenuAction::Quit:
		CloseMenu();
		UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, /*bIgnorePlatformRestrictions*/ false);
		break;
	}
}
