#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/HueTypes.h"
#include "HueTutorialBuilder.generated.h"

class APulseBlock;
class AHueBoard;
class AReactiveRig;
class AHueModulePickup;
class UTextRenderComponent;

UENUM(BlueprintType)
enum class EHueBlockKind : uint8
{
	Source,
	Receiver,
	Barrier,
	Turn,
	ColorShift,
	Delay,
	Mixer,
	/** Fixed lock that only BrickColor breaks, using that pulse up (AHueBrickBlock). */
	Brick
};

/** One block in a board layout. */
USTRUCT(BlueprintType)
struct FHueBlockSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	EHueBlockKind Kind = EHueBlockKind::Barrier;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FIntPoint Cell = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	EHueDir Facing = EHueDir::East;

	/** Pre-placed modules can be removable (player may collect/re-route them) or fixed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	bool bRemovable = false;

	/** Source only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	EHueColor SourceColor = EHueColor::B;

	/** Source only: faces that each emit one pulse (a source base). Empty = the facing only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<EHueDir> SourceFaces;

	/** Delay only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	float DelaySeconds = 0.0f;

	/** Receiver only: faces through which pulses may enter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<EHueDir> ReceiverFaces;

	/** Brick only: the one colour that breaks it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	EHueColor BrickColor = EHueColor::B;
};

/** A stack of modules lying next to the board (the exercise's supply tray). */
USTRUCT(BlueprintType)
struct FHuePickupSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	EHueModuleType Type = EHueModuleType::Turn;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	int32 Count = 1;

	/** Board-local position in cm (0,0 = south-west corner of the grid) of the floor pickup. Sandbox only: in
	 *  Instruction mode the stack goes into the board's tray. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D LocalPosition = FVector2D::ZeroVector;
};

/** Complete description of one exercise board. Editable in the details panel of the builder. */
USTRUCT(BlueprintType)
struct FHueBoardLayout
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FString Name;

	/** "{Supply}" is replaced by where the current play mode keeps the modules (Sandbox: floor, Instruction: tray). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (MultiLine = "true"))
	FString Brief;

	/** World position of the board's south-west corner, relative to the builder actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector Origin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FIntPoint GridSize = FIntPoint(6, 6);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<FHueBlockSpec> Blocks;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<FHuePickupSpec> Pickups;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<FHueObjective> Objectives;

	/** Rig position relative to the board origin (track runs along +X from here; default: viewer side, 3 m north of the grid). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector RigOffset = FVector(-200.f, -170.f, 0.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	float TrackLength = 1000.f;

	/**
	 * Baseline solution for graybox validation (GDD 4): blocks the self-test installs on top of the layout to
	 * prove the exercise is solvable. A solution block on a cell holding a removable block replaces it.
	 * Never shown to the player.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<FHueBlockSpec> SolutionBlocks;
};

/**
 * Builds the Level 1 "prototype table" at BeginPlay from data: four exercise boards (GDD 4: route a signal,
 * change motion, synthesize a stop; plus the colour key: open a blue brick, mix a Y and time it to break the yellow board on stage), their rigs, their module supply (Instruction mode: the board's tray; Sandbox mode:
 * floor pickups), titles, and - if the level has none - a ground slab and lights. A fifth board east of them is the
 * Level 2 sample (8 x 8, source base with four outputs) until Level 2 gets its own map. Layouts are plain USTRUCT
 * data so designers can edit them in the details panel.
 */
UCLASS()
class HUEBOX_API AHueTutorialBuilder : public AActor
{
	GENERATED_BODY()

public:
	AHueTutorialBuilder();

	/** The tutorial exercises (the three of the GDD, then the colour key), then the Level 2 sample. */
	static void MakeDefaultTutorialLayouts(TArray<FHueBoardLayout>& OutLayouts);

	/** Spawns one block from a spec on Board (handles kind -> class, colour, delay, receiver faces, rig link). */
	static APulseBlock* SpawnBlockFromSpec(AHueBoard* Board, const FHueBlockSpec& Spec, AReactiveRig* Rig);

	/** Installs Layout.SolutionBlocks on Board (used by the self-test). */
	static void InstallSolution(AHueBoard* Board, const FHueBoardLayout& Layout);

	const TArray<FHueBoardLayout>& GetLayouts() const { return Layouts; }
	/** Layout that produced Board, or null. */
	const FHueBoardLayout* FindLayoutForBoard(const AHueBoard* Board) const;

	static AHueTutorialBuilder* FindInWorld(const UObject* WorldContext);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void BuildAll();
	AHueBoard* BuildBoard(const FHueBoardLayout& Layout, int32 Index);
	void SpawnEnvironmentIfMissing();

	UPROPERTY(EditAnywhere, Category = "Hue|Tutorial")
	TArray<FHueBoardLayout> Layouts;

	UPROPERTY(EditAnywhere, Category = "Hue|Tutorial")
	bool bSpawnGround = true;

	UPROPERTY(EditAnywhere, Category = "Hue|Tutorial")
	bool bSpawnLightsIfMissing = true;

	/** Everything this builder spawned; destroyed if the builder is removed during play. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	/** Board -> layout index (weak keys: boards may be destroyed independently). */
	TMap<TWeakObjectPtr<const AHueBoard>, int32> BoardToLayout;
};
