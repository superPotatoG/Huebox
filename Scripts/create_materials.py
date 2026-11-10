"""
Creates the project materials under /Game/Huebox/Materials (rebuilds their graphs if they exist):
  M_HueUnlit   unlit, emissive = vector parameter "Color". Flat colours of skins (values above 1 glow).
  M_HuePuppet  unlit, two-sided, emissive = vertex colour x "Color". Shadow-puppet pieces: the vertex colour is the
               dye of the hide (or the weight of a carved opening), "Color" the light coming through it.

Run headless (editor closed):
  "D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>/Huebox.uproject" ^
      -run=pythonscript -script="<project>/Scripts/create_materials.py" -unattended -nosplash -nullrhi
"""
import unreal

FOLDER = "/Game/Huebox/Materials"
lib = unreal.MaterialEditingLibrary


def open_material(name):
    path = FOLDER + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.log("Huebox: %s already exists - rebuilding its graph" % path)
        material = unreal.EditorAssetLibrary.load_asset(path)
        lib.delete_all_material_expressions(material)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        material = tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    return material, path


def color_parameter(material, x, y):
    color = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    return color


def finish(material, path):
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("Huebox: built " + path)


def build_unlit():
    material, path = open_material("M_HueUnlit")
    # Silhouettes are skeletal meshes: without this usage flag a packaged game silently falls back to the default material.
    material.set_editor_property("used_with_skeletal_mesh", True)
    color = color_parameter(material, -400, 0)
    lib.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material, path)


def build_puppet():
    material, path = open_material("M_HuePuppet")
    # Pieces turn round (mirrored) when the puppet changes direction: both faces must draw.
    material.set_editor_property("two_sided", True)
    vertex = lib.create_material_expression(material, unreal.MaterialExpressionVertexColor, -600, 0)
    color = color_parameter(material, -600, 200)
    multiply = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -300, 100)
    lib.connect_material_expressions(vertex, "", multiply, "A")
    lib.connect_material_expressions(color, "", multiply, "B")
    lib.connect_material_property(multiply, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material, path)


build_unlit()
build_puppet()
