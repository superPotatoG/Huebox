#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HueboxSettings.generated.h"

class UHuePerformerSkin;

/**
 * Project Settings > Game > Huebox. Stored in Config/DefaultGame.ini.
 * The skin list is what the sandbox (and the Hue.Skin console command) offers; DefaultSkin is used by every rig
 * whose own SkinClass is empty.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Huebox"))
class HUEBOX_API UHueboxSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UHueboxSettings();

	virtual FName GetCategoryName() const override { return TEXT("Game"); }

	/** Skin used by the official levels. */
	UPROPERTY(Config, EditAnywhere, Category = "Skins")
	TSoftClassPtr<UHuePerformerSkin> DefaultSkin;

	/** All selectable skins (built-in ones are always available even if this list is empty). */
	UPROPERTY(Config, EditAnywhere, Category = "Skins")
	TArray<TSoftClassPtr<UHuePerformerSkin>> Skins;

	/** Resolved default skin class (falls back to the silhouette skin). */
	static TSubclassOf<UHuePerformerSkin> GetDefaultSkinClass();
	/** Built-in skins + configured skins, de-duplicated, in that order. */
	static void GetAvailableSkins(TArray<TSubclassOf<UHuePerformerSkin>>& OutSkins);
};
