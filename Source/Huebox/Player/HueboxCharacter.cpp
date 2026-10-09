#include "Player/HueboxCharacter.h"

#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Game/ExhibitManager.h"
#include "Game/HueboxGameMode.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Grid/HueBoard.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Player/HueInputConfig.h"
#include "Player/HueInteractable.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"
#include "Player/HueboxPlayerController.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace HueCharacterTuning
{
	static constexpr float CapsuleRadius = 34.f;
	static constexpr float CapsuleHalfHeight = 96.f;
	static constexpr float EyeHeight = 64.f;
	static constexpr float WalkSpeed = 450.f;

	/** Preview ghost matches the module body (BodyScale 0.7 x 0.7 x 0.5, centre 25 cm above the board top). */
	static const FVector PreviewScale(0.7f, 0.7f, 0.5f);
	static constexpr float PreviewHeight = 25.f;
	static constexpr float PreviewArrowScale = 0.25f;
	static constexpr float PreviewArrowOffset = 40.f;
	static const FLinearColor PreviewValid(0.2f, 0.9f, 0.3f);
	static const FLinearColor PreviewInvalid(0.9f, 0.2f, 0.2f);

	static constexpr float DropDistance = 120.f;
	static constexpr float DropHeightAboveFloor = 30.f;

	/** Respawn after a fall when the game mode offers no start spot (= the tutorial's default player spawn). */
	static const FVector FallbackRespawnLocation(300.f, 1750.f, 120.f);
	static constexpr float FallbackRespawnYaw = -90.f;

	/** A placed block is ~50 cm tall; anything whose feet are below this above the board top would intersect it. */
	static constexpr float BlockClearance = 100.f;
}

namespace HueCharacterText
{
	static const TCHAR* SelectModule = TEXT("Select a module (1-6 / wheel)");
	static const TCHAR* ResetBeforeEditing = TEXT("Reset the run (R) before editing");
	static const TCHAR* InventoryFull = TEXT("Inventory full");
	static const TCHAR* FixedUnits = TEXT("Fixed units cannot be removed");
	static const TCHAR* StandingOnCell = TEXT("Step off this cell to place here");
	static const TCHAR* CannotPlace = TEXT("Cannot place here");
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

	// Constructor-only asset lookups.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	// Preview components live on the pawn (so they are owned and destroyed with it) but use absolute transforms,
	// so they stay on the targeted cell instead of following the camera.
	auto SetupPreviewComponent = [](UStaticMeshComponent* Component, UStaticMesh* MeshAsset, UMaterialInterface* Material, const FVector& Scale)
	{
		Component->SetUsingAbsoluteLocation(true);
		Component->SetUsingAbsoluteRotation(true);
		Component->SetUsingAbsoluteScale(true);
		Component->SetRelativeScale3D(Scale);
		if (MeshAsset)
		{
			Component->SetStaticMesh(MeshAsset);
		}
		if (Material)
		{
			Component->SetMaterial(0, Material);
		}
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		Component->SetCastShadow(false);
		Component->SetVisibility(false);
	};

	PreviewMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PreviewMesh"));
	PreviewMesh->SetupAttachment(GetCapsuleComponent());
	SetupPreviewComponent(PreviewMesh, CubeFinder.Object, MaterialFinder.Object, HueCharacterTuning::PreviewScale);

	PreviewArrow = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PreviewArrow"));
	PreviewArrow->SetupAttachment(GetCapsuleComponent());
	SetupPreviewComponent(PreviewArrow, ConeFinder.Object, MaterialFinder.Object, FVector(HueCharacterTuning::PreviewArrowScale));
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void AHueboxCharacter::BeginPlay()
{
	Super::BeginPlay();

	UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial();
	if (!BaseMaterial && PreviewMesh)
	{
		BaseMaterial = PreviewMesh->GetMaterial(0);
	}
	if (BaseMaterial && PreviewMesh && PreviewArrow)
	{
		// One MID shared by ghost and arrow; the Transient UPROPERTY keeps it referenced.
		PreviewMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		PreviewMesh->SetMaterial(0, PreviewMID);
		PreviewArrow->SetMaterial(0, PreviewMID);
	}
	PreviewColorState = -1;

	EnsureInputConfig();
	// The controller may already be set (e.g. a pawn placed in the level with auto-possess).
	AddMappingContext();
}

void AHueboxCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Only touch the block when it outlives us; on level teardown every actor is going away anyway.
	if (EndPlayReason == EEndPlayReason::Destroyed || EndPlayReason == EEndPlayReason::RemovedFromWorld)
	{
		ClearHighlight();
	}
	HighlightedBlock.Reset();
	Target = FHueInteractionTarget();

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

	if (!IsLocallyControlled())
	{
		if (PreviewMesh)
		{
			PreviewMesh->SetVisibility(false);
		}
		if (PreviewArrow)
		{
			PreviewArrow->SetVisibility(false);
		}
		return;
	}

	UpdateTarget();
	UpdatePreview();
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
	switch (Target.Kind)
	{
	case EHueTargetKind::Pickup:
		if (AHueModulePickup* Pickup = Target.Pickup.Get())
		{
			IHueInteractable::Execute_Interact(Pickup, this);
		}
		break;

	case EHueTargetKind::Block:
		if (APulseBlock* Block = Target.Block.Get())
		{
			IHueInteractable::Execute_Interact(Block, this);
		}
		break;

	case EHueTargetKind::Cell:
		TryPlaceSelected();
		break;

	case EHueTargetKind::None:
	default:
		break;
	}

	// The world under the crosshair just changed (block removed / placed, pickup gone): refresh immediately.
	UpdateTarget();
	UpdatePreview();
}

void AHueboxCharacter::HandleRotateLeft()
{
	RotateTargetOrPreview(-1);
}

void AHueboxCharacter::HandleRotateRight()
{
	RotateTargetOrPreview(+1);
}

void AHueboxCharacter::HandleDelayDown()
{
	AdjustTargetDelay(-1);
}

void AHueboxCharacter::HandleDelayUp()
{
	AdjustTargetDelay(+1);
}

void AHueboxCharacter::HandleHotbarScroll(const FInputActionValue& Value)
{
	const float Axis = Value.Get<float>();
	if (!Inventory || FMath::IsNearlyZero(Axis))
	{
		return;
	}
	// Wheel up = previous slot.
	Inventory->CycleSelection(Axis > 0.f ? -1 : +1);
}

void AHueboxCharacter::HandleHotbarSlot(const FInputActionValue& Value)
{
	// Keys 1..6 are scaled to 1..6 by the mapping's scalar modifier.
	const int32 SlotNumber = FMath::RoundToInt(Value.Get<float>());
	if (Inventory && SlotNumber >= 1)
	{
		Inventory->SelectSlot(SlotNumber - 1);
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
	if (!IsEditingAllowed())
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

// ---- Targeting & preview ------------------------------------------------------------------------------------

void AHueboxCharacter::UpdateTarget()
{
	FHueInteractionTarget NewTarget;

	UWorld* World = GetWorld();
	if (World && Camera)
	{
		const FVector Start = Camera->GetComponentLocation();
		const FVector End = Start + Camera->GetForwardVector() * InteractRange;

		FCollisionQueryParams Params(SCENE_QUERY_STAT(HueInteractTrace), /*bTraceComplex*/ false, this);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			AActor* HitActor = Hit.GetActor();

			if (APulseBlock* Block = Cast<APulseBlock>(HitActor))
			{
				NewTarget.Kind = EHueTargetKind::Block;
				NewTarget.Block = Block;
				NewTarget.Board = Block->GetBoard();
				NewTarget.Cell = Block->GetCell();
			}
			else if (AHueModulePickup* Pickup = Cast<AHueModulePickup>(HitActor))
			{
				NewTarget.Kind = EHueTargetKind::Pickup;
				NewTarget.Pickup = Pickup;
			}
			else if (AHueBoard* Board = Cast<AHueBoard>(HitActor))
			{
				FIntPoint Cell;
				if (Board->WorldToCell(Hit.ImpactPoint, Cell))
				{
					NewTarget.Board = Board;
					NewTarget.Cell = Cell;

					if (APulseBlock* Occupant = Board->GetBlockAt(Cell))
					{
						// Aiming at the floor beside a block's body still targets that block.
						NewTarget.Kind = EHueTargetKind::Block;
						NewTarget.Block = Occupant;
					}
					else
					{
						NewTarget.Kind = EHueTargetKind::Cell;

						const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
						if (Selected == EHueModuleType::None)
						{
							NewTarget.Reason = HueCharacterText::SelectModule;
						}
						else if (!Board->CanPlaceAt(Cell, &NewTarget.Reason))
						{
							if (NewTarget.Reason.IsEmpty())
							{
								NewTarget.Reason = HueCharacterText::CannotPlace;
							}
						}
						else if (IsStandingOnCell(Board, Cell))
						{
							NewTarget.Reason = HueCharacterText::StandingOnCell;
						}
						else
						{
							NewTarget.bCanPlace = true;
						}
					}
				}
			}
		}
	}

	// Highlight follows the block target. Both sides are weak: either block may be gone by now.
	APulseBlock* NewHighlight = (NewTarget.Kind == EHueTargetKind::Block) ? NewTarget.Block.Get() : nullptr;
	APulseBlock* OldHighlight = HighlightedBlock.Get();
	if (NewHighlight != OldHighlight)
	{
		if (OldHighlight)
		{
			OldHighlight->SetHighlighted(false);
		}
		if (NewHighlight)
		{
			NewHighlight->SetHighlighted(true);
		}
		HighlightedBlock = NewHighlight;
	}

	Target = MoveTemp(NewTarget);
}

void AHueboxCharacter::UpdatePreview()
{
	if (!PreviewMesh || !PreviewArrow)
	{
		return;
	}

	const EHueModuleType Selected = Inventory ? Inventory->GetSelectedType() : EHueModuleType::None;
	const AHueBoard* Board = Target.Board.Get();
	const bool bShow = Target.Kind == EHueTargetKind::Cell && Board != nullptr && Selected != EHueModuleType::None;

	PreviewMesh->SetVisibility(bShow);
	PreviewArrow->SetVisibility(bShow);
	if (!bShow)
	{
		return;
	}

	// Boards are unrotated, so board-local directions are world directions.
	const FVector CellCenter = Board->CellToWorld(Target.Cell);
	const FVector Facing = HueDir::ToVector(PreviewFacing);
	const FVector Lift(0.f, 0.f, HueCharacterTuning::PreviewHeight);

	PreviewMesh->SetWorldLocationAndRotation(CellCenter + Lift, FRotator(0.f, HueDir::ToYaw(PreviewFacing), 0.f));
	// Cone points along its local +Z.
	PreviewArrow->SetWorldLocationAndRotation(CellCenter + Facing * HueCharacterTuning::PreviewArrowOffset + Lift,
		FRotationMatrix::MakeFromZ(Facing).Rotator());

	const int8 NewColorState = Target.bCanPlace ? 1 : 0;
	if (PreviewMID && NewColorState != PreviewColorState)
	{
		PreviewMID->SetVectorParameterValue(TEXT("Color"),
			Target.bCanPlace ? HueCharacterTuning::PreviewValid : HueCharacterTuning::PreviewInvalid);
		PreviewColorState = NewColorState;
	}
}

void AHueboxCharacter::ClearHighlight()
{
	if (APulseBlock* Block = HighlightedBlock.Get())
	{
		Block->SetHighlighted(false);
	}
	HighlightedBlock.Reset();
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

// ---- Editing actions ----------------------------------------------------------------------------------------

void AHueboxCharacter::RotateTargetOrPreview(int32 Steps)
{
	if (Target.Kind == EHueTargetKind::Block)
	{
		if (APulseBlock* Block = Target.Block.Get())
		{
			if (!Block->IsRemovable() || !Block->CanRotate())
			{
				ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("%s is fixed and cannot be rotated"), *Block->GetDisplayName()));
				return;
			}
			if (!IsEditingAllowed())
			{
				ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::ResetBeforeEditing);
				return;
			}
			Block->RotateBy(Steps);
			return;
		}
	}

	// Nothing rotatable under the crosshair: rotate the placement preview instead.
	PreviewFacing = HueDir::Rotate(PreviewFacing, Steps);
}

void AHueboxCharacter::AdjustTargetDelay(int32 Steps)
{
	AHueDelayBlock* Delay = (Target.Kind == EHueTargetKind::Block) ? Cast<AHueDelayBlock>(Target.Block.Get()) : nullptr;
	if (!Delay)
	{
		ShowMessage(EHueFeedbackKind::Info, TEXT("Aim at a delay block to change its delay (Z / X)"));
		return;
	}
	if (!IsEditingAllowed())
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::ResetBeforeEditing);
		return;
	}

	const float Before = Delay->GetDelaySeconds();
	Delay->AdjustDelay(Steps);
	const float After = Delay->GetDelaySeconds();

	if (FMath::IsNearlyEqual(Before, After))
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Delay %.2f s (%s)"), After,
			After <= AHueDelayBlock::MinDelay + UE_KINDA_SMALL_NUMBER ? TEXT("minimum") : TEXT("maximum")));
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Delay %.2f s"), After));
	}
}

bool AHueboxCharacter::TryPlaceSelected()
{
	AHueBoard* Board = Target.Board.Get();
	if (Target.Kind != EHueTargetKind::Cell || !Board || !Inventory)
	{
		return false;
	}

	const EHueModuleType Type = Inventory->GetSelectedType();
	if (Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Info, HueCharacterText::SelectModule);
		return false;
	}

	// Re-validate at click time: the run state or occupancy may have changed since the last tick.
	FString Reason;
	if (!Board->CanPlaceAt(Target.Cell, &Reason))
	{
		ShowMessage(EHueFeedbackKind::Warning, Reason.IsEmpty() ? FString(HueCharacterText::CannotPlace) : Reason);
		return false;
	}
	if (IsStandingOnCell(Board, Target.Cell))
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::StandingOnCell);
		return false;
	}

	const TSubclassOf<APulseBlock> BlockClass = AHueboxGameMode::ResolveModuleClass(this, Type);
	if (!BlockClass)
	{
		ShowMessage(EHueFeedbackKind::Error, FString::Printf(TEXT("No block class for %s"), HueModule::DisplayName(Type)));
		return false;
	}

	const int32 SlotIndex = Inventory->GetSelectedIndex();
	APulseBlock* Block = Board->SpawnBlock(BlockClass, Target.Cell, PreviewFacing, /*bRemovable*/ true);
	if (!Block)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::CannotPlace);
		return false;
	}

	// Transaction: the inventory pays for the block that now exists; if it cannot, undo the placement.
	if (!Inventory->RemoveFromSlot(SlotIndex, 1))
	{
		Board->RemoveBlock(Block);
		return false;
	}
	SelectNextFilledSlotIfEmpty();
	return true;
}

bool AHueboxCharacter::TryCollectBlock(APulseBlock* Block)
{
	if (!IsValid(Block))
	{
		return false;
	}

	const EHueModuleType Type = Block->GetModuleType();
	if (!Block->IsRemovable() || Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::FixedUnits);
		return false;
	}
	if (!IsEditingAllowed())
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::ResetBeforeEditing);
		return false;
	}
	if (!Inventory || !Inventory->CanAdd(Type, 1))
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::InventoryFull);
		return false;
	}

	const FString Name = Block->GetDisplayName();

	if (HighlightedBlock.Get() == Block)
	{
		HighlightedBlock.Reset();
	}

	// The board owns the block: let it unregister and destroy it. A stray block without a board is destroyed directly.
	bool bRemoved = false;
	if (AHueBoard* Board = Block->GetBoard())
	{
		bRemoved = Board->RemoveBlock(Block);
	}
	else
	{
		bRemoved = Block->Destroy();
	}

	if (!bRemoved)
	{
		ShowMessage(EHueFeedbackKind::Warning, FString::Printf(TEXT("%s could not be removed"), *Name));
		return false;
	}

	// CanAdd above guarantees this fits.
	const int32 Added = Inventory->AddItem(Type, 1);
	ensureMsgf(Added == 1, TEXT("Inventory refused a module after CanAdd succeeded"));

	SelectIfNothingSelected(Type);
	ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Collected %s"), *Name));
	return true;
}

int32 AHueboxCharacter::TryCollectPickup(AHueModulePickup* Pickup)
{
	if (!IsValid(Pickup) || !Inventory)
	{
		return 0;
	}

	const EHueModuleType Type = Pickup->GetModuleType();
	const int32 Count = Pickup->GetCount();

	if (Type == EHueModuleType::None)
	{
		ShowMessage(EHueFeedbackKind::Warning, TEXT("This pickup holds no module"));
		return 0;
	}
	if (Count <= 0)
	{
		Pickup->Destroy();
		return 0;
	}

	const int32 Added = Inventory->AddItem(Type, Count);
	if (Added <= 0)
	{
		ShowMessage(EHueFeedbackKind::Warning, HueCharacterText::InventoryFull);
		return 0;
	}

	const int32 Left = Count - Added;
	if (Left > 0)
	{
		Pickup->SetContents(Type, Left);
	}
	else
	{
		Pickup->Destroy();
	}

	SelectIfNothingSelected(Type);

	if (Left > 0)
	{
		ShowMessage(EHueFeedbackKind::Warning, FString::Printf(TEXT("Collected %d x %s - inventory full, %d left"), Added, HueModule::DisplayName(Type), Left));
	}
	else
	{
		ShowMessage(EHueFeedbackKind::Info, FString::Printf(TEXT("Collected %d x %s"), Added, HueModule::DisplayName(Type)));
	}
	return Added;
}

// ---- Helpers ------------------------------------------------------------------------------------------------

void AHueboxCharacter::ShowMessage(EHueFeedbackKind Kind, const FString& Message)
{
	if (Message.IsEmpty())
	{
		return;
	}

	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		// The manager logs and keeps the line for the HUD feedback list.
		Manager->PushFeedback(Kind, Message);
	}
	else
	{
		UE_LOG(LogHuebox, Log, TEXT("[Player] %s"), *Message);
	}
}

bool AHueboxCharacter::IsEditingAllowed() const
{
	const AExhibitManager* Manager = AExhibitManager::Get(this);
	return Manager == nullptr || Manager->IsEditingAllowed();
}

void AHueboxCharacter::SelectIfNothingSelected(EHueModuleType Type)
{
	if (!Inventory || Inventory->GetSelectedType() != EHueModuleType::None)
	{
		return;
	}

	const TArray<FHueInventorySlot>& Slots = Inventory->GetSlots();
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (!Slots[Index].IsEmpty() && Slots[Index].Type == Type)
		{
			Inventory->SelectSlot(Index);
			return;
		}
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
