#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Core/HueTypes.h"
#include "Player/HueBuildToolComponent.h"
#include "HueboxCharacter.generated.h"

class UCameraComponent;
class UHueInventoryComponent;
class UHueInputConfig;
class AHueBoard;
class APlayerController;
class UDamageType;
struct FInputActionValue;

/**
 * The maker in Sandbox mode (-HueSandbox / ?HueMode=Sandbox): a first-person pawn (no visible body, GDD 5) that
 * walks around the workbench with WASD. All construction rules live in UHueBuildToolComponent: every tick the
 * camera trace becomes the tool's target, and LMB (collect pickup / collect block / place the selected module),
 * Q/E, Z/X, wheel and 1-6 are forwarded to it. The pawn keeps its own inventory (the tool's inventory source),
 * floor drops (G) and the fall recovery. Input actions/mapping are created in C++ (UHueInputConfig).
 */
UCLASS()
class HUEBOX_API AHueboxCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AHueboxCharacter();

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;
	/** Below KillZ: back to the player start (keeps inventory and possession) instead of AActor's Destroy, since
	 *  nothing would respawn the maker (GDD: no falling death). */
	virtual void FellOutOfWorld(const UDamageType& DmgType) override;

	UHueInventoryComponent* GetInventory() const { return Inventory; }
	UHueBuildToolComponent* GetBuildTool() const { return BuildTool; }
	/** Convenience forwards to the build tool (crosshair target / placement facing). */
	const FHueInteractionTarget& GetTarget() const;
	EHueDir GetPreviewFacing() const;
	bool IsHelpVisible() const { return bShowHelp; }
	bool IsPauseMenuOpen() const { return bPauseMenuOpen; }

	/** Short message on the HUD (fell out, nothing to drop, ...). Forwarded to the exhibit feedback log by the tool. */
	void ShowMessage(EHueFeedbackKind Kind, const FString& Message);

	/** Max interaction distance from the camera (cm). */
	UPROPERTY(EditAnywhere, Category = "Hue|Interaction")
	float InteractRange = 900.f;

protected:
	virtual void BeginPlay() override;
	/** Unbinds the tool's placement veto and takes the mapping context off the player's input stack. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Input handlers ---------------------------------------------------------------------------------------
	void HandleMove(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
	void HandleInteract();
	void HandleRotateLeft();
	void HandleRotateRight();
	void HandleDelayDown();
	void HandleDelayUp();
	void HandleHotbarScroll(const FInputActionValue& Value);
	void HandleHotbarSlot(const FInputActionValue& Value);
	void HandleDrop();
	void HandleRunToggle();
	void HandleReset();
	void HandleToggleHelp();
	void HandlePauseMenu();

	void EnsureInputConfig();
	void AddMappingContext();

	/** Camera line trace -> BuildTool->SetTargetFromHit (a miss clears the target). */
	void UpdateTarget();
	/** Placement veto (bound to BuildTool->CellBlockedQuery): true if the capsule overlaps Cell of Board. */
	bool IsStandingOnCell(const AHueBoard* Board, FIntPoint Cell) const;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UCameraComponent> Camera;

	/** Personal inventory (Sandbox): the build tool pays from it and collects into it. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UHueInventoryComponent> Inventory;

	/** Targeting, placement rules, ghost preview and highlight (shared with AHueBuilderPawn). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UHueBuildToolComponent> BuildTool;

	/** Runtime-created input actions + mapping context. Owned here so the GC keeps them alive. */
	UPROPERTY(Transient)
	TObjectPtr<UHueInputConfig> InputConfig;

	bool bShowHelp = true;
	bool bPauseMenuOpen = false;
	bool bMappingAdded = false;

private:
	/** Removes the mapping context from the controller it was added to (unpossess / EndPlay). */
	void RemoveMappingContext();
	/** After dropping the last module of the selected stack: selects the next filled slot (wrapping), if any.
	 *  (Same rule the build tool applies after a placement.) */
	void SelectNextFilledSlotIfEmpty();

	/** Controller whose local player received our mapping context. Weak: the pawn never keeps it alive. */
	TWeakObjectPtr<APlayerController> MappedController;
};
