#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Core/HueTypes.h"
#include "HueboxGameMode.generated.h"

class APulseBlock;
class AExhibitManager;
class AHueTutorialBuilder;

/**
 * Game mode for the Huebox levels. Sets the first-person maker pawn, controller and HUD, makes sure an
 * AExhibitManager exists, and (if the level has no builder) spawns the tutorial builder so even an empty map
 * becomes the Level 1 prototype table. Also the single catalogue mapping inventory module types to classes,
 * so Blueprint subclasses with real art can replace the placeholder blocks without touching C++.
 */
UCLASS()
class HUEBOX_API AHueboxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AHueboxGameMode();

	/** Resolves PlayMode: URL option "?HueMode=Sandbox|Instruction" or the -HueSandbox switch override the default. */
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	/** Instruction -> BuilderPawnClass, Sandbox -> SandboxPawnClass. */
	virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;

	EHuePlayMode GetPlayMode() const { return PlayMode; }
	/** Play mode of the world's Huebox game mode; Instruction when there is none (tests, other game modes). */
	static EHuePlayMode GetPlayMode(const UObject* WorldContext);

	/** Class spawned when an inventory item of Type is placed. Falls back to the native classes. */
	TSubclassOf<APulseBlock> GetModuleClass(EHueModuleType Type) const;
	static TSubclassOf<APulseBlock> ResolveModuleClass(const UObject* WorldContext, EHueModuleType Type);

	AExhibitManager* GetExhibitManager() const { return ExhibitManager; }

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Hue|Modules")
	TMap<EHueModuleType, TSubclassOf<APulseBlock>> ModuleClasses;

	UPROPERTY(EditDefaultsOnly, Category = "Hue|Mode")
	EHuePlayMode PlayMode = EHuePlayMode::Instruction;

	/** Top-down build / side watch pawn (AHueBuilderPawn). */
	UPROPERTY(EditDefaultsOnly, Category = "Hue|Mode")
	TSubclassOf<APawn> BuilderPawnClass;

	/** Free first-person maker (AHueboxCharacter). */
	UPROPERTY(EditDefaultsOnly, Category = "Hue|Mode")
	TSubclassOf<APawn> SandboxPawnClass;

	/** Spawn a tutorial builder when the level contains none. */
	UPROPERTY(EditDefaultsOnly, Category = "Hue|Level")
	bool bAutoSpawnTutorial = true;

	UPROPERTY(EditDefaultsOnly, Category = "Hue|Level")
	TSubclassOf<AHueTutorialBuilder> TutorialBuilderClass;

	/** Used when the level has no APlayerStart. */
	UPROPERTY(EditDefaultsOnly, Category = "Hue|Level")
	FTransform FallbackSpawn = FTransform(FRotator(0.f, -90.f, 0.f), FVector(300.f, 1200.f, 120.f));

	UPROPERTY(Transient)
	TObjectPtr<AExhibitManager> ExhibitManager;

	UPROPERTY(Transient)
	TObjectPtr<AActor> FallbackStartActor;

	/** Builder found in or spawned into the level (the level also references it; kept for clarity / debugging). */
	UPROPERTY(Transient)
	TObjectPtr<AHueTutorialBuilder> TutorialBuilder;
};
