"""
Creates /Game/Huebox/Materials/M_HueUnlit: an unlit material whose emissive colour is the vector parameter "Color".
Used by skins for flat colours (silhouettes = black, the backlit paper screen and spotlight = values above 1 glow).

Run headless (editor closed):
  "D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>/Huebox.uproject" ^
      -run=pythonscript -script="<project>/Scripts/create_materials.py" -unattended -nosplash -nullrhi
"""
import unreal

FOLDER = "/Game/Huebox/Materials"
NAME = "M_HueUnlit"
PATH = FOLDER + "/" + NAME

lib = unreal.MaterialEditingLibrary

if unreal.EditorAssetLibrary.does_asset_exist(PATH):
    unreal.log("Huebox: %s already exists - rebuilding its graph" % PATH)
    material = unreal.EditorAssetLibrary.load_asset(PATH)
    lib.delete_all_material_expressions(material)
else:
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(NAME, FOLDER, unreal.Material, unreal.MaterialFactoryNew())

material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
# Silhouettes are skeletal meshes: without this usage flag a packaged game silently falls back to the default material.
material.set_editor_property("used_with_skeletal_mesh", True)

color = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -400, 0)
color.set_editor_property("parameter_name", "Color")
color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
lib.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

lib.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(PATH)
unreal.log("Huebox: created " + PATH)
