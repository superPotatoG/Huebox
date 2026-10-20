#include "Player/HueBuilderPawn.h"

#include "Blocks/PulseBlock.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Game/ExhibitManager.h"
#include "Game/HueStageCamera.h"
#include "GameFramework/PlayerController.h"
#include "Grid/HueBoard.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Player/HueBuildToolComponent.h"
#include "Player/HueInputConfig.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueboxPlayerController.h"
#include "Rig/ReactiveRig.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// Named (not anonymous) namespace: unity builds merge several .cpp files into one translation unit.
namespace HueBuilderPrivate
{
	/** Used while the viewport size is unknown (no window yet, -nullrhi). */
	constexpr float FallbackAspect = 16.f / 9.f;

	/** Cursor outline: bar width as a fraction of the cell, bar height (cm) - above the tallest tile (barrier 10 cm). */
	constexpr float CursorEdgeFraction = 0.07f;
	constexpr float CursorEdgeHeight = 12.f;
	static const FLinearColor CursorColor(1.f, 0.7f, 0.1f);
	const TCHAR* const FlatMaterialPath = TEXT("/Game/Huebox/Materials/M_HueUnlit.M_HueUnlit");

	/** Axis values beyond this count as a pressed direction (keys give exactly +-1). */
	constexpr float CursorAxisThreshold = 0.5f;
	/** Lower bound for the repeat interval so a zero tuning value cannot move the cursor every frame forever. */
	constexpr float MinRepeatInterval = 0.02f;

	/** Unlit project material (vector parameter "Color"); the lit basic-shape material if it is missing. Weak cache:
	 *  the asset is never kept alive by this static. */
	static UMaterialInterface* GetFlatMaterial()
	{
		static TWeakObjectPtr<UMaterialInterface> Cached;
		if (!Cached.IsValid())
		{
			Cached = LoadObject<UMaterialInterface>(nullptr, FlatMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		UMaterialInterface* Material = Cached.Get();
		return Material ? Material : APulseBlock::GetBasicMaterial();
	}

	static float GetViewportAspect(const APlayerController* PC)
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		if (PC)
		{
			PC->GetViewportSize(SizeX, SizeY);
		}
		return (SizeX > 0 && SizeY > 0) ? static_cast<float>(SizeX) / static_cast<float>(SizeY) : FallbackAspect;
	}

	/**
	 * Places the four bars (north, south, east, west) of the hollow cursor square inside one cell, in CursorFrame
	 * space (the frame sits at the cell centre on the board top, unscaled). The engine cube is 100 cm, centred.
	 */
	static void LayoutCursorEdges(const TArray<TObjectPtr<UStaticMeshComponent>>& Edges, float CellSize)
	{
		const float Cell = FMath::Max(CellSize, 1.f);
		const float Thickness = Cell * CursorEdgeFraction;
		const float Offset = (Cell - Thickness) * 0.5f;
		const float Z = CursorEdgeHeight * 0.5f;

		// North / south bars span the whole cell; east / west bars fit between them (no overlapping faces).
		constexpr int32 NumEdges = 4;
		const FVector Locations[NumEdges] = { FVector(0.f, Offset, Z), FVector(0.f, -Offset, Z), FVector(Offset, 0.f, Z), FVector(-Offset, 0.f, Z) };
		const FVector Sizes[NumEdges] = {
			FVector(Cell, Thickness, CursorEdgeHeight), FVector(Cell, Thickness, CursorEdgeHeight),
			FVector(Thickness, Cell - 2.f * Thickness, CursorEdgeHeight), FVector(Thickness, Cell - 2.f * Thickness, CursorEdgeHeight) };

		for (int32 Index = 0; Index < FMath::Min(Edges.Num(), NumEdges); ++Index)
		{
			if (UStaticMeshComponent* Edge = Edges[Index])
			{
				Edge->SetRelativeLocation(Locations[Index]);
				Edge->SetRelativeScale3D(Sizes[Index] / 100.f);
			}
		}
	}

	/** Cosmetic, collision-free part of the cursor outline. */
	static void SetupCursorPart(UStaticMeshComponent* Part)
	{
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetCastShadow(false);
		Part->SetVisibility(false);
	}

	/** If the selected tray slot is empty, selects the first filled slot (if any). */
	static void SelectFirstFilledSlotIfEmpty(UHueInventoryComponent* Tray)
	{
		if (!IsValid(Tray) || Tray->GetSelectedType() != EHueModuleType::None)
		{
			return;
		}
		const TArray<FHueInventorySlot>& Slots = Tray->GetSlots();
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			if (!Slots[Index].IsEmpty())
			{
				Tray->SelectSlot(Index);
				return;
			}
		}
	}

	/** -1, 0 or +1 per axis. */
	static int32 AxisSign(double Value)
	{
		return Value > CursorAxisThreshold ? 1 : (Value < -CursorAxisThreshold ? -1 : 0);
	}
}

AHueBuilderPawn::AHueBuilderPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// The view is fixed by the build camera / stage camera, never by the controller's rotation.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	SetCanBeDamaged(false);

	// No movement component and no collision: the pawn is only a camera rig + build tool.
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SceneRoot);
	// Straight down with screen right = +X and screen up = -Y. Absolute: the game mode spawns the pawn with the
	// player start's yaw, which must not turn the build view.
	Camera->SetUsingAbsoluteRotation(true);
	Camera->SetRelativeRotation(FRotator(-90.f, -90.f, 0.f));
	Camera->bUsePawnControlRotation = false;
	Camera->SetFieldOfView(CameraFov);
	// CameraFov is horizontal at every window shape, which is what ComputeTopDownHeight assumes (the project's local
	// player default keeps the vertical FOV instead).
	Camera->bOverrideAspectRatioAxisConstraint = true;
	Camera->SetAspectRatioAxisConstraint(EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV);

	BuildTool = CreateDefaultSubobject<UHueBuildToolComponent>(TEXT("BuildTool"));

	// Constructor-only asset lookup.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));

	// The frame is the outline's anchor (absolute transform at the cursor cell); the four bars below draw it.
	CursorFrame = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CursorFrame"));
	CursorFrame->SetupAttachment(SceneRoot);
	CursorFrame->SetUsingAbsoluteLocation(true);
	CursorFrame->SetUsingAbsoluteRotation(true);
	CursorFrame->SetUsingAbsoluteScale(true);
	HueBuilderPrivate::SetupCursorPart(CursorFrame);

	const TCHAR* const EdgeNames[] = { TEXT("CursorEdgeNorth"), TEXT("CursorEdgeSouth"), TEXT("CursorEdgeEast"), TEXT("CursorEdgeWest") };
	for (const TCHAR* EdgeName : EdgeNames)
	{
		UStaticMeshComponent* Edge = CreateDefaultSubobject<UStaticMeshComponent>(EdgeName);
		Edge->SetupAttachment(CursorFrame);
		if (CubeFinder.Succeeded())
		{
			Edge->SetStaticMesh(CubeFinder.Object);
		}
		HueBuilderPrivate::SetupCursorPart(Edge);
		CursorEdges.Add(Edge);
	}
	HueBuilderPrivate::LayoutCursorEdges(CursorEdges, 100.f);
	CursorLayoutCellSize = 100.f;
}

// ---- Lifecycle ----------------------------------------------------------------------------------------------

void AHueBuilderPawn::BeginPlay()
{
	Super::BeginPlay();

	SetActorRotation(FRotator::ZeroRotator);
	if (Camera)
	{
		Camera->SetFieldOfView(CameraFov);
	}

	// One MID shared by the four bars; the Transient UPROPERTY keeps it referenced.
	if (UMaterialInterface* BaseMaterial = HueBuilderPrivate::GetFlatMaterial())
	{
		CursorMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		CursorMID->SetVectorParameterValue(TEXT("Color"), HueBuilderPrivate::CursorColor);
		for (UStaticMeshComponent* Edge : CursorEdges)
		{
			if (Edge)
			{
				Edge->SetMaterial(0, CursorMID);
			}
		}
	}
	if (CursorFrame)
	{
		CursorFrame->SetVisibility(false, /*bPropagateToChildren*/ true);
	}

	// Side camera of the Watch view. Owned by this pawn: strong UPROPERTY, destroyed in EndPlay.
	if (UWorld* World = GetWorld())
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		StageCamera = World->SpawnActor<AHueStageCamera>(AHueStageCamera::StaticClass(), GetActorTransform(), Params);
		if (!StageCamera)
		{
			UE_LOG(LogHuebox, Error, TEXT("%s: could not spawn the stage camera - the Watch view keeps the top-down camera"), *GetNameSafe(this));
		}
	}

	EnsureInputConfig();
	// The controller may already be set (possession happens before BeginPlay).
	ApplyMappingContext();

	// Manager and boards are usually spawned after this pawn began play; Tick keeps retrying.
	TryLateInitialize();
}

void AHueBuilderPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AExhibitManager* Manager = BoundManager.Get())
	{
		Manager->OnStateChanged.RemoveDynamic(this, &AHueBuilderPawn::HandleExhibitStateChanged);
	}
	BoundManager.Reset();

	RemoveMappingContexts();

	if (IsValid(StageCamera))
	{
		// Leaving a world that keeps playing: take the owned camera with us. On level teardown / PIE end the world
		// destroys every actor itself.
		if (EndPlayReason == EEndPlayReason::Destroyed || EndPlayReason == EEndPlayReason::RemovedFromWorld)
		{
			StageCamera->Destroy();
		}
	}
	StageCamera = nullptr;

	FocusedBoard.Reset();
	HeldCursorDirection = FIntPoint::ZeroValue;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AHueBuilderPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!BoundManager.IsValid() || !FocusedBoard.IsValid())
	{
		TryLateInitialize();
	}

	if (FocusedBoard.IsValid())
	{
		// The goal also follows the window shape (resizes); the pawn (= camera) glides towards it.
		UpdateCameraGoal(/*bSnap*/ false);
		const FVector Current = GetActorLocation();
		if (!Current.Equals(CameraGoal, 0.01))
		{
			SetActorLocation(FMath::VInterpTo(Current, CameraGoal, DeltaSeconds, CameraInterpSpeed));
		}
	}

	if (ViewMode == EHueViewMode::Build && IsLocallyControlled())
	{
		UpdateCursorFromMouse();
	}
	PushToolTarget();
	UpdateCursorVisual();
}

// ---- Late init & views --------------------------------------------------------------------------------------

void AHueBuilderPawn::TryLateInitialize()
{
	AExhibitManager* Manager = BoundManager.Get();
	if (!Manager)
	{
		Manager = AExhibitManager::Get(this);
		if (!Manager)
		{
			return;
		}
		// Unique: a re-bind after the weak pointer was reset must never double the callback.
		Manager->OnStateChanged.AddUniqueDynamic(this, &AHueBuilderPawn::HandleExhibitStateChanged);
		BoundManager = Manager;

		// A run may already be going when the manager is found: show the matching view at once.
		const EHueViewMode Wanted = Manager->GetState() == EHueRunState::Build ? EHueViewMode::Build : EHueViewMode::Watch;
		if (Wanted != ViewMode)
		{
			EnterViewMode(Wanted, /*bInstant*/ true);
		}
	}

	if (!FocusedBoard.IsValid())
	{
		// The next exercise (highlighted by the manager) first, otherwise the first board in progression order.
		AHueBoard* First = nullptr;
		AHueBoard* FirstHighlighted = nullptr;
		for (const TObjectPtr<AHueBoard>& Board : Manager->GetBoards())
		{
			if (!IsValid(Board))
			{
				continue;
			}
			if (!First)
			{
				First = Board;
			}
			if (!FirstHighlighted && Board->IsHighlighted())
			{
				FirstHighlighted = Board;
			}
		}
		if (AHueBoard* Pick = FirstHighlighted ? FirstHighlighted : First)
		{
			FocusBoard(Pick, /*bSnap*/ true);
		}
	}
}

void AHueBuilderPawn::HandleExhibitStateChanged(EHueRunState NewState)
{
	// Build -> top-down view; Running / Paused / Ended / Complete -> stage view. Running <-> Paused keeps the view.
	const EHueViewMode Wanted = (NewState == EHueRunState::Build) ? EHueViewMode::Build : EHueViewMode::Watch;
	if (Wanted != ViewMode)
	{
		EnterViewMode(Wanted, /*bInstant*/ false);
	}
}

void AHueBuilderPawn::EnterViewMode(EHueViewMode NewMode, bool bInstant)
{
	ViewMode = NewMode;

	APlayerController* PC = Cast<APlayerController>(GetController());
	const float BlendTime = bInstant ? 0.f : FMath::Max(ViewBlendTime, 0.f);

	if (NewMode == EHueViewMode::Watch)
	{
		AExhibitManager* Manager = BoundManager.IsValid() ? BoundManager.Get() : AExhibitManager::Get(this);
		AHueBoard* Board = Manager ? Manager->GetActiveBoard() : nullptr;
		if (!IsValid(Board))
		{
			Board = FocusedBoard.Get();
		}
		AReactiveRig* Rig = IsValid(Board) ? Board->GetRig() : nullptr;

		if (IsValid(StageCamera) && IsValid(Rig))
		{
			// Rerun from Complete / Ended passes through Build within the same call: the stage camera is still the
			// current view target then, so it keeps following smoothly instead of jumping back to the start.
			// ViewTarget, not GetViewTarget(): the latter returns the pending target (this pawn) while a blend runs.
			const APlayerCameraManager* CameraManager = PC ? PC->PlayerCameraManager.Get() : nullptr;
			const bool bAlreadyWatching = CameraManager && CameraManager->ViewTarget.Target == StageCamera.Get()
				&& StageCamera->GetRig() == Rig;
			StageCamera->SetRig(Rig, /*bSnap*/ !bAlreadyWatching);
			if (PC)
			{
				// bLockOutgoing: a reversal during a running blend then blends on from the half-blended pose
				// (without it the camera manager aborts the blend and cuts).
				PC->SetViewTargetWithBlend(StageCamera.Get(), BlendTime, VTBlend_Cubic, 0.f, /*bLockOutgoing*/ true);
			}
		}
		else
		{
			UE_LOG(LogHuebox, Warning, TEXT("%s: no stage to watch (board %s, rig %s) - keeping the top-down view"),
				*GetNameSafe(this), *GetNameSafe(Board), *GetNameSafe(Rig));
		}
	}
	else if (PC)
	{
		PC->SetViewTargetWithBlend(this, BlendTime, VTBlend_Cubic, 0.f, /*bLockOutgoing*/ true);
	}

	// A cursor key held across the switch would never send Completed once its mapping is gone.
	HeldCursorDirection = FIntPoint::ZeroValue;
	HeldCursorTime = 0.f;
	NextCursorRepeat = 0.f;

	ApplyMappingContext();
	PushToolTarget();
	UpdateCursorVisual();
}

// ---- Boards -------------------------------------------------------------------------------------------------

void AHueBuilderPawn::FocusBoard(AHueBoard* Board, bool bSnap)
{
	if (!IsValid(Board))
	{
		return;
	}

	const bool bChanged = FocusedBoard.Get() != Board;
	FocusedBoard = Board;

	// The focused board's tray pays for placements and receives collected blocks (weak inside the tool).
	UHueInventoryComponent* Tray = Board->GetTray();
	if (BuildTool)
	{
		BuildTool->SetInventory(Tray);
	}
	HueBuilderPrivate::SelectFirstFilledSlotIfEmpty(Tray);

	CursorCell = ClampCursor(CursorCell, Board->GetGridSize());

	// The top-down camera is off screen in the Watch view: jump, so the blend back starts from the right spot.
	UpdateCameraGoal(bSnap || ViewMode == EHueViewMode::Watch);

	PushToolTarget();
	UpdateCursorVisual();

	if (bChanged)
	{
		UE_LOG(LogHuebox, Log, TEXT("%s: focused %s"), *GetNameSafe(this), *Board->GetBoardName());
	}
}

void AHueBuilderPawn::FocusBoardIndex(int32 Index, bool bSnap)
{
	const AExhibitManager* Manager = BoundManager.IsValid() ? BoundManager.Get() : AExhibitManager::Get(this);
	if (!Manager)
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: FocusBoardIndex(%d) without an exhibit manager"), *GetNameSafe(this), Index);
		return;
	}

	const TArray<TObjectPtr<AHueBoard>>& Boards = Manager->GetBoards();
	if (!Boards.IsValidIndex(Index) || !IsValid(Boards[Index]))
	{
		UE_LOG(LogHuebox, Warning, TEXT("%s: FocusBoardIndex(%d) - only %d board(s)"), *GetNameSafe(this), Index, Boards.Num());
		return;
	}
	FocusBoard(Boards[Index].Get(), bSnap);
}

void AHueBuilderPawn::FocusNextBoard(int32 Delta)
{
	const AExhibitManager* Manager = BoundManager.IsValid() ? BoundManager.Get() : AExhibitManager::Get(this);
	if (!Manager)
	{
		if (BuildTool)
		{
			BuildTool->ShowMessage(EHueFeedbackKind::Warning, TEXT("No exhibit manager in this level"));
		}
		return;
	}

	// A finished attempt (Complete / Ended) may be left: the manager releases it once another board is focused.
	const EHueRunState State = Manager->GetState();
	if (State == EHueRunState::Running || State == EHueRunState::Paused)
	{
		if (BuildTool)
		{
			BuildTool->ShowMessage(EHueFeedbackKind::Warning, TEXT("Press R to stop the run first"));
		}
		return;
	}

	const TArray<TObjectPtr<AHueBoard>>& Boards = Manager->GetBoards();
	const int32 Num = Boards.Num();
	if (Num == 0 || Delta == 0)
	{
		return;
	}

	int32 Current = INDEX_NONE;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (Boards[Index].Get() == FocusedBoard.Get())
		{
			Current = Index;
			break;
		}
	}

	// Wrapping walk in Delta's direction, skipping destroyed boards. Without a focus the first step lands on the
	// first (next) or last (previous) board.
	const int32 Step = Delta > 0 ? 1 : -1;
	int32 Index = (Current != INDEX_NONE) ? Current : (Step > 0 ? Num - 1 : 0);
	for (int32 Tries = 0; Tries < Num; ++Tries)
	{
		Index = (Index + Step + Num) % Num;
		if (IsValid(Boards[Index]))
		{
			if (Boards[Index].Get() != FocusedBoard.Get())
			{
				FocusBoard(Boards[Index].Get(), /*bSnap*/ false);
			}
			return;
		}
	}
}

bool AHueBuilderPawn::HasCursor() const
{
	return ViewMode == EHueViewMode::Build && FocusedBoard.IsValid();
}

// ---- Camera & cursor ----------------------------------------------------------------------------------------

float AHueBuilderPawn::ComputeTopDownHeight(const FVector2D& AreaSize, float HorizontalFovDegrees, float AspectRatio)
{
	const double HalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(HorizontalFovDegrees), 1.0, 170.0) * 0.5);
	const double Aspect = AspectRatio > UE_KINDA_SMALL_NUMBER ? static_cast<double>(AspectRatio) : static_cast<double>(HueBuilderPrivate::FallbackAspect);

	// Looking straight down, the visible ground rectangle at height H is 2 H tan(hfov/2) wide and 2 H tan(vfov/2)
	// tall, with tan(vfov/2) = tan(hfov/2) / aspect. The area fits once both extents do.
	const double TanHalfHorizontal = FMath::Tan(HalfFovRadians);
	const double TanHalfVertical = TanHalfHorizontal / Aspect;
	const double ForWidth = FMath::Max(AreaSize.X, 0.0) / (2.0 * TanHalfHorizontal);
	const double ForHeight = FMath::Max(AreaSize.Y, 0.0) / (2.0 * TanHalfVertical);
	return static_cast<float>(FMath::Max(ForWidth, ForHeight));
}

FIntPoint AHueBuilderPawn::ClampCursor(FIntPoint Cell, FIntPoint GridSize)
{
	return FIntPoint(
		FMath::Clamp(Cell.X, 0, FMath::Max(GridSize.X - 1, 0)),
		FMath::Clamp(Cell.Y, 0, FMath::Max(GridSize.Y - 1, 0)));
}

void AHueBuilderPawn::UpdateCameraGoal(bool bSnap)
{
	const AHueBoard* Board = FocusedBoard.Get();
	if (!IsValid(Board))
	{
		return;
	}

	// Screen X = world X, screen Y = world Y, so the grid (plus margin) is fitted per axis.
	const FVector Center = Board->GetWorldBounds().GetCenter();
	const double BoardTopZ = Board->CellToWorld(FIntPoint::ZeroValue).Z;
	const FIntPoint Grid = Board->GetGridSize();
	const float Margin = FMath::Max(CameraMargin, 0.f);
	const FVector2D Area(Grid.X * Board->GetCellSize() + 2.f * Margin, Grid.Y * Board->GetCellSize() + 2.f * Margin);
	const float Aspect = HueBuilderPrivate::GetViewportAspect(Cast<APlayerController>(GetController()));

	// XY exactly above the grid centre: AExhibitManager::FindBoardNear(pawn location) then finds this board.
	CameraGoal = FVector(Center.X, Center.Y, BoardTopZ + ComputeTopDownHeight(Area, CameraFov, Aspect));

	if (bSnap)
	{
		SetActorLocation(CameraGoal);
	}
}

void AHueBuilderPawn::UpdateCursorFromMouse()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	const AHueBoard* Board = FocusedBoard.Get();
	if (!PC || !IsValid(Board))
	{
		return;
	}

	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!PC->GetMousePosition(MouseX, MouseY))
	{
		return; // no mouse over the viewport
	}

	// Only a moving mouse takes the cursor over; a keyboard move keeps it until the mouse moves again.
	const FVector2D Mouse(MouseX, MouseY);
	if (Mouse.Equals(LastMousePosition, 0.5))
	{
		return;
	}

	// While the view still blends in, the projection is not the top-down one yet: try again when it has.
	if (PC->PlayerCameraManager && PC->PlayerCameraManager->PendingViewTarget.Target)
	{
		return;
	}
	LastMousePosition = Mouse;

	FVector RayOrigin;
	FVector RayDirection;
	if (!PC->DeprojectMousePositionToWorld(RayOrigin, RayDirection) || FMath::Abs(RayDirection.Z) < UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Intersect with the board's top plane; a hit outside the grid leaves the cursor where it is (clamped grid).
	const double PlaneZ = Board->CellToWorld(FIntPoint::ZeroValue).Z;
	const double Distance = (PlaneZ - RayOrigin.Z) / RayDirection.Z;
	if (Distance <= 0.0)
	{
		return;
	}

	FIntPoint Cell;
	if (Board->WorldToCell(RayOrigin + RayDirection * Distance, Cell))
	{
		CursorCell = Cell;
	}
}

void AHueBuilderPawn::MoveCursor(FIntPoint Delta)
{
	const AHueBoard* Board = FocusedBoard.Get();
	if (Delta == FIntPoint::ZeroValue || !IsValid(Board))
	{
		return;
	}

	CursorCell = ClampCursor(CursorCell + Delta, Board->GetGridSize());
	PushToolTarget();
	UpdateCursorVisual();
}

void AHueBuilderPawn::UpdateCursorVisual()
{
	if (!CursorFrame)
	{
		return;
	}

	const AHueBoard* Board = FocusedBoard.Get();
	const bool bVisible = HasCursor() && IsValid(Board) && IsLocallyControlled();
	if (bVisible)
	{
		const float CellSize = Board->GetCellSize();
		if (!FMath::IsNearlyEqual(CellSize, CursorLayoutCellSize))
		{
			HueBuilderPrivate::LayoutCursorEdges(CursorEdges, CellSize);
			CursorLayoutCellSize = CellSize;
		}
		CursorFrame->SetWorldLocation(Board->CellToWorld(CursorCell));
	}

	if (CursorFrame->GetVisibleFlag() != bVisible)
	{
		CursorFrame->SetVisibility(bVisible, /*bPropagateToChildren*/ true);
	}
}

void AHueBuilderPawn::PushToolTarget()
{
	if (!BuildTool)
	{
		return;
	}

	AHueBoard* Board = FocusedBoard.Get();
	if (ViewMode == EHueViewMode::Build && IsValid(Board))
	{
		BuildTool->SetTargetCell(Board, CursorCell);
	}
	else
	{
		// Watch view: no highlight, no ghost.
		BuildTool->ClearTarget();
	}
}

// ---- Input setup --------------------------------------------------------------------------------------------

void AHueBuilderPawn::EnsureInputConfig()
{
	if (!InputConfig)
	{
		// Outer = this pawn; the UPROPERTY is the GC reference that keeps actions, contexts and modifiers alive.
		InputConfig = NewObject<UHueInputConfig>(this);
	}
	InputConfig->BuildDefaults();
}

void AHueBuilderPawn::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	EnsureInputConfig();
	// Also takes our contexts off a previous controller's input stack.
	ApplyMappingContext();
}

void AHueBuilderPawn::ApplyMappingContext()
{
	APlayerController* PC = Cast<APlayerController>(GetController());

	// Possession moved away from the controller we mapped for: clean up its input stack first.
	if (MappedController.IsValid() && MappedController.Get() != PC)
	{
		RemoveMappingContexts();
	}
	if (!PC || !PC->IsLocalController())
	{
		return;
	}

	EnsureInputConfig();
	UInputMappingContext* Wanted = (ViewMode == EHueViewMode::Build) ? InputConfig->BuildContext.Get() : InputConfig->WatchContext.Get();
	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
	if (!Subsystem || !Wanted)
	{
		return;
	}

	// One Instruction context at a time: the other view's keys must not reach the shared actions. Keys held during
	// the swap (Space that started the run, R that reset it) are ignored until released (FModifyContextOptions default).
	for (const UInputMappingContext* Context : { InputConfig->BuildContext.Get(), InputConfig->WatchContext.Get() })
	{
		if (Context && Context != Wanted && Subsystem->HasMappingContext(Context))
		{
			Subsystem->RemoveMappingContext(Context);
		}
	}
	if (!Subsystem->HasMappingContext(Wanted))
	{
		Subsystem->AddMappingContext(Wanted, /*Priority*/ 0);
	}

	MappedController = PC;
	ActiveContext = Wanted;
}

void AHueBuilderPawn::RemoveMappingContexts()
{
	// The player's input (owned by the controller) would otherwise keep referencing contexts whose outer chain is
	// this pawn after it is destroyed or unpossessed.
	APlayerController* PC = MappedController.Get();
	if (PC && InputConfig)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			for (const UInputMappingContext* Context : { InputConfig->BuildContext.Get(), InputConfig->WatchContext.Get() })
			{
				if (Context && Subsystem->HasMappingContext(Context))
				{
					Subsystem->RemoveMappingContext(Context);
				}
			}
		}
	}

	MappedController.Reset();
	ActiveContext.Reset();
}

void AHueBuilderPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	EnsureInputConfig();

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogHuebox, Error, TEXT("%s: expected an UEnhancedInputComponent (check DefaultInputComponentClass in DefaultInput.ini)"), *GetNameSafe(this));
		return;
	}

	// Every action of both views is bound once; the active mapping context decides which keys reach them.
	const UHueInputConfig* Config = InputConfig;

	// Cursor: held keys repeat (Triggered every frame while held), release resets the repeat.
	Input->BindAction(Config->CursorMove, ETriggerEvent::Triggered, this, &AHueBuilderPawn::HandleCursorMove);
	Input->BindAction(Config->CursorMove, ETriggerEvent::Completed, this, &AHueBuilderPawn::HandleCursorMoveReleased);
	Input->BindAction(Config->HotbarScroll, ETriggerEvent::Triggered, this, &AHueBuilderPawn::HandleHotbarScroll);

	// One-shot buttons fire once per press.
	Input->BindAction(Config->Interact, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleInteract);
	Input->BindAction(Config->Secondary, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleSecondary);
	Input->BindAction(Config->RotateLeft, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleRotateLeft);
	Input->BindAction(Config->RotateRight, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleRotateRight);
	Input->BindAction(Config->DelayDown, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleDelayDown);
	Input->BindAction(Config->DelayUp, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleDelayUp);
	Input->BindAction(Config->HotbarSlot, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleHotbarSlot);
	Input->BindAction(Config->RunToggle, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleRunToggle);
	Input->BindAction(Config->ResetRun, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleReset);
	Input->BindAction(Config->NextBoard, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleNextBoard);
	Input->BindAction(Config->PrevBoard, ETriggerEvent::Started, this, &AHueBuilderPawn::HandlePrevBoard);
	Input->BindAction(Config->ToggleHelp, ETriggerEvent::Started, this, &AHueBuilderPawn::HandleToggleHelp);
	// PauseMenu has bTriggerWhenPaused, so this binding also works while the world is paused.
	Input->BindAction(Config->PauseMenu, ETriggerEvent::Started, this, &AHueBuilderPawn::HandlePauseMenu);
}

// ---- Input handlers -----------------------------------------------------------------------------------------

void AHueBuilderPawn::HandleCursorMove(const FInputActionValue& Value)
{
	if (!HasCursor())
	{
		return;
	}

	// Screen right = +X cell, screen up = -Y cell (the build camera looks down with screen up = world -Y).
	const FVector2D Axis = Value.Get<FVector2D>();
	const FIntPoint Direction(HueBuilderPrivate::AxisSign(Axis.X), -HueBuilderPrivate::AxisSign(Axis.Y));
	if (Direction == FIntPoint::ZeroValue)
	{
		HandleCursorMoveReleased(Value);
		return;
	}

	if (Direction != HeldCursorDirection)
	{
		// A new key: one cell at once along the newly pressed axis (a released axis does not move), then wait
		// KeyRepeatDelay before auto-repeat starts.
		const FIntPoint Step(
			Direction.X != HeldCursorDirection.X ? Direction.X : 0,
			Direction.Y != HeldCursorDirection.Y ? Direction.Y : 0);
		HeldCursorDirection = Direction;
		HeldCursorTime = 0.f;
		NextCursorRepeat = FMath::Max(KeyRepeatDelay, 0.f);
		MoveCursor(Step);
		return;
	}

	// Held: repeat every KeyRepeatInterval (at most one cell per frame; a hitch does not fire a burst).
	const UWorld* World = GetWorld();
	HeldCursorTime += World ? World->GetDeltaSeconds() : 0.f;
	if (HeldCursorTime >= NextCursorRepeat)
	{
		const float Interval = FMath::Max(KeyRepeatInterval, HueBuilderPrivate::MinRepeatInterval);
		NextCursorRepeat += Interval;
		if (NextCursorRepeat <= HeldCursorTime)
		{
			NextCursorRepeat = HeldCursorTime + Interval;
		}
		MoveCursor(Direction);
	}
}

void AHueBuilderPawn::HandleCursorMoveReleased(const FInputActionValue& /*Value*/)
{
	HeldCursorDirection = FIntPoint::ZeroValue;
	HeldCursorTime = 0.f;
	NextCursorRepeat = 0.f;
}

void AHueBuilderPawn::HandleInteract()
{
	if (!HasCursor() || !BuildTool)
	{
		return;
	}
	// Act on the cell under the mouse right now, not the one from the last tick.
	UpdateCursorFromMouse();
	PushToolTarget();
	BuildTool->PrimaryAction();
	UpdateCursorVisual();
}

void AHueBuilderPawn::HandleSecondary()
{
	if (!HasCursor() || !BuildTool)
	{
		return;
	}
	UpdateCursorFromMouse();
	PushToolTarget();
	BuildTool->SecondaryAction();
	UpdateCursorVisual();
}

void AHueBuilderPawn::HandleRotateLeft()
{
	if (HasCursor() && BuildTool)
	{
		PushToolTarget();
		BuildTool->RotateTargetOrPreview(-1);
	}
}

void AHueBuilderPawn::HandleRotateRight()
{
	if (HasCursor() && BuildTool)
	{
		PushToolTarget();
		BuildTool->RotateTargetOrPreview(+1);
	}
}

void AHueBuilderPawn::HandleDelayDown()
{
	if (HasCursor() && BuildTool)
	{
		PushToolTarget();
		BuildTool->AdjustTargetDelay(-1);
	}
}

void AHueBuilderPawn::HandleDelayUp()
{
	if (HasCursor() && BuildTool)
	{
		PushToolTarget();
		BuildTool->AdjustTargetDelay(+1);
	}
}

void AHueBuilderPawn::HandleHotbarScroll(const FInputActionValue& Value)
{
	const float Axis = Value.Get<float>();
	if (ViewMode != EHueViewMode::Build || !BuildTool || FMath::IsNearlyZero(Axis))
	{
		return;
	}
	// Wheel up = previous slot.
	BuildTool->CycleSelection(Axis > 0.f ? -1 : +1);
}

void AHueBuilderPawn::HandleHotbarSlot(const FInputActionValue& Value)
{
	if (ViewMode != EHueViewMode::Build || !BuildTool)
	{
		return;
	}
	// Keys 1..6 are scaled to 1..6 by the mapping's scalar modifier.
	const int32 SlotNumber = FMath::RoundToInt(Value.Get<float>());
	if (SlotNumber >= 1)
	{
		BuildTool->SelectSlot(SlotNumber - 1);
	}
}

void AHueBuilderPawn::HandleRunToggle()
{
	AExhibitManager* Manager = AExhibitManager::Get(this);
	if (!Manager)
	{
		if (BuildTool)
		{
			BuildTool->ShowMessage(EHueFeedbackKind::Warning, TEXT("No exhibit manager in this level"));
		}
		return;
	}

	// The focused board's centre rather than the pawn: while the camera still glides over a neighbouring board the
	// manager would otherwise pick that one.
	const AHueBoard* Board = FocusedBoard.Get();
	Manager->ToggleRun(IsValid(Board) ? Board->GetWorldBounds().GetCenter() : GetActorLocation());
}

void AHueBuilderPawn::HandleReset()
{
	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		Manager->ResetAttempt();
	}
	else if (BuildTool)
	{
		BuildTool->ShowMessage(EHueFeedbackKind::Warning, TEXT("No exhibit manager in this level"));
	}
}

void AHueBuilderPawn::HandleNextBoard()
{
	// Both views: refused while a run is Running / Paused, allowed after Complete / Ended.
	FocusNextBoard(+1);
}

void AHueBuilderPawn::HandlePrevBoard()
{
	if (ViewMode == EHueViewMode::Build)
	{
		FocusNextBoard(-1);
	}
}

void AHueBuilderPawn::HandleToggleHelp()
{
	bShowHelp = !bShowHelp;
}

void AHueBuilderPawn::HandlePauseMenu()
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
