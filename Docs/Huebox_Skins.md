# Huebox · 表演者皮肤（Skin）系统

> **皮肤只负责“看起来是什么”，物理完全不变。** 所有皮肤共用同一个物理体：沿轨道运动，R 推、C 拉、W 刹车。
> 所以换皮肤不影响关卡能不能解，自动化测试和 `-HueSelfTest` 的结果也一样。

## 内置皮肤

| 皮肤 | 类 | 说明 |
|---|---|---|
| **Shadow Puppets 皮影**（官方，默认，11-10 起） | `UHueSkin_ShadowPuppet` | 皮影戏台上的旦角舞者和拿扇子的生角舞伴。她身上的镂空处显示她当前的颜色，舞伴的镂空永远是红色（R）。舞伴会回应她收到的颜色：R 用扇子把她推开，C 招手叫她回来，W 点头。见下面“皮影” |
| Silhouette Dancer | `UHueSkin_Silhouette` | 背光纸屏前的剪影舞者和舞伴（用模板人偶）。舞者面朝运动方向小跑，静止时待机；纸屏会微微染上当前的信号颜色；停止区画成纸屏上的聚光灯 |
| Prototype Carriage | `UHueSkin_Carriage` | 原型阶段的彩色小车加人偶，保留作调试和参考 |

## 皮影（Shadow Puppets）

**形状在一个 Python 脚本里，动画在 C++ 里。** 不需要外部美术软件，也不需要贴图。

- **形状**：[Scripts/puppets/gen_puppets.py](../Scripts/puppets/gen_puppets.py)。每个皮影由十来片“皮”组成（下身、上身、头、上臂、前臂、水袖、腿、步摇、飘带……），每片在关节处有一个轴心。每片皮有两层：
  - 染色的皮和墨线（第 0 段）：真皮影是半透明的，所以颜色 = 染料 × 透过纸屏的灯光；
  - 镂空（第 1 段）：透出来的光 = 她当前的信号颜色。
  
  花纹用辅助函数画：`cloud`（云纹）、`flower`、`lace`（在一片区域里铺满小镂空：菱形、钱纹、四瓣花、鱼鳞）、`knot`（关节上的线结）。
- **运行脚本**（用引擎自带的 Python）：
  ```
  "D:/Epic Games/UE_5.8/Engine/Binaries/ThirdParty/Python3/Win64/python.exe" Scripts/puppets/gen_puppets.py --png preview.png --zoom closeup.png --tiles tiles.png
  ```
  它会重新生成 `Source/Huebox/Rig/HuePuppetShapes.inl`（不要手改），并且不开引擎就能渲染预览图：`--png` 是几个姿势，`--zoom` 是近景，`--tiles` 是模块图案。改完形状要重新编译。
- **动画**：[Rig/HueShadowPuppet.cpp](../Source/Huebox/Rig/HueShadowPuppet.cpp) 的 `ComputePose()`，全部是代码驱动：
  - 转向时像艺人把皮影翻过来（镜像经过侧面）；
  - 步伐随速度变化；被推时身体前倾，被拉或刹车时后仰；
  - 水袖、步摇、飘带和帽带都挂在阻尼弹簧上，随加速度甩动；
  - 钥匙动作是甩袖，撞上窗棂时向后倒下；
  - 三根操纵杆（颈杆和两根手杆）伸到台下。
- **台上的锁**：舞台板改成窗棂道具（`Pane`），镂空透出钥匙的颜色，打碎时在纸屏平面里碎成木片和亮片。
- **戏台**（`bShowTheatre`）：红漆台柱、带雕花横楣的台楣、舞者站的台沿、挡住操纵杆的栏板（停止区和出击区的说明文字写在栏板上），两侧有帘子，后面是暗下来的后台。纸屏换成 `M_HueScreen`（纸纹、油灯光斑、轻微闪烁），停止区和出击区是 `M_HueGlowPool` 的柔光。
- **模块图案**也由同一个脚本生成（`TileTurn`、`TileShift`……），躺在每个方块的木板上，见 `APulseBlock::TileArt`。

代码在 [Rig/HuePerformerSkin.h](../Source/Huebox/Rig/HuePerformerSkin.h)。rig 怎样根据皮肤生成组件，见 [ReactiveRig.cpp](../Source/Huebox/Rig/ReactiveRig.cpp) 里的 `ApplySkin()`。

## 皮肤可以设置的内容（全是数据）

| 分组 | 字段 | 作用 |
|---|---|---|
| 基本 | `DisplayName`、`PerformerName` | 列表里显示的名字；HUD 上的称呼，比如 "Dancer" |
| Rig parts | `bShowCarriageBody`、`bShowAnchorBlock`、`StructureColor`、`bLettersInSignalColor` | 是否显示小车方块和锚点方块；轨道和挡块的颜色；A/B/N 字母是否用信号颜色 |
| Performer | `PerformerMesh`、`PerformerMoveAnim`、`PerformerIdleAnim`、`PerformerKeyActionAnim`、`PerformerCrashAnim`、`PerformerScale`、`bFaceMotionDirection`、`bFlatPerformerColor`、`PerformerColor` | 舞者的骨骼网格、移动动画（播放速度随移动速度变化）、待机动画、**钥匙动作**（黄光等钥匙颜色到达时播放一次，例如舞步、出拳、剑术；留空则只在舞者身上闪出颜色字母）、**撞板倒地动作**（撞上没打碎的板子时播放一次并停在最后一帧；留空则只是停住）、大小、是否转身面向运动方向、是否涂成纯色（剪影） |
| Partner | `bShowPartner`、`PartnerMesh`、`PartnerAnim` | 锚点处的舞伴 |
| Backdrop | `bShowBackdrop`、`BackdropColor`、`BackdropSignalTint`、`BackdropHeight`、`bZoneAsSpotlight`、`SpotlightColor`、`FlatMaterial`、`bShowTheatre`、`TheatreLacquer`、`CurtainColor` | 背光纸屏的亮度和颜色、信号染色程度、高度；停止区是否画成聚光灯；纯色部件使用的材质；是否搭起戏台，以及漆色和帘子颜色 |
| Puppet | `PerformerPuppet`、`PartnerPuppet`、`PuppetScale`、`PuppetScreenGap`、`PuppetTransmission`、`PuppetGlow` | 用哪个皮影代替舞者和舞伴的网格；大小（生成的尺寸约 125 cm 高）；离纸屏多近；纸屏的光有多少透过染色的皮；镂空处的亮度 |

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
- **材质**都在 `/Game/Huebox/Materials`，由 `Scripts/create_materials.py` 生成（已存在的不会覆盖，要重建就先删掉那个资源）：
  - `M_HueUnlit`：纯色和发光（无光照，参数名 `Color`，数值大于 1 会发光）；
  - `M_HuePuppet`：皮影和模块图案（顶点颜色 × `Color`，双面）；
  - `M_HueScreen`：戏台纸屏；
  - `M_HueGlowPool`：叠加的柔光圆。
  
  如果你自己做材质：
  - 也要提供一个名为 `Color` 的 Vector 参数；
  - 用在骨骼网格上时，必须勾选 **Used with Skeletal Mesh**，否则打包后会退回默认材质。
- **自己做的舞者网格**最好和 UE Mannequin 共用同一套骨骼。这样可以直接用 Mannequin 的动画，或者从 Mixamo 下载跳舞动画后重定向过来。
- **打包**时如果皮肤资源只靠软引用、没有被任何关卡直接引用，要把它所在的文件夹加进 `Project Settings → Packaging → Additional Asset Directories to Cook`。

## 和沙盒模式的关系
沙盒里按 **K** 依次切换 `UHueboxSettings::GetAvailableSkins()` 里的皮肤（内置皮肤加上 Project Settings 里的列表）。所以按上面的方法添加的皮肤会自动出现在沙盒里，不需要再改代码。
