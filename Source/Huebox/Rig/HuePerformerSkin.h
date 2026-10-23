#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "HuePerformerSkin.generated.h"

class USkeletalMesh;
class UAnimSequence;
class UMaterialInterface;

/**
 * Presentation of a reactive rig ("what the mechanism looks like"). Pure data: AReactiveRig reads the class
 * default object (GetDefault) and builds runtime components from it; physics never depends on the skin.
 *
 * Add a skin without C++: Content Browser > Blueprint Class > parent "HuePerformerSkin", fill in the fields,
 * then add it to Project Settings > Game > Huebox > Skins. A C++ subclass works the same way (set the fields in
 * its constructor). See Docs/Huebox_Skins.md.
 *
 * Assets are soft references: a skin does not force its meshes / animations into memory until it is applied,
 * and a missing asset (e.g. the Epic template content is not in the git repo) falls back to primitive shapes.
 */
UCLASS(Blueprintable, BlueprintType, Abstract)
class HUEBOX_API UHuePerformerSkin : public UObject
{
	GENERATED_BODY()

public:
	UHuePerformerSkin();

	/** Shown in skin lists (sandbox / console). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin")
	FText DisplayName;

	/** Name of the moving piece in HUD / labels, e.g. "Dancer", "Carriage". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin")
	FString PerformerName = TEXT("Performer");

	// ---- Which built-in rig parts stay visible ------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Rig parts")
	bool bShowCarriageBody = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Rig parts")
	bool bShowAnchorBlock = true;

	/** Colour of track / end stops / marker posts. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Rig parts")
	FLinearColor StructureColor = FLinearColor(0.4f, 0.42f, 0.45f);

	/** Colour letters (A / B / N) drawn in the signal colour instead of dark-on-colour. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Rig parts")
	bool bLettersInSignalColor = false;

	// ---- Performer (rides the physics body) ---------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	TSoftObjectPtr<USkeletalMesh> PerformerMesh;

	/** Looped while moving; play rate follows the speed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	TSoftObjectPtr<UAnimSequence> PerformerMoveAnim;

	/** Looped while (almost) still. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	TSoftObjectPtr<UAnimSequence> PerformerIdleAnim;

	/**
	 * Played once when a key colour (Y, G, ...) reaches the performer: "the yellow move". Purely presentation - a dance
	 * step, a martial-arts strike or a sword cut all fit; the rules only care where the performer is at that moment.
	 * Empty: the key letter flashes on the performer instead.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	TSoftObjectPtr<UAnimSequence> PerformerKeyActionAnim;

	/** Played once and held when the performer runs into an unbroken stage board (falling backwards). Empty: the
	 *  performer just stops at the board. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	TSoftObjectPtr<UAnimSequence> PerformerCrashAnim;

	/** Uniform scale of the performer mesh (the mannequin is ~180 cm tall). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	float PerformerScale = 0.45f;

	/** Turn the performer to face its direction of motion (profile view, good for silhouettes). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	bool bFaceMotionDirection = false;

	/** Flat unlit colour for the performer (silhouette); when false the mesh keeps its own materials. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	bool bFlatPerformerColor = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Performer")
	FLinearColor PerformerColor = FLinearColor::Black;

	// ---- Partner (stands at the fixed A anchor) -----------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Partner")
	bool bShowPartner = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Partner")
	TSoftObjectPtr<USkeletalMesh> PartnerMesh;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Partner")
	TSoftObjectPtr<UAnimSequence> PartnerAnim;

	// ---- Backdrop (backlit paper screen behind the track) -----------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	bool bShowBackdrop = false;

	/** Base glow of the screen (unlit; values above 1 bloom). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	FLinearColor BackdropColor = FLinearColor(3.0f, 2.75f, 2.3f);

	/** 0..1: how much the current signal colour (A / B / N) tints the screen. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop", meta = (ClampMin = "0", ClampMax = "1"))
	float BackdropSignalTint = 0.3f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	float BackdropHeight = 280.f;

	/** Draw the stop zone as a glowing spotlight circle on the screen instead of a floor plate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	bool bZoneAsSpotlight = false;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	FLinearColor SpotlightColor = FLinearColor(9.0f, 8.2f, 6.0f);

	/** Optional material for flat-coloured parts; needs a vector parameter "Color". Null = /Game/Huebox/Materials/M_HueUnlit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skin|Backdrop")
	TSoftObjectPtr<UMaterialInterface> FlatMaterial;
};

/** Official skin: paper-cut silhouette dancer and her partner in front of a backlit paper screen. */
UCLASS(meta = (DisplayName = "Silhouette Dancer"))
class HUEBOX_API UHueSkin_Silhouette : public UHuePerformerSkin
{
	GENERATED_BODY()

public:
	UHueSkin_Silhouette();
};

/** Prototype skin: the coloured carriage with a mannequin on top (kept as a debug / reference look). */
UCLASS(meta = (DisplayName = "Prototype Carriage"))
class HUEBOX_API UHueSkin_Carriage : public UHuePerformerSkin
{
	GENERATED_BODY()

public:
	UHueSkin_Carriage();
};
