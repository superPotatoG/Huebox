#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Core/HueTypes.h"
#include "HueboxCharacter.generated.h"

class UCameraComponent;
class UStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;
class UHueInventoryComponent;
class UHueInputConfig;
class APulseBlock;
class AHueBoard;
class AHueModulePickup;
class APlayerController;
class UDamageType;
struct FInputActionValue;

UENUM()
enum class EHueTargetKind : uint8
{
	None,
	Block,
	Pickup,
	Cell
};

/** What the crosshair currently points at. All object references are weak (selection never owns). */
struct FHueInteractionTarget
{
	EHueTargetKind Kind = EHueTargetKind::None;
	TWeakObjectPtr<APulseBlock> Block;
	TWeakObjectPtr<AHueModulePickup> Pickup;
	TWeakObjectPtr<AHueBoard> Board;
	FIntPoint Cell = FIntPoint(INDEX_NONE, INDEX_NONE);
	/** For Cell targets: can the selected module be placed here? */
	bool bCanPlace = false;
	/** Player-facing reason when bCanPlace is false. */
	FString Reason;
};

/**
 * The maker: a first-person pawn (no visible body, GDD 5) that walks around the workbench with WASD and uses
 * the left mouse button to collect pickups, collect placed modules back into the inventory, or place the
 * selected module on the targeted empty cell. Input actions/mapping are created in C++ (UHueInputConfig).
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
	const FHueInteractionTarget& GetTarget() const { return Target; }
	EHueDir GetPreviewFacing() const { return PreviewFacing; }
	bool IsHelpVisible() const { return bShowHelp; }
	bool IsPauseMenuOpen() const { return bPauseMenuOpen; }

	/** Short message on the HUD (inventory full, cannot place, ...). Also forwarded to the exhibit feedback log. */
	void ShowMessage(EHueFeedbackKind Kind, const FString& Message);

	/** Collects a placed block into the inventory (used by APulseBlock::Interact). */
	bool TryCollectBlock(APulseBlock* Block);
	/** Collects a pickup into the inventory (used by AHueModulePickup::Interact). Returns number taken. */
	int32 TryCollectPickup(AHueModulePickup* Pickup);

	/** Max interaction distance from the camera (cm). */
	UPROPERTY(EditAnywhere, Category = "Hue|Interaction")
	float InteractRange = 900.f;

protected:
	virtual void BeginPlay() override;
	/** Unhighlights the target and takes the mapping context off the player's input stack. */
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

	void UpdateTarget();
	void UpdatePreview();
	void ClearHighlight();
	void RotateTargetOrPreview(int32 Steps);
	void AdjustTargetDelay(int32 Steps);
	bool TryPlaceSelected();
	/** True if the pawn's capsule overlaps Cell of Board (cannot place a block on yourself). */
	bool IsStandingOnCell(const AHueBoard* Board, FIntPoint Cell) const;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UHueInventoryComponent> Inventory;

	/** Ghost of the module that would be placed (absolute transform, no collision). */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> PreviewMesh;

	/** Cone showing the preview facing. */
	UPROPERTY(VisibleAnywhere, Category = "Hue|Components")
	TObjectPtr<UStaticMeshComponent> PreviewArrow;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PreviewMID;

	/** Runtime-created input actions + mapping context. Owned here so the GC keeps them alive. */
	UPROPERTY(Transient)
	TObjectPtr<UHueInputConfig> InputConfig;

	FHueInteractionTarget Target;
	/** Block currently highlighted (weak: it may be collected or destroyed at any time). */
	TWeakObjectPtr<APulseBlock> HighlightedBlock;

	EHueDir PreviewFacing = EHueDir::East;
	bool bShowHelp = true;
	bool bPauseMenuOpen = false;
	bool bMappingAdded = false;

private:
	/** Removes the mapping context from the controller it was added to (unpossess / EndPlay). */
	void RemoveMappingContext();
	/** Build-state check through the exhibit manager (no manager => editing allowed). */
	bool IsEditingAllowed() const;
	/** Selects the first slot holding Type if the current selection is empty (after a collect). */
	void SelectIfNothingSelected(EHueModuleType Type);
	/** After placing / dropping the last module of the selected stack: selects the next filled slot (wrapping), if any. */
	void SelectNextFilledSlotIfEmpty();

	/** Controller whose local player received our mapping context. Weak: the pawn never keeps it alive. */
	TWeakObjectPtr<APlayerController> MappedController;

	/** Last preview colour state written to PreviewMID (-1 unknown, 0 invalid, 1 valid) to avoid redundant updates. */
	int8 PreviewColorState = -1;
};
