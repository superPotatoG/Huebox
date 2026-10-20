#include "Core/HueboxSettings.h"

#include "Rig/HuePerformerSkin.h"

UHueboxSettings::UHueboxSettings()
{
	DefaultSkin = UHueSkin_Silhouette::StaticClass();
}

TSubclassOf<UHuePerformerSkin> UHueboxSettings::GetDefaultSkinClass()
{
	const UHueboxSettings* Settings = GetDefault<UHueboxSettings>();
	if (Settings && !Settings->DefaultSkin.IsNull())
	{
		if (UClass* Loaded = Settings->DefaultSkin.LoadSynchronous())
		{
			return Loaded;
		}
	}
	return UHueSkin_Silhouette::StaticClass();
}

void UHueboxSettings::GetAvailableSkins(TArray<TSubclassOf<UHuePerformerSkin>>& OutSkins)
{
	OutSkins.Reset();
	OutSkins.Add(UHueSkin_Silhouette::StaticClass());
	OutSkins.Add(UHueSkin_Carriage::StaticClass());

	if (const UHueboxSettings* Settings = GetDefault<UHueboxSettings>())
	{
		for (const TSoftClassPtr<UHuePerformerSkin>& Entry : Settings->Skins)
		{
			UClass* Loaded = Entry.IsNull() ? nullptr : Entry.LoadSynchronous();
			if (Loaded && !Loaded->HasAnyClassFlags(CLASS_Abstract))
			{
				OutSkins.AddUnique(Loaded);
			}
		}
	}
}
