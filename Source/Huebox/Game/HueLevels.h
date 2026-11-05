#pragma once

#include "CoreMinimal.h"

/**
 * The game's maps (created by Scripts/create_maps.py). The title screen opens the levels; each level's pause menu
 * leads back to it. All of them are listed under MapsToCook in DefaultGame.ini.
 */
namespace HueLevels
{
	/** Title screen: AHueMenuGameMode, the dancer performing behind the menu. The packaged game starts here. */
	inline const TCHAR* const MainMenu = TEXT("/Game/Huebox/Maps/Lvl_MainMenu");
	/** Level 1, the prototype table: four exercises (AHueTutorialBuilder). */
	inline const TCHAR* const Tutorial = TEXT("/Game/Huebox/Maps/Lvl_Tutorial");
	/** Level 2, the exhibition box (AHueExhibitBuilder). */
	inline const TCHAR* const Exhibit = TEXT("/Game/Huebox/Maps/Lvl_Exhibit");
	/** Free building in first person (AHueSandboxGameMode, AHueSandboxBuilder). */
	inline const TCHAR* const Sandbox = TEXT("/Game/Huebox/Maps/Lvl_Sandbox");
}
