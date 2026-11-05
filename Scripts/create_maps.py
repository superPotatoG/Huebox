"""
Creates the maps next to Lvl_Tutorial (see create_tutorial_map.py), each an empty non world-partition level with
lights, sky and the game mode as the world-settings override:

  Lvl_MainMenu  title screen - AHueMenuGameMode (no pawn; the dancer performs behind the menu)
  Lvl_Exhibit   Level 2      - AHueboxGameMode + AHueExhibitBuilder + a player start
  Lvl_Sandbox   sandbox      - AHueSandboxGameMode + AHueSandboxBuilder + a player start

Re-runnable: an existing map is loaded and its actors rebuilt. Run headless:
  "D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>/Huebox.uproject" ^
      -run=pythonscript -script="<project>/Scripts/create_maps.py" -unattended -nosplash -nullrhi
or from the editor: Tools > Execute Python Script.
"""
import unreal

level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def spawn(cls, location, rotation=unreal.Rotator(0.0, 0.0, 0.0), label=None):
    actor = actors.spawn_actor_from_class(cls, location, rotation)
    if actor is None:
        raise RuntimeError("Huebox: failed to spawn %s" % cls)
    if label:
        actor.set_actor_label(label)
    return actor


def open_or_create(path):
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.log("Huebox: %s exists - rebuilding its actors" % path)
        if not level_editor.load_level(path):
            raise RuntimeError("Huebox: could not load " + path)
        old = [a for a in actors.get_all_level_actors() if not isinstance(a, (unreal.WorldSettings, unreal.Brush))]
        if old:
            actors.destroy_actors(old)
    elif not level_editor.new_level(path, False):
        raise RuntimeError("Huebox: could not create " + path)


def add_lighting():
    # All movable: the project disables static lighting.
    sun = spawn(unreal.DirectionalLight, unreal.Vector(0, 0, 800), unreal.Rotator(roll=0.0, pitch=-50.0, yaw=40.0), "Sun")
    sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun.light_component.set_editor_property("atmosphere_sun_light", True)

    sky_light = spawn(unreal.SkyLight, unreal.Vector(0, 0, 600), label="SkyLight")
    sky_light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.light_component.set_editor_property("real_time_capture", True)

    spawn(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0), label="SkyAtmosphere")
    spawn(unreal.ExponentialHeightFog, unreal.Vector(0, 0, -100), label="HeightFog")


def make_map(path, game_mode, builder=None, player_start=False):
    open_or_create(path)
    add_lighting()
    if player_start:
        # North of the board, facing south (world -Y) so +X reads left-to-right, as in the tutorial.
        spawn(unreal.PlayerStart, unreal.Vector(300, 1200, 120), unreal.Rotator(roll=0.0, pitch=0.0, yaw=-90.0), "PlayerStart")
    if builder:
        spawn(unreal.load_class(None, builder), unreal.Vector(0, 0, 0), label="Builder")

    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.load_class(None, game_mode))

    if not level_editor.save_current_level():
        raise RuntimeError("Huebox: could not save " + path)
    unreal.log("Huebox: created " + path)


make_map("/Game/Huebox/Maps/Lvl_MainMenu", "/Script/Huebox.HueMenuGameMode")
make_map("/Game/Huebox/Maps/Lvl_Exhibit", "/Script/Huebox.HueboxGameMode", "/Script/Huebox.HueExhibitBuilder", True)
make_map("/Game/Huebox/Maps/Lvl_Sandbox", "/Script/Huebox.HueSandboxGameMode", "/Script/Huebox.HueSandboxBuilder", True)
