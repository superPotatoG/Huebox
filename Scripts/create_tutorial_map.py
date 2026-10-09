"""
Creates /Game/Huebox/Maps/Lvl_Tutorial: an empty (non world-partition) level with lights, sky, a player start,
the AHueTutorialBuilder (which builds the three Level 1 exercise boards at BeginPlay) and the Huebox game mode
as the world-settings override.

Run headless (editor closed):
  "D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>/Huebox.uproject" ^
      -run=pythonscript -script="<project>/Scripts/create_tutorial_map.py" -unattended -nosplash -nullrhi
or from the editor: Tools > Execute Python Script.
"""
import unreal

MAP_PATH = "/Game/Huebox/Maps/Lvl_Tutorial"

level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    unreal.log_warning("Huebox: %s already exists - it will be replaced" % MAP_PATH)
    unreal.EditorAssetLibrary.delete_asset(MAP_PATH)

if not level_editor.new_level(MAP_PATH, False):
    raise RuntimeError("Huebox: could not create " + MAP_PATH)


def spawn(cls, location, rotation=unreal.Rotator(0.0, 0.0, 0.0), label=None):
    actor = actors.spawn_actor_from_class(cls, location, rotation)
    if actor is None:
        raise RuntimeError("Huebox: failed to spawn %s" % cls)
    if label:
        actor.set_actor_label(label)
    return actor


# Lighting (all movable: the project disables static lighting and uses Lumen).
sun = spawn(unreal.DirectionalLight, unreal.Vector(0, 0, 800), unreal.Rotator(roll=0.0, pitch=-50.0, yaw=40.0), "Sun")
sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sun.light_component.set_editor_property("atmosphere_sun_light", True)

sky_light = spawn(unreal.SkyLight, unreal.Vector(0, 0, 600), label="SkyLight")
sky_light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky_light.light_component.set_editor_property("real_time_capture", True)

spawn(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), label="SkyAtmosphere")
spawn(unreal.ExponentialHeightFog, unreal.Vector(0, 0, -100), label="HeightFog")

# Player starts north of exercise 1, facing south (world -Y) so +X reads left-to-right.
spawn(unreal.PlayerStart, unreal.Vector(300, 1750, 120), unreal.Rotator(roll=0.0, pitch=0.0, yaw=-90.0), "PlayerStart")

builder_class = unreal.load_class(None, "/Script/Huebox.HueTutorialBuilder")
spawn(builder_class, unreal.Vector(0, 0, 0), label="TutorialBuilder")

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
world_settings = world.get_world_settings()
world_settings.set_editor_property("default_game_mode", unreal.load_class(None, "/Script/Huebox.HueboxGameMode"))

if not level_editor.save_current_level():
    raise RuntimeError("Huebox: could not save " + MAP_PATH)

unreal.log("Huebox: created " + MAP_PATH)
