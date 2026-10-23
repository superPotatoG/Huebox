#include "Game/HueTutorialBuilder.h"

#include "CollisionQueryParams.h"
#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"

#include "Blocks/HueFixedBlocks.h"
#include "Blocks/HueModuleBlocks.h"
#include "Blocks/PulseBlock.h"
#include "Game/ExhibitManager.h"
#include "Game/HueboxGameMode.h"
#include "Grid/HueBoard.h"
#include "Player/HueInventoryComponent.h"
#include "Player/HueModulePickup.h"
#include "Rig/ReactiveRig.h"

namespace HueTutorialPrivate
{
	/** Board i sits at (i * BoardSpacing, 0, 0) relative to the builder. */
	constexpr float BoardSpacing = 1800.f;
	/** Supply pickups float this high above the ground. */
	constexpr float PickupHeight = 30.f;
	/** Default cell size of AHueBoard (used only to probe the floor before boards exist). */
	constexpr float ProbeCellSize = 100.f;

	/** Ground slab (relative to the builder): 108 m x 50 m under the four exercises (boards at x 0 .. 6000, stages
	 *  to ~6300) and the player spawn; the west edge stays where it was for three boards. */
	constexpr float GroundCenterX = 2800.f;
	constexpr float GroundCenterY = 600.f;
	constexpr float GroundHalfX = 5400.f;
	constexpr float GroundHalfY = 2500.f;
	/** Invisible boundary walls stand this far inside the slab edges. */
	constexpr float WallInset = 100.f;
	constexpr float WallThickness = 20.f;
	constexpr float WallHeight = 400.f;
	/** KillZ this far below the slab top: a fall that still happens ends after ~2 s instead of minutes. */
	constexpr float KillDepth = 2000.f;

	// ---- Layout helpers (keep MakeDefaultTutorialLayouts readable as data) -----------------------------------

	FHueBlockSpec MakeBlock(EHueBlockKind Kind, int32 X, int32 Y, EHueDir Facing = EHueDir::East, bool bRemovable = false)
	{
		FHueBlockSpec Spec;
		Spec.Kind = Kind;
		Spec.Cell = FIntPoint(X, Y);
		Spec.Facing = Facing;
		Spec.bRemovable = bRemovable;
		return Spec;
	}

	FHueBlockSpec MakeSource(int32 X, int32 Y, EHueDir Facing, EHueColor Color = EHueColor::B)
	{
		FHueBlockSpec Spec = MakeBlock(EHueBlockKind::Source, X, Y, Facing);
		Spec.SourceColor = Color;
		return Spec;
	}

	/** Receiver facing stays East: AcceptFaces are board-local, so Facing has no rule meaning for receivers. */
	FHueBlockSpec MakeReceiver(int32 X, int32 Y, std::initializer_list<EHueDir> Faces)
	{
		FHueBlockSpec Spec = MakeBlock(EHueBlockKind::Receiver, X, Y);
		Spec.ReceiverFaces = TArray<EHueDir>(Faces);
		return Spec;
	}

	FHueBlockSpec MakeDelay(int32 X, int32 Y, EHueDir Facing, float Seconds, bool bRemovable)
	{
		FHueBlockSpec Spec = MakeBlock(EHueBlockKind::Delay, X, Y, Facing, bRemovable);
		Spec.DelaySeconds = Seconds;
		return Spec;
	}

	FHueBlockSpec MakeBrick(int32 X, int32 Y, EHueColor Color)
	{
		FHueBlockSpec Spec = MakeBlock(EHueBlockKind::Brick, X, Y);
		Spec.BrickColor = Color;
		return Spec;
	}

	FHuePickupSpec MakePickup(EHueModuleType Type, int32 Count, float LocalX, float LocalY)
	{
		FHuePickupSpec Pickup;
		Pickup.Type = Type;
		Pickup.Count = Count;
		Pickup.LocalPosition = FVector2D(LocalX, LocalY);
		return Pickup;
	}

	FHueObjective MakeObjective(EHueObjectiveType Type, const TCHAR* Description)
	{
		FHueObjective Objective;
		Objective.Type = Type;
		Objective.Description = Description;
		return Objective;
	}

	FHueBoardLayout MakeLayout(int32 Index, const TCHAR* Name, const TCHAR* Brief)
	{
		FHueBoardLayout Layout;
		Layout.Name = Name;
		Layout.Brief = Brief;
		Layout.Origin = FVector(Index * BoardSpacing, 0.f, 0.f);
		Layout.GridSize = FIntPoint(6, 6);
		// Stage just behind the far (south) edge of the grid: the viewer north of the board sees the grid in front and
		// the performance behind it in one view (the paper screen sits a further 60 cm back).
		Layout.RigOffset = FVector(-200.f, -170.f, 0.f);
		Layout.TrackLength = 1000.f;
		return Layout;
	}

	// ---- Spawning helpers -------------------------------------------------------------------------------------

	EHueModuleType ToModuleType(EHueBlockKind Kind)
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

	const TCHAR* KindName(EHueBlockKind Kind)
	{
		switch (Kind)
		{
		case EHueBlockKind::Source:     return TEXT("Source");
		case EHueBlockKind::Receiver:   return TEXT("Receiver");
		case EHueBlockKind::Barrier:    return TEXT("Barrier");
		case EHueBlockKind::Turn:       return TEXT("Turn");
		case EHueBlockKind::ColorShift: return TEXT("Colour shift");
		case EHueBlockKind::Delay:      return TEXT("Delay");
		case EHueBlockKind::Mixer:      return TEXT("Mixer");
		case EHueBlockKind::Brick:      return TEXT("Brick");
		default:                        return TEXT("Block");
		}
	}

	/** Briefs are shared by both play modes; "{Supply}" names where this mode keeps the exercise's modules. */
	FString ResolveBrief(const FString& Brief, EHuePlayMode Mode)
	{
		const TCHAR* const Supply = (Mode == EHuePlayMode::Sandbox)
			? TEXT("Collect the modules on the floor")
			: TEXT("Use the modules in this board's tray");
		return Brief.Replace(TEXT("{Supply}"), Supply, ESearchCase::CaseSensitive);
	}

	/** Instruction mode: the layout's supply goes into the board's tray (instead of floor pickups); slot 1 selected. */
	void FillTray(AHueBoard* Board, const FHueBoardLayout& Layout)
	{
		UHueInventoryComponent* Tray = IsValid(Board) ? Board->GetTray() : nullptr;
		if (!IsValid(Tray))
		{
			UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: '%s' has no tray - its supply was not handed out"), *Layout.Name);
			return;
		}

		for (const FHuePickupSpec& PickupSpec : Layout.Pickups)
		{
			if (PickupSpec.Type == EHueModuleType::None || PickupSpec.Count <= 0)
			{
				continue;
			}
			const int32 Added = Tray->AddItem(PickupSpec.Type, PickupSpec.Count);
			if (Added < PickupSpec.Count)
			{
				UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: the tray of '%s' is full - %d of %d %s not added"),
					*Layout.Name, PickupSpec.Count - Added, PickupSpec.Count, HueModule::DisplayName(PickupSpec.Type));
			}
		}
		Tray->SelectSlot(0);
	}

	template <typename ActorType>
	bool WorldHasActor(UWorld* World)
	{
		for (TActorIterator<ActorType> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				return true;
			}
		}
		return false;
	}

	/** True if level geometry already provides a floor at Location (within half a metre of its height). */
	bool HasFloorAt(const UWorld* World, const FVector& Location)
	{
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		const FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(HueGroundProbe), /*bTraceComplex*/ false);

		FHitResult Hit;
		const bool bHit = World->LineTraceSingleByObjectType(Hit, Location + FVector(0.f, 0.f, 200.f),
			Location - FVector(0.f, 0.f, 200.f), ObjectParams, QueryParams);
		return bHit && FMath::Abs(Hit.ImpactPoint.Z - Location.Z) <= 50.f;
	}

	/**
	 * Four hidden walls around the ground slab (FloorCenter = centre of its top face). Profile InvisibleWall blocks
	 * pawns but ignores the Visibility channel, so interaction and drop traces pass through. Spawned actors are added
	 * to OutSpawned (the builder's UPROPERTY list), which also destroys them with the builder.
	 */
	void SpawnBoundaryWalls(UWorld* World, AActor* Owner, const FVector& FloorCenter, TArray<TObjectPtr<AActor>>& OutSpawned)
	{
		const float HalfX = GroundHalfX - WallInset;
		const float HalfY = GroundHalfY - WallInset;
		const float Z = WallHeight * 0.5f;
		struct FWallSpec
		{
			FVector Offset; // centre, relative to FloorCenter
			FVector Size;   // cm
		};
		const FWallSpec Walls[] =
		{
			{ FVector(0.f, HalfY, Z),  FVector(2.f * HalfX + WallThickness, WallThickness, WallHeight) }, // north
			{ FVector(0.f, -HalfY, Z), FVector(2.f * HalfX + WallThickness, WallThickness, WallHeight) }, // south
			{ FVector(HalfX, 0.f, Z),  FVector(WallThickness, 2.f * HalfY + WallThickness, WallHeight) }, // east
			{ FVector(-HalfX, 0.f, Z), FVector(WallThickness, 2.f * HalfY + WallThickness, WallHeight) }, // west
		};

		for (const FWallSpec& Wall : Walls)
		{
			const FTransform WallTransform(FRotator::ZeroRotator, FloorCenter + Wall.Offset, Wall.Size / 100.f);
			AStaticMeshActor* WallActor = World->SpawnActorDeferred<AStaticMeshActor>(AStaticMeshActor::StaticClass(), WallTransform,
				Owner, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!WallActor)
			{
				continue;
			}
			// Movable first: a static component's mesh cannot be changed at runtime.
			WallActor->SetMobility(EComponentMobility::Movable);
			if (UStaticMeshComponent* WallMesh = WallActor->GetStaticMeshComponent())
			{
				WallMesh->SetStaticMesh(APulseBlock::GetCubeMesh());
				WallMesh->SetCollisionProfileName(TEXT("InvisibleWall"));
				WallMesh->SetCastShadow(false);
			}
			WallActor->SetActorHiddenInGame(true);
			WallActor->Tags.Add(TEXT("HueBoundary"));
			WallActor->FinishSpawning(WallTransform);
			OutSpawned.Add(WallActor);
		}
	}
}

// ---- Construction ---------------------------------------------------------------------------------------------

AHueTutorialBuilder::AHueTutorialBuilder()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetCanBeDamaged(false);

	// Defaults live on the CDO so designers see (and can edit) the three exercises in the details panel.
	MakeDefaultTutorialLayouts(Layouts);
}

void AHueTutorialBuilder::MakeDefaultTutorialLayouts(TArray<FHueBoardLayout>& OutLayouts)
{
	using namespace HueTutorialPrivate;

	OutLayouts.Reset();

	// Notation in comments: S Source, R Receiver, X Barrier, K Brick, T Turn, C Colour shift, D Delay, M Mixer.
	// Board-local: East = +X ("right" for the viewer), North = +Y (towards the viewer), 4 cells/s = 0.25 s per cell.

	// ---- Exercise 1: route a signal ------------------------------------------------------------------------------
	{
		FHueBoardLayout& Layout = OutLayouts.Add_GetRef(MakeLayout(0,
			TEXT("1 \u00B7 Route a Signal"),
			TEXT("The source fires one B pulse to the right, but a barrier blocks the way. {Supply}, route the pulse up ")
			TEXT("and around, and turn it into A so the dancer is pushed away from her partner.")));

		Layout.Blocks = {
			MakeSource(0, 4, EHueDir::East, EHueColor::B),
			MakeBlock(EHueBlockKind::Barrier, 3, 3),
			MakeBlock(EHueBlockKind::Barrier, 3, 4),
			MakeBlock(EHueBlockKind::Barrier, 3, 5),
			MakeReceiver(5, 1, { EHueDir::West }),
		};

		Layout.Pickups = {
			MakePickup(EHueModuleType::Turn, 2, 100.f, 720.f),
			MakePickup(EHueModuleType::ColorShift, 1, 260.f, 720.f),
		};

		FHueObjective TurnsA = MakeObjective(EHueObjectiveType::ReceiverColor, TEXT("Receiver turns A"));
		TurnsA.Color = EHueColor::A;
		FHueObjective PastHalfway = MakeObjective(EHueObjectiveType::PassFarMarker, TEXT("Pushed past the halfway marker"));
		PastHalfway.TrackFraction = 0.5f;
		Layout.Objectives = { TurnsA, PastHalfway };

		// T(1,4) West [W+S]: east-bound pulse turns south. T(1,1) East [E+N]: south-bound turns east.
		// C(2,1): B -> A. Receiver (5,1) gets A through its West face at t = 2.00 s.
		Layout.SolutionBlocks = {
			MakeBlock(EHueBlockKind::Turn, 1, 4, EHueDir::West, true),
			MakeBlock(EHueBlockKind::Turn, 1, 1, EHueDir::East, true),
			MakeBlock(EHueBlockKind::ColorShift, 2, 1, EHueDir::East, true),
		};
	}

	// ---- Exercise 2: change motion -------------------------------------------------------------------------------
	{
		FHueBoardLayout& Layout = OutLayouts.Add_GetRef(MakeLayout(1,
			TEXT("2 \u00B7 Change Motion"),
			TEXT("Two routes are ready. Route 1 turns B into A (push). Route 2 is a delayed B (pull). Try each alone by ")
			TEXT("collecting a block, then run both: point at the delay block and press Z/X so the pull arrives later. ")
			TEXT("Same-time arrivals cancel out.")));

		// Route 1: A reaches R (West face) at 1.25 s. Route 2: B reaches R (South face) at 0.75 s + delay, so the
		// default 0.5 s delay collides with the A on purpose (same-time conflict).
		Layout.Blocks = {
			MakeSource(0, 3, EHueDir::East, EHueColor::B),
			MakeBlock(EHueBlockKind::ColorShift, 2, 3, EHueDir::East, true),
			MakeReceiver(5, 3, { EHueDir::West, EHueDir::South }),
			MakeSource(5, 0, EHueDir::North, EHueColor::B),
			MakeDelay(5, 1, EHueDir::North, 0.5f, true),
		};

		FHueObjective PushedFar = MakeObjective(EHueObjectiveType::PassFarMarker, TEXT("Pushed past the far marker"));
		PushedFar.TrackFraction = 0.6f;
		FHueObjective PulledBack = MakeObjective(EHueObjectiveType::ReturnBelow, TEXT("Pulled back past the return marker"));
		PulledBack.TrackFraction = 0.3f;
		Layout.Objectives = { PushedFar, PulledBack };

		// Delay 3.0 s: B arrives at 3.75 s, after the push carried the carriage past 0.6 (needs >= 2.5 s).
		Layout.SolutionBlocks = {
			MakeDelay(5, 1, EHueDir::North, 3.0f, true),
		};
	}

	// ---- Exercise 3: synthesize a stop ---------------------------------------------------------------------------
	{
		FHueBoardLayout& Layout = OutLayouts.Add_GetRef(MakeLayout(2,
			TEXT("3 \u00B7 Synthesize a Stop"),
			TEXT("Route 1 starts the dancer. The mixer turns A + B into N, which brakes her - but only if both arrive ")
			TEXT("within 0.5 s. The B route is longer: place the delay on the A route and tune it.")));

		Layout.Blocks = {
			// Route 1: A reaches R (West face) at 1.00 s and starts the carriage.
			MakeSource(0, 3, EHueDir::East, EHueColor::B),
			MakeBlock(EHueBlockKind::ColorShift, 1, 3, EHueDir::East, false),
			MakeReceiver(4, 3, { EHueDir::West, EHueDir::South }),
			// Mixer output North -> (4,2) -> R South face; inputs: West face (input 0), East face (input 1).
			MakeBlock(EHueBlockKind::Mixer, 4, 1, EHueDir::North, false),
			// Route 2: A reaches M's West input at 1.00 s.
			MakeSource(0, 1, EHueDir::East, EHueColor::B),
			MakeBlock(EHueBlockKind::ColorShift, 1, 1, EHueDir::East, false),
			// Route 3: B along row 0, T(5,0) North [N+W] turns it north, T(5,1) West [W+S] turns it west into
			// M's East input at 1.75 s.
			MakeSource(0, 0, EHueDir::East, EHueColor::B),
			MakeBlock(EHueBlockKind::Turn, 5, 0, EHueDir::North, false),
			MakeBlock(EHueBlockKind::Turn, 5, 1, EHueDir::West, false),
		};

		Layout.Pickups = {
			MakePickup(EHueModuleType::Delay, 1, 100.f, 720.f),
		};

		FHueObjective Leave = MakeObjective(EHueObjectiveType::LeaveStart, TEXT("Leaves the start"));
		Leave.TrackFraction = 0.12f;
		FHueObjective Stop = MakeObjective(EHueObjectiveType::StopInZone, TEXT("Stop in the spotlight and hold still for 1 s"));
		Stop.ZoneMin = 0.30f;
		Stop.ZoneMax = 0.56f;
		Stop.MaxSpeedFraction = 0.02f;
		Stop.HoldSeconds = 1.0f;
		Layout.Objectives = { Leave, Stop };

		// D(2,1) East 1.0 s: A reaches M at 2.00 s, B waiting since 1.75 s (dt 0.25 s) -> N leaves M at 2.00 s and
		// reaches R at 2.50 s. Any delay 0.25 .. 1.25 s stops inside the zone.
		Layout.SolutionBlocks = {
			MakeDelay(2, 1, EHueDir::East, 1.0f, true),
		};
	}

	// ---- Exercise 4: colour key ----------------------------------------------------------------------------------
	// Colour is a key, locks have a colour: a blue brick on the grid, a yellow board on the stage. A key colour does
	// nothing to the motion; Y makes the dancer do her move where she is, and the board only breaks for a move made
	// within reach of it - too early she misses, too late she runs into it and the attempt fails. Three steps, each from
	// an earlier idea: open the brick with the spare B (routing, ex 1), make Y = A + G (mixing, ex 3), time it (delay).
	{
		FHueBoardLayout& Layout = OutLayouts.Add_GetRef(MakeLayout(3,
			TEXT("4 · Colour Key"),
			TEXT("A pane of yellow glass stands on the dancer's track. When yellow light (Y) reaches her she strikes, which ")
			TEXT("shatters the glass from the yellow strike zone in front of it - too early she misses, too late she runs ")
			TEXT("into it. {Supply}. 1) BRICK B blocks the push and only B breaks it: turn the spare B into it. 2) Light adds ")
			TEXT("up: A + G = Y - mix where they meet. 3) Delay the Y until she is in the strike zone.")));

		Layout.Blocks = {
			// The push: A along row 3 reaches K(3,3) at 0.75 s and R(4,3) (West face) at 1.00 s once K is open. Shipped,
			// the blue brick blocks it: the dancer never starts.
			MakeSource(0, 3, EHueDir::East, EHueColor::A),
			MakeBrick(3, 3, EHueColor::B),
			MakeReceiver(4, 3, { EHueDir::West, EHueDir::South }),
			// The spare: B travelling West along row 4 past (3,4) at 0.25 s and off the board - unless a turn there
			// sends it South into K (0.50 s, before the push).
			MakeSource(4, 4, EHueDir::West, EHueColor::B),
			// The key: A from (2,1) reaches (4,1) at 0.50 s, G from (5,1) at 0.25 s (dt 0.25 s). Without a mixer they
			// cross and stop at the opposite source.
			MakeSource(2, 1, EHueDir::East, EHueColor::A),
			MakeSource(5, 1, EHueDir::West, EHueColor::G),
		};

		Layout.Pickups = {
			MakePickup(EHueModuleType::Turn, 1, 100.f, 720.f),
			MakePickup(EHueModuleType::Mixer, 1, 260.f, 720.f),
			MakePickup(EHueModuleType::Delay, 1, 420.f, 720.f),
		};

		FHueObjective YellowBoard = MakeObjective(EHueObjectiveType::BreakBoard, TEXT("Break the yellow glass"));
		YellowBoard.Color = EHueColor::Y;
		// Three delay steps work: 2.50 s and one step either side (one step moves the dancer 0.075 at cruise 3 m/s). The
		// zone holds the moves of 2.25 / 2.50 / 2.75 s both in the prediction (0.555 / 0.63 / 0.705) and in real physics,
		// which runs ~0.015 ahead (0.57 / 0.645 / 0.72); 2.00 s (0.48 / 0.495) and 3.00 s (0.78 / 0.795) stay >= 0.025
		// outside. The glass stands where a strike from the zone's far end lands (~0.075 on); a strike from further back
		// cracks it, and it bursts as she gets there.
		YellowBoard.ZoneMin = 0.52f;
		YellowBoard.ZoneMax = 0.745f;
		YellowBoard.TrackFraction = 0.80f;
		Layout.Objectives = { YellowBoard };

		// T(3,4) South [S+E]: the spare B turns South and breaks K at 0.50 s; the push passes at 0.75 s, R turns A at
		// 1.00 s and the dancer runs (cruise 3 m/s = 0.075 of the track per 0.25 s). M(4,1) North: Y leaves at 0.50 s,
		// D(4,2) North holds it, R gets it at 1.00 s + delay. Delay 2.50 s -> move at ~0.63, in the strike zone (2.25 s and
		// 2.75 s too); 2.00 s misses and she runs into the glass, 3.00 s is too late.
		Layout.SolutionBlocks = {
			MakeBlock(EHueBlockKind::Turn, 3, 4, EHueDir::South, true),
			MakeBlock(EHueBlockKind::Mixer, 4, 1, EHueDir::North, true),
			MakeDelay(4, 2, EHueDir::North, 2.5f, true),
		};
	}
}

// ---- Static helpers -------------------------------------------------------------------------------------------

APulseBlock* AHueTutorialBuilder::SpawnBlockFromSpec(AHueBoard* Board, const FHueBlockSpec& Spec, AReactiveRig* Rig)
{
	if (!IsValid(Board))
	{
		return nullptr;
	}

	TSubclassOf<APulseBlock> BlockClass;
	bool bFixedKind = true;
	switch (Spec.Kind)
	{
	case EHueBlockKind::Source:
		BlockClass = AHueSourceBlock::StaticClass();
		break;
	case EHueBlockKind::Receiver:
		BlockClass = AHueReceiverBlock::StaticClass();
		break;
	case EHueBlockKind::Barrier:
		BlockClass = AHueBarrierBlock::StaticClass();
		break;
	case EHueBlockKind::Brick:
		BlockClass = AHueBrickBlock::StaticClass();
		break;
	case EHueBlockKind::Turn:
	case EHueBlockKind::ColorShift:
	case EHueBlockKind::Delay:
	case EHueBlockKind::Mixer:
		// Modules go through the game mode catalogue so Blueprint art classes replace the placeholders.
		bFixedKind = false;
		BlockClass = AHueboxGameMode::ResolveModuleClass(Board, HueTutorialPrivate::ToModuleType(Spec.Kind));
		break;
	default:
		break;
	}

	if (!BlockClass)
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: no class for %s at (%d,%d) on %s"),
			HueTutorialPrivate::KindName(Spec.Kind), Spec.Cell.X, Spec.Cell.Y, *Board->GetBoardName());
		return nullptr;
	}

	// Sources, receivers, barriers and bricks are always fixed; modules follow the spec.
	const bool bRemovable = !bFixedKind && Spec.bRemovable;
	APulseBlock* Block = Board->SpawnBlock(BlockClass, Spec.Cell, Spec.Facing, bRemovable);
	if (!Block)
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: cannot place %s at (%d,%d) on %s (%s)"),
			HueTutorialPrivate::KindName(Spec.Kind), Spec.Cell.X, Spec.Cell.Y, *Board->GetBoardName(),
			!Board->IsInside(Spec.Cell) ? TEXT("outside the grid") : TEXT("cell occupied"));
		return nullptr;
	}

	if (AHueSourceBlock* SourceBlock = Cast<AHueSourceBlock>(Block))
	{
		SourceBlock->SetEmitColor(Spec.SourceColor);
	}
	else if (AHueDelayBlock* DelayBlock = Cast<AHueDelayBlock>(Block))
	{
		DelayBlock->SetDelaySeconds(Spec.DelaySeconds);
	}
	else if (AHueBrickBlock* Brick = Cast<AHueBrickBlock>(Block))
	{
		Brick->SetBrickColor(Spec.BrickColor);
	}
	else if (AHueReceiverBlock* ReceiverBlock = Cast<AHueReceiverBlock>(Block))
	{
		// An empty face list keeps the receiver's class defaults.
		if (Spec.ReceiverFaces.Num() > 0)
		{
			ReceiverBlock->SetAcceptFaces(Spec.ReceiverFaces);
		}
		AReactiveRig* LinkedRig = IsValid(Rig) ? Rig : Board->GetRig();
		ReceiverBlock->SetRig(LinkedRig);
		if (!IsValid(LinkedRig))
		{
			UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: receiver at (%d,%d) on %s has no rig to drive"),
				Spec.Cell.X, Spec.Cell.Y, *Board->GetBoardName());
		}
	}

	return Block;
}

void AHueTutorialBuilder::InstallSolution(AHueBoard* Board, const FHueBoardLayout& Layout)
{
	if (!IsValid(Board))
	{
		return;
	}

	AReactiveRig* Rig = Board->GetRig();
	int32 Installed = 0;
	for (const FHueBlockSpec& Spec : Layout.SolutionBlocks)
	{
		if (APulseBlock* Existing = Board->GetBlockAt(Spec.Cell))
		{
			if (!Existing->IsRemovable())
			{
				UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: solution %s at (%d,%d) on %s skipped - a fixed %s is there"),
					HueTutorialPrivate::KindName(Spec.Kind), Spec.Cell.X, Spec.Cell.Y, *Board->GetBoardName(),
					*Existing->GetDisplayName());
				continue;
			}
			// A removable block (pre-placed or player-placed) is replaced by the solution block.
			Board->RemoveBlock(Existing);
		}

		if (SpawnBlockFromSpec(Board, Spec, Rig))
		{
			++Installed;
		}
	}

	UE_LOG(LogHuebox, Log, TEXT("HueTutorialBuilder: installed %d/%d solution block(s) on %s"),
		Installed, Layout.SolutionBlocks.Num(), *Board->GetBoardName());
}

const FHueBoardLayout* AHueTutorialBuilder::FindLayoutForBoard(const AHueBoard* Board) const
{
	if (!Board)
	{
		return nullptr;
	}

	if (const int32* Index = BoardToLayout.Find(TWeakObjectPtr<const AHueBoard>(Board)))
	{
		if (Layouts.IsValidIndex(*Index))
		{
			return &Layouts[*Index];
		}
	}

	// Boards this builder did not spawn (hand-placed copies): match by name.
	for (const FHueBoardLayout& Layout : Layouts)
	{
		if (Layout.Name == Board->GetBoardName())
		{
			return &Layout;
		}
	}
	return nullptr;
}

AHueTutorialBuilder* AHueTutorialBuilder::FindInWorld(const UObject* WorldContext)
{
	UWorld* World = (WorldContext && GEngine)
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<AHueTutorialBuilder> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

// ---- Lifetime -------------------------------------------------------------------------------------------------

void AHueTutorialBuilder::BeginPlay()
{
	Super::BeginPlay();

	if (Layouts.Num() == 0)
	{
		UE_LOG(LogHuebox, Log, TEXT("HueTutorialBuilder: no layouts set - using the default Level 1 exercises"));
		MakeDefaultTutorialLayouts(Layouts);
	}

	// Environment first: the floor probe must not see our own boards.
	SpawnEnvironmentIfMissing();
	BuildAll();
}

void AHueTutorialBuilder::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Level teardown destroys everything anyway; only an explicit removal of the builder takes its exhibit along.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
		{
			AHueBoard* Board = Cast<AHueBoard>(Spawned.Get());
			if (!IsValid(Board))
			{
				continue;
			}
			// Also catches blocks the player placed. Scratch raw-pointer list: it never outlives this scope, so no
			// garbage collection can run while it is held.
			TArray<APulseBlock*, TInlineAllocator<32>> BoardBlocks;
			Board->ForEachBlock([&BoardBlocks](APulseBlock& Block) { BoardBlocks.Add(&Block); });
			for (APulseBlock* Block : BoardBlocks)
			{
				Board->RemoveBlock(Block);
			}
		}

		for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
		{
			if (IsValid(Spawned))
			{
				Spawned->Destroy();
			}
		}
	}

	GetWorldTimerManager().ClearAllTimersForObject(this);
	SpawnedActors.Empty();
	BoardToLayout.Empty();

	Super::EndPlay(EndPlayReason);
}

// ---- Building -------------------------------------------------------------------------------------------------

void AHueTutorialBuilder::BuildAll()
{
	int32 Built = 0;
	for (int32 Index = 0; Index < Layouts.Num(); ++Index)
	{
		if (BuildBoard(Layouts[Index], Index))
		{
			++Built;
		}
	}

	SpawnedActors.RemoveAll([](const TObjectPtr<AActor>& Actor) { return !IsValid(Actor); });
	UE_LOG(LogHuebox, Log, TEXT("HueTutorialBuilder: built %d/%d exercise board(s)"), Built, Layouts.Num());
}

AHueBoard* AHueTutorialBuilder::BuildBoard(const FHueBoardLayout& Layout, int32 Index)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// Boards and rigs are spawned unrotated (track direction and DOF locks are world-axis aligned).
	const FVector BoardOrigin = GetActorLocation() + Layout.Origin;

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// ---- Board (begins play inside SpawnActor; RebuildGridVisuals copes with the size set right after) -------------
	AHueBoard* Board = World->SpawnActor<AHueBoard>(AHueBoard::StaticClass(), FTransform(BoardOrigin), Params);
	if (!Board)
	{
		UE_LOG(LogHuebox, Error, TEXT("HueTutorialBuilder: failed to spawn board '%s'"), *Layout.Name);
		return nullptr;
	}
	SpawnedActors.Add(Board);
	BoardToLayout.Add(TWeakObjectPtr<const AHueBoard>(Board), Index);
	Board->SetGridSize(Layout.GridSize);
	const EHuePlayMode PlayMode = AHueboxGameMode::GetPlayMode(this);
	Board->SetTexts(Layout.Name, HueTutorialPrivate::ResolveBrief(Layout.Brief, PlayMode));

	// ---- Rig (deferred: TrackLength must be known before its construction lays out the track) ------------------
	const FTransform RigTransform(BoardOrigin + Layout.RigOffset);
	AReactiveRig* Rig = World->SpawnActorDeferred<AReactiveRig>(AReactiveRig::StaticClass(), RigTransform, this, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Rig)
	{
		Rig->TrackLength = FMath::Max(200.f, Layout.TrackLength);
		Rig->FinishSpawning(RigTransform);
		SpawnedActors.Add(Rig);
		Board->SetRig(Rig);
	}
	else
	{
		UE_LOG(LogHuebox, Error, TEXT("HueTutorialBuilder: failed to spawn the rig of '%s'"), *Layout.Name);
	}

	// ---- Blocks -------------------------------------------------------------------------------------------------
	bool bHasSource = false;
	bool bHasReceiver = false;
	for (const FHueBlockSpec& Spec : Layout.Blocks)
	{
		if (APulseBlock* Block = SpawnBlockFromSpec(Board, Spec, Rig))
		{
			SpawnedActors.Add(Block);
			bHasSource |= Spec.Kind == EHueBlockKind::Source;
			bHasReceiver |= Spec.Kind == EHueBlockKind::Receiver;
		}
	}
	if (!bHasSource || !bHasReceiver)
	{
		UE_LOG(LogHuebox, Warning, TEXT("HueTutorialBuilder: '%s' has %s - it cannot be completed"),
			*Layout.Name, !bHasSource ? TEXT("no source") : TEXT("no receiver"));
	}

	// ---- Supply tray ----------------------------------------------------------------------------------------------
	// Instruction: the modules go straight into the board's own tray (no floor pickups; collected blocks come back
	// to it). Sandbox: they lie next to the board as pickups for the first-person maker, as before.
	if (PlayMode == EHuePlayMode::Instruction)
	{
		HueTutorialPrivate::FillTray(Board, Layout);
	}
	else
	{
		for (const FHuePickupSpec& PickupSpec : Layout.Pickups)
		{
			if (PickupSpec.Type == EHueModuleType::None || PickupSpec.Count <= 0)
			{
				continue;
			}
			const FVector PickupLocation = BoardOrigin
				+ FVector(PickupSpec.LocalPosition.X, PickupSpec.LocalPosition.Y, HueTutorialPrivate::PickupHeight);
			AHueModulePickup* Pickup = World->SpawnActor<AHueModulePickup>(AHueModulePickup::StaticClass(), FTransform(PickupLocation), Params);
			if (Pickup)
			{
				Pickup->SetContents(PickupSpec.Type, PickupSpec.Count);
				SpawnedActors.Add(Pickup);
			}
		}
	}

	// ---- Objectives (after SetRig, so the rig shows the markers once) + progression -------------------------------
	Board->SetObjectives(Layout.Objectives);

	// The board registered itself in BeginPlay already; RegisterBoard dedupes and re-sorts.
	if (AExhibitManager* Manager = AExhibitManager::Get(this))
	{
		Manager->RegisterBoard(Board);
	}

	return Board;
}

void AHueTutorialBuilder::SpawnEnvironmentIfMissing()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FVector Base = GetActorLocation();

	// ---- Ground slab: 90 m x 50 m, top at the builder's height --------------------------------------------------
	if (bSpawnGround)
	{
		// Probe the play area (board centres + default player spawn) for an existing level floor.
		TArray<FVector, TInlineAllocator<8>> Probes;
		for (const FHueBoardLayout& Layout : Layouts)
		{
			const float HalfCell = 0.5f * HueTutorialPrivate::ProbeCellSize;
			Probes.Add(Base + Layout.Origin + FVector(Layout.GridSize.X * HalfCell, Layout.GridSize.Y * HalfCell, 0.f));
		}
		Probes.Add(Base + FVector(300.f, 1200.f, 0.f));

		int32 Covered = 0;
		for (const FVector& Probe : Probes)
		{
			Covered += HueTutorialPrivate::HasFloorAt(World, Probe) ? 1 : 0;
		}

		if (Covered == Probes.Num())
		{
			UE_LOG(LogHuebox, Log, TEXT("HueTutorialBuilder: the level already has a floor under the exercises - no ground slab"));
		}
		else
		{
			// Partially covered: sink the slab 2 cm so it never z-fights with the level's own floor.
			const float TopZ = Covered > 0 ? -2.f : 0.f;
			const FTransform GroundTransform(FRotator::ZeroRotator,
				Base + FVector(HueTutorialPrivate::GroundCenterX, HueTutorialPrivate::GroundCenterY, TopZ - 10.f),
				FVector(2.f * HueTutorialPrivate::GroundHalfX / 100.f, 2.f * HueTutorialPrivate::GroundHalfY / 100.f, 0.2f));

			AStaticMeshActor* Ground = World->SpawnActorDeferred<AStaticMeshActor>(AStaticMeshActor::StaticClass(), GroundTransform,
				this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Ground)
			{
				// Components are already registered by SpawnActorDeferred: Movable first, so the mesh / material can
				// be changed at runtime.
				Ground->SetMobility(EComponentMobility::Movable);
				if (UStaticMeshComponent* GroundMesh = Ground->GetStaticMeshComponent())
				{
					GroundMesh->SetStaticMesh(APulseBlock::GetCubeMesh());
					GroundMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
					if (UMaterialInterface* BaseMaterial = APulseBlock::GetBasicMaterial())
					{
						// Outer = component; the component's material slot (a UPROPERTY) keeps the MID alive.
						UMaterialInstanceDynamic* GroundMID = UMaterialInstanceDynamic::Create(BaseMaterial, GroundMesh);
						GroundMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.32f, 0.32f, 0.32f));
						GroundMesh->SetMaterial(0, GroundMID);
					}
				}
				Ground->Tags.Add(TEXT("HueGround"));
				Ground->FinishSpawning(GroundTransform);
				SpawnedActors.Add(Ground);
			}

			// The slab is the only floor: frame it with invisible walls 1 m inside its edges so the maker cannot walk
			// off (GDD: no falling death), and make a fall that still happens end within seconds instead of minutes
			// (AHueboxCharacter::FellOutOfWorld puts the pawn back at the player start instead of destroying it).
			if (Covered == 0)
			{
				HueTutorialPrivate::SpawnBoundaryWalls(World, this,
					Base + FVector(HueTutorialPrivate::GroundCenterX, HueTutorialPrivate::GroundCenterY, TopZ), SpawnedActors);
				if (AWorldSettings* WorldSettings = World->GetWorldSettings())
				{
					WorldSettings->KillZ = FMath::Max(WorldSettings->KillZ, static_cast<float>(Base.Z + TopZ) - HueTutorialPrivate::KillDepth);
				}
			}
		}
	}

	// ---- Lights: only when the level has no sun at all -------------------------------------------------------------
	if (bSpawnLightsIfMissing && !HueTutorialPrivate::WorldHasActor<ADirectionalLight>(World))
	{
		const FTransform SunTransform(FRotator(-50.f, 40.f, 0.f), Base + FVector(1900.f, 600.f, 800.f));
		ADirectionalLight* Sun = World->SpawnActorDeferred<ADirectionalLight>(ADirectionalLight::StaticClass(), SunTransform,
			this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Sun)
		{
			if (ULightComponent* SunLight = Sun->GetLightComponent())
			{
				SunLight->SetMobility(EComponentMobility::Movable);
			}
			Sun->FinishSpawning(SunTransform);
			SpawnedActors.Add(Sun);
		}

		if (!HueTutorialPrivate::WorldHasActor<ASkyLight>(World))
		{
			const FTransform SkyTransform(Base + FVector(1900.f, 600.f, 400.f));
			ASkyLight* Sky = World->SpawnActorDeferred<ASkyLight>(ASkyLight::StaticClass(), SkyTransform,
				this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Sky)
			{
				if (USkyLightComponent* SkyComponent = Sky->GetLightComponent())
				{
					// SpawnActorDeferred already registered the native components, so the flag must go through the
					// setter: it re-creates the render state (the proxy is only created when real-time capture is on)
					// and queues a capture. A direct write after SetMobility's re-register would never take effect.
					SkyComponent->SetMobility(EComponentMobility::Movable);
					SkyComponent->SetRealTimeCaptureEnabled(true);
				}
				Sky->FinishSpawning(SkyTransform);
				SpawnedActors.Add(Sky);
			}
		}

		if (!HueTutorialPrivate::WorldHasActor<ASkyAtmosphere>(World))
		{
			FActorSpawnParameters Params;
			Params.Owner = this;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FTransform(Base), Params))
			{
				SpawnedActors.Add(Atmosphere);
			}
		}

		UE_LOG(LogHuebox, Log, TEXT("HueTutorialBuilder: the level had no directional light - spawned placeholder lighting"));
	}
}
