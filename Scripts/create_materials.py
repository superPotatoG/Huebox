"""
Creates the project materials under /Game/Huebox/Materials (rebuilds their graphs if they exist):
  M_HueUnlit   unlit, emissive = vector parameter "Color". Flat colours of skins (values above 1 glow).
  M_HuePuppet  unlit, two-sided, emissive = vertex colour x "Color". Shadow-puppet pieces: the vertex colour is the
               dye of the hide (or the weight of a carved opening), "Color" the light coming through it.
  M_HueScreen  unlit, the backlit paper screen: "Color" x paper texture x oil-lamp pools x flicker (world space).
  M_HueGlowPool  unlit additive soft disc (plane mesh UVs): the stop / strike zones as pools of lamp light.

Run headless (editor closed):
  "D:/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<project>/Huebox.uproject" ^
      -run=pythonscript -script="<project>/Scripts/create_materials.py" -unattended -nosplash -nullrhi
"""
import unreal

FOLDER = "/Game/Huebox/Materials"
lib = unreal.MaterialEditingLibrary


# Existing materials are left alone: deleting the graph of an already loaded material can trip an engine assertion
# (!IsRooted). Delete the asset first to rebuild one.
class Skip(Exception):
    pass


def open_material(name):
    path = FOLDER + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.log("Huebox: %s exists - kept (delete it to rebuild)" % path)
        raise Skip()
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


SCREEN_HLSL = r"""
struct FHuePaper
{
    float Hash(float2 q) { return frac(sin(dot(q, float2(127.1, 311.7))) * 43758.5453); }
    float Noise(float2 q)
    {
        float2 i = floor(q);
        float2 f = frac(q);
        float2 u = f * f * (3.0 - 2.0 * f);
        return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
    }
};
FHuePaper Paper;
float2 q = P.xz;
// Paper: soft clouds, fine grain and long fibres running along the screen.
float clouds = Paper.Noise(q * 0.012) * 0.6 + Paper.Noise(q * 0.045) * 0.4;
float grain = Paper.Noise(q * 0.9);
float fibre = Paper.Noise(float2(q.x * 0.015, q.y * 0.9));
float paper = 0.90 + 0.08 * clouds + 0.03 * grain - 0.05 * smoothstep(0.6, 0.95, fibre);
// Oil lamps behind the screen every LampSpacing cm: brighter pools, darker between and towards top and bottom.
float lx = frac(q.x / LampSpacing) - 0.5;
float dz = (q.y - LampHeight) / 300.0;
float lamp = 0.72 + 0.38 * exp(-(lx * lx * 7.0 + dz * dz * 6.0));
// A slight flicker of the flames.
float flicker = 1.0 + 0.022 * sin(T * 7.3) + 0.014 * sin(T * 12.7 + q.x * 0.003) + 0.01 * sin(T * 3.1);
return C * (paper * lamp * flicker);
"""

POOL_HLSL = r"""
// A soft pool of light: bright core, gentle falloff, nothing at the square's edge.
float r = length(UV - 0.5) * 2.0;
float core = 1.0 - smoothstep(0.55, 0.92, r);
float halo = (1.0 - smoothstep(0.0, 1.0, r)) * 0.35;
return C * saturate(core * 0.75 + halo);
"""


def custom_node(material, code, inputs, x, y):
    custom = lib.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("code", code)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    pins = []
    for name in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", name)
        pins.append(pin)
    custom.set_editor_property("inputs", pins)
    return custom


def scalar_parameter(material, name, value, x, y):
    param = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    param.set_editor_property("parameter_name", name)
    param.set_editor_property("default_value", value)
    return param


def build_screen():
    """M_HueScreen: the backlit paper screen (亮子). Emissive = Color x paper texture x lamp pools x flicker."""
    material, path = open_material("M_HueScreen")
    custom = custom_node(material, SCREEN_HLSL, ["P", "C", "T", "LampSpacing", "LampHeight"], -300, 0)
    world = lib.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -700, -150)
    color = color_parameter(material, -700, 0)
    time = lib.create_material_expression(material, unreal.MaterialExpressionTime, -700, 150)
    spacing = scalar_parameter(material, "LampSpacing", 420.0, -700, 250)
    height = scalar_parameter(material, "LampHeight", 150.0, -700, 350)
    lib.connect_material_expressions(world, "", custom, "P")
    lib.connect_material_expressions(color, "", custom, "C")
    lib.connect_material_expressions(time, "", custom, "T")
    lib.connect_material_expressions(spacing, "", custom, "LampSpacing")
    lib.connect_material_expressions(height, "", custom, "LampHeight")
    lib.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material, path)


def build_pool():
    """M_HueGlowPool: an additive soft disc of light for the stop / strike zones on the screen (on a plane mesh)."""
    material, path = open_material("M_HueGlowPool")
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    custom = custom_node(material, POOL_HLSL, ["UV", "C"], -300, 0)
    uv = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -700, -100)
    color = color_parameter(material, -700, 50)
    lib.connect_material_expressions(uv, "", custom, "UV")
    lib.connect_material_expressions(color, "", custom, "C")
    lib.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    finish(material, path)


for build in (build_unlit, build_puppet, build_screen, build_pool):
    try:
        build()
    except Skip:
        pass
