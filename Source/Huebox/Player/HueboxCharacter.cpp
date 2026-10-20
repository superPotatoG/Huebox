#include "Player/HueboxCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Game/ExhibitManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Grid/HueBoard.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Player/HueInputConfig.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"
#include "Player/HueboxPlayerController.h"
#include "TimerManager.h"

namespace HueCharacterTuning
{
	static constexpr float CapsuleRadius = 34.f;
	static constexpr float CapsuleHalfHeight = 96.f;
	static constexpr float EyeHeight = 64.f;
	static constexpr float WalkSpeed = 450.f;

	static constexpr float DropDistance = 120.f;
	static constexpr float DropHeightAboveFloor = 30.f;

	/** Respawn after a fall when the game mode offers no start spot (= the tutorial's default player spawn). */
	static const FVector FallbackRespawnLocation(300.f, 1200.f, 120.f);
	static constexpr float FallbackRespawnYaw = -90.f;

	/** Feet below this height above the board top count as standing on a cell (conservative: placed modules are
	 *  flat tiles now, but a block must never be placed into the capsule). */
	static constexpr float BlockClearance = 100.f;
}

namespace HueCharacterText
{
	static const TCHAR* ResetBeforeEditing = TEXT("Reset the run (R) before editing");
	static const TCHAR* StandingOnCell = TEXT("Step off this cell to place here");
}

AHueboxCharacter::AHueboxCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	GetCapsuleComponent()->InitCapsuleSize(HueCharacterTuning::CapsuleRadius, HueCharacterTuning::CapsuleHalfHeight);

	// First person: the body yaws with the controller, the camera also pitches.
	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.f, 0.f, HueCharacterTuning::EyeHeight));
	Camera->bUsePawnControlRotation = true;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = HueCharacterTuning::WalkSpeed;
		Movement->bOrientRotationToMovement = false;
	}

	// GDD 5: the player is the maker; no visible body.
	if (USkeletalMeshComponent* Body = GetMesh())
	{
		Body->SetHiddenInGame(true);
		Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Body->SetGenerateOverlapEvents(false);
	}

	Inventory = CreateDefaultSubobject<UHueInventoryComponent>(TEXT("Inventory"));

	// The tool creates its placement ghost on this pawn at BeginPlay (runtime components, absolute transforms).
	BuildTool = CreateDefaultSubobject<UHueBuildToolComponent>(TEXT("BuildTool"));
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void AHueboxCharacter::BeginPlay()
{
	// Components (the build tool included) begin play inside Super.
	Super::BeginPlay();

	if (BuildTool)
	{
		// Sandbox: the personal inventory pays for placements; cells under the capsule are vetoed. BindUObject is
		// weak on this pawn, and EndPlay unbinds it anyway.
		BuildTool->SetInventory(Inventory);
		BuildTool->CellBlockedReason = HueCharacterText::StandingOnCell;
		BuildTool->CellBlockedQuery.BindUObject(this, &AHueboxCharacter::IsStandingOnCell);
	}

	EnsureInputConfig();
	// The controller may already be set (e.g. a pawn placed in the level with auto-possess).
	AddMappingContext();
}

void AHueboxCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Before Super (which ends the tool's play): the tool unhighlights and destroys its own preview.
	if (BuildTool)
	{
		BuildTool->CellBlockedQuery.Unbind();
	}

	RemoveMappingContext();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AHueboxCharacter::FellOutOfWorld(const UDamageType& DmgType)
{
	// Deliberately no Super: AActor::FellOutOfWorld destroys the pawn, the game mode never restarts the player, and
	// the controller would be left without a pawn (dead input, no HUD, carried modules lost).
	FVector Location = HueCharacterTuning::FallbackRespawnLocation;
	FRotator Rotation(0.f, HueCharacterTuning::FallbackRespawnYaw, 0.f);

	UWorld* World = GetWorld();
	if (AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr)
	{
		if (const AActor* Start = GameMode->FindPlayerStart(GetController()))
		{
			Location = Start->GetActorLocation();
			Rotation = FRotator(0.f, Start->GetActorRotation().Yaw, 0.f);
		}
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}
	// Encroachment-checked teleport first (nudges out of overlaps); forced if no free spot is found.
	if (!TeleportTo(Location, Rotation, /*bIsATest*/ false, /*bNoCheck*/ false))
	{
		TeleportTo(Location, Rotation, /*bIsATest*/ false, /*bNoCheck*/ true);
	}
	if (Controller)
	{
		// The body yaws with the controller (bUseControllerRotationYaw).
		Controller->SetControlRotation(Rotation);
	}

	UE_LOG(LogHuebox, Log, TEXT("%s fell out of the world - returned to the player start"), *GetNameSafe(this));
	ShowMessage(EHueFeedbackKind::Warning, TEXT("You left the play area - back at the start"));
}

void AHueboxCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!BuildTool)
	{
		return;
	}
	if (!IsLocallyControlled())
	{
		// No crosshair for remote / AI-driven copies: no target => no ghost, no highlight.
		BuildTool->ClearTarget();
		return;
	}

	UpdateTarget();
}

// ---- Input setup --------------------------------------------------------------------------------------------

void AHueboxCharacter::EnsureInputConfig()
{
	if (!InputConfig)
	{
		// Outer = this pawn; the UPROPERTY is the GC reference that keeps actions, context and modifiers alive.
		InputConfig = NewObject<UHueInputConfig>(this);
	}
	InputConfig->BuildDefaults();
}

void AHueboxCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	EnsureInputConfig();
	AddMappingContext();
}

void AHueboxCharacter::AddMappingContext()
{
	APlayerController* PC = Cast<APlayerController>(GetController());

	// Possession moved away from the controller we mapped for: take our context off its input stack first.
	if (bMappingAdded && MappedController.Get() != PC)
	{
		RemoveMappingContext();
	}

	if (!PC || !PC->IsLocalController() || !InputConfig || !InputConfig->MappingContext)
	{
		return;
	}
	if (bMappingAdded)
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
	{
		if (!Subsystem->HasMappingContext(InputConfig->MappingContext))
		{
			Subsystem->AddMappingContext(InputConfig->MappingContext, /*Priority*/ 0);
		}
		bMappingAdded = true;
		MappedController = PC;
	}
}

void AHueboxCharacter::RemoveMappingContext()
{
	// The player's input (owned by the controller) would otherwise keep referencing a context whose outer chain
	// is this pawn after it is destroyed or unpossessed.
	APlayerController* PC = MappedController.Get();
	if (PC && InputConfig && InputConfig->MappingContext)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (Subsystem->HasMappingContext(InputConfig->MappingContext))
			{
				Subsystem->RemoveMappingContext(InputConfig->MappingContext);
			}
		}
	}

	MappedController.Reset();
	bMappingAdded = false;
}

void AHueboxCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	EnsureInputConfig();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogHuebox, Error, TEXT("%s: expected an UEnhancedInputComponent (check DefaultInputComponentClass in DefaultInput.ini)"), *GetNameSafe(this));
		return;
	}

	const UHueInputConfig* Config = InputConfig;

	// Continuous axes.
	Input->BindAction(Config->Move, ETriggerEvent::Triggered, this, &AHueboxCharacter::HandleMove);
	Input->BindAction(Config->Look, ETriggerEvent::Triggered, this, &AHueboxCharacter::HandleLook);
	Input->BindAction(Config->HotbarScroll, ETriggerEvent::Triggered, this, &AHueboxCharacter::HandleHotbarScroll);

	// One-shot buttons fire once per press.
	Input->BindAction(Config->HotbarSlot, ETriggerEvent::Started, this, &AHueboxCharacter::HandleHotbarSlot);
	Input->BindAction(Config->Interact, ETriggerEvent::Started, this, &AHueboxCharacter::HandleInteract);
	Input->BindAction(Config->RotateLeft, ETriggerEvent::Started, this, &AHueboxCharacter::HandleRotateLeft);
	Input->BindAction(Config->RotateRight, ETriggerEvent::Started, this, &AHueboxCharacter::HandleRotateRight);
	Input->BindAction(Config->DelayDown, ETriggerEvent::Started, this, &AHueboxCharacter::HandleDelayDown);
	Input->BindAction(Config->DelayUp, ETriggerEvent::Started, this, &AHueboxCharacter::HandleDelayUp);
	Input->BindAction(Config->Drop, ETriggerEvent::Started, this, &AHueboxCharacter::HandleDrop);
	Input->BindAction(Config->RunToggle, ETriggerEvent::Started, this, &AHueboxCharacter::HandleRunToggle);
	Input->BindAction(Config->ResetRun, ETriggerEvent::Started, this, &AHueboxCharacter::HandleReset);
	Input->BindAction(Config->ToggleHelp, ETriggerEvent::Started, this, &AHueboxCharacter::HandleToggleHelp);
	// PauseMenu has bTriggerWhenPaused, so this binding also works while the world is paused.
	Input->BindAction(Config->PauseMenu, ETriggerEvent::Started, this, &AHueboxCharacter::HandlePauseMenu);
}

// ---- Input handlers -----------------------------------------------------------------------------------------

void AHueboxCharacter::HandleMove(const FInputActionValue& Value)
{
	if (!Controller)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	AddMovementInput(GetActorForwardVector(), Axis.Y);
	AddMovementInput(GetActorRightVector(), Axis.X);
}

void AHueboxCharacter::HandleLook(const FInputActionValue& Value)
{
	if (!Controller)
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AHueboxCharacter::HandleInteract()
{
	if (!BuildTool)
	{
		return;
	}

	// Pickup -> collect, block -> collect, empty cell -> place (the tool refreshes its target afterwards).
	BuildTool->PrimaryAction();

	// The world under the crosshair just changed (block removed / placed, pickup gone): re-aim immediately.
	UpdateTarget();
}

void AHueboxCharacter::HandleRotateLeft()
{
	if (BuildTool)
	{
		BuildTool->RotateTargetOrPreview(-1);
	}
}

void AHueboxCharacter::HandleRotateRight()
{
	if (BuildTool)
	{
		BuildTool->RotateTargetOrPreview(+1);
	}
}

void AHueboxCharacter::HandleDelayDown()
{
	if (BuildTool)
	{
		BuildTool->AdjustTargetDelay(-1);
	}
}

void AHueboxCharacter::HandleDelayUp()
{
	if (BuildTool)
	{
		BuildTool->AdjustTargetDelay(+1);
	}
}

void AHueboxCharacter::HandleHotbarScroll(const FInputActionValue& Value)
{
	const float Axis = Value.Get<float>();
	if (!BuildTool || FMath::IsNearlyZero(Axis))
	{
		return;
	}
	// Wheel up = previous slot.
	BuildTool->CycleSelection(Axis > 0.f ? -1 : +1);
}

void AHueboxCharacter::HandleHotbarSlot(const FInputActionValue& Value)
{
	// Keys 1..6 are scaled to 1..6 by the mapping's scalar modifier.
	const int32 SlotNumber = FMath::RoundToInt(Value.Get<float>());
	if (BuildTool && SlotNumber >= 1)
	{
		BuildTool->SelectSlot(SlotNumber - 1);
	}
}

void AHueboxCharacter::HandleDrop()
{
	if (!Inventory)
	{
		return;
	}

	const EHueModuleType Type = Inventory->GetSelectedType();
	if (Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Info, TEXT("Nothing selected to drop"));
		return;
	}
	if (BuildTool && !BuildTool->IsEditingAllowed())
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::ResetBeforeEditing);
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : HueCharacterTuning::CapsuleHalfHeight;
	const FVector Ahead = GetActorLocation() + GetActorForwardVector().GetSafeNormal2D() * HueCharacterTuning::DropDistance;

	// Default: our feet height; then snap to whatever floor is below the drop point.
	FVector DropLocation = Ahead + FVector(0.f, 0.f, HueCharacterTuning::DropHeightAboveFloor - HalfHeight);
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HueDropTrace), /*bTraceComplex*/ false, this);
	const FVector TraceStart = Ahead + FVector(0.f, 0.f, HalfHeight);
	const FVector TraceEnd = Ahead - FVector(0.f, 0.f, HalfHeight + 500.f);
	if (World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Visibility, Params))
	{
		DropLocation = Hit.ImpactPoint + FVector(0.f, 0.f, HueCharacterTuning::DropHeightAboveFloor);
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The level's actor list owns the spawned pickup; we keep no reference to it.
	AHueModulePickup* Pickup = World->SpawnActor<AHueModulePickup>(AHueModulePickup::StaticClass(), DropLocation, FRotator::ZeroRotator, SpawnParams);
	if (!Pickup)
	{
		ShowMessage(EHueFeedbackKind::Error, TEXT("Could not drop the module"));
		return;
	}
	Pickup->SetContents(Type, 1);

	// Transaction: if the inventory cannot pay, the pickup must not exist (no duplication).
	if (!Inventory->RemoveFromSlot(Inventory->GetSelectedIndex(), 1))
	{
		Pickup->Destroy();
		return;
	}
	SelectNextFilledSlotIfEmpty();

	ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Dropped 1 x %s"), HueModule::DisplayName(Type)));
}

void AHueboxCharacter::HandleRunToggle()
{
	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		Manager->ToggleRun(GetActorLocation());
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Warning, TEXT("No exhibit manager in this level"));
	}
}

void AHueboxCharacter::HandleReset()
{
	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		Manager->ResetAttempt();
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Warning, TEXT("No exhibit manager in this level"));
	}
}

void AHueboxCharacter::HandleToggleHelp()
{
	bShowHelp = !bShowHelp;
}

void AHueboxCharacter::HandlePauseMenu()
{
	bPauseMenuOpen = !bPauseMenuOpen;

	if (AHueboxPlayerController* PC = Cast<AHueboxPlayerController>(GetController()))
	{
		PC->SetPauseMenuOpen(bPauseMenuOpen);
	}
	else
	{
		UGameplayStatics::SetGamePaused(this, bPauseMenuOpen);
	}
}

// ---- Targeting ----------------------------------------------------------------------------------------------

void AHueboxCharacter::UpdateTarget()
{
	if (!BuildTool)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !Camera)
	{
		BuildTool->ClearTarget();
		return;
	}

	const FVector Start = Camera->GetComponentLocation();
	const FVector End = Start + Camera->GetForwardVector() * InteractRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(HueInteractTrace), /*bTraceComplex*/ false, this);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		// Block / pickup / board floor cell classification and the placement rules are the tool's job.
		BuildTool->SetTargetFromHit(Hit);
	}
	else
	{
		BuildTool->ClearTarget();
	}
}

bool AHueboxCharacter::IsStandingOnCell(const AHueBoard* Board, FIntPoint Cell) const
{
	if (!Board || !Board->IsInside(Cell))
	{
		return false;
	}

	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : HueCharacterTuning::CapsuleRadius;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : HueCharacterTuning::CapsuleHalfHeight;

	const FVector CellCenter = Board->CellToWorld(Cell);
	const float HalfCell = Board->GetCellSize() * 0.5f;
	const FVector Location = GetActorLocation();

	// Cell rectangle grown by the capsule radius (conservative: a corner overlap also counts).
	const bool bOverlapXY = FMath::Abs(Location.X - CellCenter.X) < HalfCell + Radius
		&& FMath::Abs(Location.Y - CellCenter.Y) < HalfCell + Radius;
	const bool bOverlapZ = (Location.Z - HalfHeight) < CellCenter.Z + HueCharacterTuning::BlockClearance
		&& (Location.Z + HalfHeight) > CellCenter.Z;

	return bOverlapXY && bOverlapZ;
}

const FHueInteractionTarget& AHueboxCharacter::GetTarget() const
{
	// Plain value data (weak refs only), so a function-local static is safe at shutdown.
	static const FHueInteractionTarget NoTarget{};
	return BuildTool ? BuildTool->GetTarget() : NoTarget;
}

EHueDir AHueboxCharacter::GetPreviewFacing() const
{
	return BuildTool ? BuildTool->GetPreviewFacing() : EHueDir::East;
}

// ---- Helpers ------------------------------------------------------------------------------------------------

void AHueboxCharacter::ShowMessage(EHueFeedbackKind Kind, const FString& Message)
{
	if (BuildTool)
	{
		// The tool forwards to the exhibit feedback log (or the output log without a manager).
		BuildTool->ShowMessage(Kind, Message);
	}
	else if (!Message.IsEmpty())
	{
		UE_LOG(LogHuebox, Log, TEXT("[Player] %s"), *Message);
	}
}

void AHueboxCharacter::SelectNextFilledSlotIfEmpty()
{
	if (!Inventory || Inventory->GetSelectedType() != EHueModuleType::None)
	{
		return;
	}

	const TArray<FHueInventorySlot>& Slots = Inventory->GetSlots();
	const int32 NumSlots = Slots.Num();
	const int32 Start = Inventory->GetSelectedIndex();
	for (int32 Offset = 1; Offset < NumSlots; ++Offset)
	{
		const int32 Index = (Start + Offset) % NumSlots;
		if (!Slots[Index].IsEmpty())
		{
			Inventory->SelectSlot(Index);
			return;
		}
	}
}
