# Huebox · 表演者皮肤（Skin）系统

> **皮肤只负责“看起来是什么”，物理完全不变。** 所有皮肤共用同一个物理体：沿轨道运动，A 推、B 拉、N 刹车。
> 所以换皮肤不影响关卡能不能解，自动化测试和 `-HueSelfTest` 的结果也一样。

## 内置皮肤

| 皮肤 | 类 | 说明 |
|---|---|---|
| **Silhouette Dancer**（官方，默认） | `UHueSkin_Silhouette` | 背光纸屏前的剪影舞者和舞伴。舞伴站在 A 锚点处。舞者面朝运动方向小跑，静止时待机；纸屏会微微染上当前的信号颜色；停止区画成纸屏上的聚光灯 |
| Prototype Carriage | `UHueSkin_Carriage` | 原型阶段的彩色小车加人偶，保留作调试和参考 |

代码在 [Rig/HuePerformerSkin.h](../Source/Huebox/Rig/HuePerformerSkin.h)。rig 怎样根据皮肤生成组件，见 [ReactiveRig.cpp](../Source/Huebox/Rig/ReactiveRig.cpp) 里的 `ApplySkin()`。

## 皮肤可以设置的内容（全是数据）

| 分组 | 字段 | 作用 |
|---|---|---|
| 基本 | `DisplayName`、`PerformerName` | 列表里显示的名字；HUD 上的称呼，比如 "Dancer" |
| Rig parts | `bShowCarriageBody`、`bShowAnchorBlock`、`StructureColor`、`bLettersInSignalColor` | 是否显示小车方块和锚点方块；轨道和挡块的颜色；A/B/N 字母是否用信号颜色 |
| Performer | `PerformerMesh`、`PerformerMoveAnim`、`PerformerIdleAnim`、`PerformerKeyActionAnim`、`PerformerScale`、`bFaceMotionDirection`、`bFlatPerformerColor`、`PerformerColor` | 舞者的骨骼网格、移动动画（播放速度随移动速度变化）、待机动画、**钥匙动作**（黄光等钥匙颜色到达时播放一次，例如舞步、出拳、剑术；留空则只在舞者身上闪出颜色字母）、大小、是否转身面向运动方向、是否涂成纯色（剪影） |
| Partner | `bShowPartner`、`PartnerMesh`、`PartnerAnim` | 锚点处的舞伴 |
| Backdrop | `bShowBackdrop`、`BackdropColor`、`BackdropSignalTint`、`BackdropHeight`、`bZoneAsSpotlight`、`SpotlightColor`、`FlatMaterial` | 背光纸屏的亮度和颜色、信号染色程度、高度；停止区是否画成聚光灯；纯色部件使用的材质 |

**资源找不到时会自动降级。** 网格和动画都是软引用（`TSoftObjectPtr`）。比如从 GitHub clone 下来的项目没有 Epic 人偶，这时会自动改用“圆锥裙子 + 球形头”拼成的纸片人，游戏不会报错。

## 怎样添加一个新皮肤

### 方法 A：蓝图（推荐，不用写 C++）
1. Content Browser → 右键 → **Blueprint Class** → 搜索并选择父类 **HuePerformerSkin**，命名为例如 `BP_Skin_MyDancer`，建议放在 `Content/Huebox/Skins/`。
2. 打开蓝图，在 **Class Defaults** 里填写上表的字段：网格、动画、颜色、是否显示纸屏等。
3. 打开 **Project Settings → Game → Huebox → Skins**，点 `+` 加入 `BP_Skin_MyDancer`。
   - 如果想让它成为官方关卡的默认皮肤，就把 **Default Skin** 也设为它。
   - 这些设置保存在 `Config/DefaultGame.ini`。
4. 运行游戏，按 `~` 打开控制台：
   - `Hue.Skin`：列出所有皮肤和编号；
   - `Hue.Skin 2`，或 `Hue.Skin MyDancer`：给场景里所有舞台换上这个皮肤。

### 方法 B：C++
```cpp
// MySkins.h
UCLASS(meta = (DisplayName = "Wind-up Toy"))
class UHueSkin_WindUp : public UHuePerformerSkin
{
	GENERATED_BODY()
public:
	UHueSkin_WindUp()
	{
		DisplayName = FText::FromString(TEXT("Wind-up Toy"));
		PerformerName = TEXT("Toy");
		PerformerMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TEXT("/Game/MyArt/SKM_Toy.SKM_Toy")));
		bShowBackdrop = false;
	}
};
```
然后同样加进 Project Settings 的 Skins 列表。

### 只给某一个舞台换皮肤
在关卡里选中 `ReactiveRig` → Details → **Hue | Skin → Skin Class**。留空就使用 Project Settings 里的默认皮肤。

### 注意事项
- **纯色和发光**用的是 `/Game/Huebox/Materials/M_HueUnlit`（无光照，参数名 `Color`，数值大于 1 会发光）。用 `Scripts/create_materials.py` 可以重新生成。如果你自己做材质：
  - 也要提供一个名为 `Color` 的 Vector 参数；
  - 用在骨骼网格上时，必须勾选 **Used with Skeletal Mesh**，否则打包后会退回默认材质。
- **自己做的舞者网格**最好和 UE Mannequin 共用同一套骨骼。这样可以直接用 Mannequin 的动画，或者从 Mixamo 下载跳舞动画后重定向过来。
- **打包**时如果皮肤资源只靠软引用、没有被任何关卡直接引用，要把它所在的文件夹加进 `Project Settings → Packaging → Additional Asset Directories to Cook`。

## 和沙盒模式的关系（计划中）
沙盒模式会读取 `UHueboxSettings::GetAvailableSkins()`（内置皮肤加上 Project Settings 里的列表），让玩家在菜单里挑选皮肤。所以按上面的方法添加的皮肤，将来会自动出现在沙盒里，不需要再改代码。
