# Huebox · C++ 实现说明（第一部分：C++ 知识、内存管理、机制实现、按键绑定）

> 对应代码：`Source/Huebox/`（UE 5.8.3，C++ 模块 `Huebox`）。所有路径相对于项目根目录。

---

## 1. 用到的 C++ / Unreal C++ 知识点

| 类别 | 知识点 | 在项目里的位置 |
|---|---|---|
| 面向对象 | 继承 + 虚函数多态：模拟器只调用基类 `APulseBlock::ReceivePulse`，子类各自实现 | [PulseBlock.h:64](../Source/Huebox/Blocks/PulseBlock.h#L64)，[HueModuleBlocks.h](../Source/Huebox/Blocks/HueModuleBlocks.h)，[HueFixedBlocks.h](../Source/Huebox/Blocks/HueFixedBlocks.h) |
| 接口 | `UINTERFACE` + `BlueprintNativeEvent`（拾取物和方块都实现 `IHueInteractable`） | [HueInteractable.h](../Source/Huebox/Player/HueInteractable.h) |
| 值类型 | `USTRUCT FPulseData`：可随意拷贝、不含 UObject，GC 不需要追踪 | [HueTypes.h](../Source/Huebox/Core/HueTypes.h) |
| 枚举 / 命名空间 | `enum class` + 命名空间工具函数（方向旋转、颜色交换） | `HueDir::`、`HueColor::` |
| 数据结构 | 最小堆事件队列（`TArray::HeapPush/HeapPop` + 自定义比较器）、`TMap` 网格占用表、`TInlineAllocator` | [PulseSubsystem.h](../Source/Huebox/Simulation/PulseSubsystem.h)，[HueBoard.h:136](../Source/Huebox/Grid/HueBoard.h#L136) |
| 确定性 | 逻辑时间用 `int64` 毫秒（不用浮点），同一时刻的事件按 (时间, 优先级, 序号) 全序排列 | [PulseSubsystem.cpp:229](../Source/Huebox/Simulation/PulseSubsystem.cpp#L229) |
| RAII / 作用域守卫 | `TGuardValue` 防止回调重入；测试里用 RAII 结构体创建和销毁测试世界 | [PulseSubsystem.cpp:215](../Source/Huebox/Simulation/PulseSubsystem.cpp#L215)，[HueSimulationTests.cpp](../Source/Huebox/Tests/HueSimulationTests.cpp) |
| 函数对象 | `TFunctionRef` 回调（`ForEachBlock`、`PredictMotion` 每步回调） | [ReactiveRig.h](../Source/Huebox/Rig/ReactiveRig.h) |
| 委托 | 动态多播委托 `OnFeedback`、`OnInventoryChanged`、`OnStateChanged`（给 UMG/蓝图用） | `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` |
| 子系统 | `UTickableWorldSubsystem`：生命周期与 UWorld 绑定的全局模拟器 | [PulseSubsystem.h](../Source/Huebox/Simulation/PulseSubsystem.h) |
| 物理 | Chaos 刚体 + SixDOF 轴锁 + 加速度力 + 线性阻尼 + 运行时物理材质 | [ReactiveRig.cpp:162-168](../Source/Huebox/Rig/ReactiveRig.cpp#L162) |
| 输入 | Enhanced Input：在 C++ 运行时 `NewObject` 创建 InputAction / MappingContext / Modifier | [HueInputConfig.cpp](../Source/Huebox/Player/HueInputConfig.cpp) |
| 自动化测试 | `IMPLEMENT_SIMPLE_AUTOMATION_TEST`，共 22 个，覆盖规则、计时、重置、内存、教程可解性 | [HueSimulationTests.cpp](../Source/Huebox/Tests/HueSimulationTests.cpp) |

---

## 2. 内存管理（重点）

Unreal 的 `UObject` **不使用** `new/delete`，也不靠引用计数，而是由引擎的**标记-清除垃圾回收（GC）**管理：

1. **根集**：被 `AddToRoot()` 标记的对象、世界里存活的 Actor 等。
2. **标记**：GC 从根集出发，只沿着 **`UPROPERTY()` 标记的成员**（以及 `AddReferencedObjects` 报告的引用）去找可达对象。
3. **清除**：找不到的对象会被销毁。所以**只存在于裸指针或普通 C++ 容器里的 UObject 会被回收，裸指针随即变成悬空指针**。

本项目遵守的规则，以及对应的代码：

| 规则 | 为什么 | 代码 |
|---|---|---|
| **拥有的对象 = `UPROPERTY() TObjectPtr<>`** | 让 GC 能看见引用，保证对象存活 | 运行时创建的输入配置 [HueboxCharacter.h:134](../Source/Huebox/Player/HueboxCharacter.h#L134)；物理材质 [ReactiveRig.h:157](../Source/Huebox/Rig/ReactiveRig.h#L157)；材质实例 `BodyMID`；运行时端口标记组件 `PortMarkers` |
| **容器里的 UObject 也要 `UPROPERTY`** | `TMap/TArray` 只有标了 UPROPERTY，GC 才会遍历它们 | 网格占用表 `UPROPERTY() TMap<FIntPoint, TObjectPtr<APulseBlock>> Occupancy` [HueBoard.h:136](../Source/Huebox/Grid/HueBoard.h#L136) |
| **不拥有的引用 = `TWeakObjectPtr<>`** | 弱指针不阻止回收；对象销毁后 `.Get()` 返回 null，不会悬空 | 方块指向棋盘的反向指针 [PulseBlock.h:156](../Source/Huebox/Blocks/PulseBlock.h#L156)；事件队列里的方块引用 [PulseSubsystem.h:118](../Source/Huebox/Simulation/PulseSubsystem.h#L118)；玩家当前瞄准的目标 [HueboxCharacter.h:34](../Source/Huebox/Player/HueboxCharacter.h#L34) |
| **值类型不进 GC** | `FPulseData`、事件结构体是纯数据，拷贝成本低，不产生 GC 负担 | `FPulseData`、`FSimEvent`、`FTravelingPulse` |
| **对象池** | 避免每个脉冲都 Spawn/Destroy 一个 Actor（GC 抖动） | `FreeVisuals` 池 [PulseSubsystem.h:192](../Source/Huebox/Simulation/PulseSubsystem.h#L192) |
| **生命周期收尾** | Actor 被销毁后，定时器和委托回调不能再调用到它 | `EndPlay` 中 `ClearAllTimersForObject` [PulseBlock.cpp:326](../Source/Huebox/Blocks/PulseBlock.cpp#L326)；`RemoveDynamic` [ExhibitManager.cpp:137](../Source/Huebox/Game/ExhibitManager.cpp#L137)；子系统 `Deinitialize` 清空所有待处理事件 [PulseSubsystem.cpp:49](../Source/Huebox/Simulation/PulseSubsystem.cpp#L49) |
| **定时器只绑 UObject 方法** | 引擎能检测对象是否还活着；捕获裸 `this` 的 lambda 不行 | `SetTimer(FlashTimer, this, &APulseBlock::EndFlash, …)` [PulseBlock.cpp:217](../Source/Huebox/Blocks/PulseBlock.cpp#L217) |
| **回调中容器可能重新分配** | 方块（混合器）在回调里发射新脉冲，`TMap` 可能扩容，旧指针失效 | 先拷贝数据、回调后重新 `Find` [PulseSubsystem.cpp:371-377](../Source/Huebox/Simulation/PulseSubsystem.cpp#L371) |
| **不用 static `TStrongObjectPtr`** | 静态对象在 UObject 系统关闭之后才析构，会崩溃 | 引擎基础形状用 `static TWeakObjectPtr` + `LoadObject` 缓存 |
| **重置不泄漏状态** | 每次尝试都必须从同一个状态开始 | `ResetAttempt` 清空脉冲、缓冲、事件、目标和小车；有测试验证 `Huebox.Simulation.Reset` |

另外有两个专门的内存测试：`Huebox.Memory.RemovedBlockIgnoresEvents`（方块被移除后，排队的事件不会再调用它）和 `Huebox.Memory.WorldTeardownReleasesObjects`（世界销毁后，弱指针全部失效）。

---

## 3. GDD 机制如何实现

```
玩家 (AHueboxCharacter) ──LMB 放置/回收──▶ AHueBoard（网格 + 占用表 + 目标）
                                              │ 注册/查询
Space ─▶ AExhibitManager（Build/Running/Paused/Ended/Complete 状态机）
              │ StartRun
              ▼
        UPulseSubsystem（逻辑时间事件队列）──ReceivePulse()──▶ APulseBlock 子类
              │                                              Turn / ColorShift / Delay / Mixer
              │                                              Source / Receiver / Barrier / Brick
              └─ Receiver 在“同一时刻组”结束时 ─▶ AReactiveRig.ApplyColor(A/B/N)
                                                     └─ Chaos 物理小车（+ 人偶）
```

- **脉冲**：每格 250 ms（4 格/秒）。每个脉冲是一个“到达下一格”的事件，空格子直接继续前进，因此在空地交叉的光束互不影响。越出棋盘或撞到障碍物即消失。
- **同一时刻分组**：同一毫秒的所有事件都处理完后，接收器才判断收到的颜色。颜色不同则判定为冲突，状态保持不变。混合器的 0.5 s 窗口两端都算在内：到达事件的优先级高于超时事件。
- **颜色模型（10-22 起，加色光）**：每种颜色是红 / 绿 / 蓝三原色的一个集合，用 3 位掩码表示（`HueColor::ToMask`）。A = 红，B = 青（绿 + 蓝），N = 白（三色），另外还有 G 绿、Y 黄（红 + 绿）、M 品红（红 + 蓝）、V 紫（蓝）。规则都由掩码推出来：
  - **混合器**（`HueColor::Mix`）：两种颜色没有共同原色时相加（按位或），例如 A + B = N、A + G = Y；有共同原色就失败（A + A、任何颜色 + N、A + Y）。原来的“A + B → N”只是其中一种情况。
  - **变色块**（`HueColor::Shift`）：变成互补色（按位取反），A ↔ B、G ↔ M、Y ↔ V；N 不变。
  - **只有 A / B / N 改变舞者的运动**（`HueColor::DrivesRig`）。其他颜色是“钥匙”：到达接收器后不参与同一时刻的冲突判定，而是在这一时刻结束时交给舞者（`AReactiveRig::PerformKeyAction`），她当场做这个颜色的动作。动作只是皮肤里的一段动画（`PerformerKeyActionAnim`），物理不受影响。
  - **舞台板子**（目标 `BreakBoard`）：舞者的 Tick 和 `PredictMotion` 里都把没打碎的板子当作一道挡板；跑到接触线就停下、播放倒地动画，`HasCrashed()` 变为真，`AExhibitManager` 立即判这一轮失败（`HandleRunFailed`）。钥匙动作只有发生在出拳区（例如 60–68%）时才会打碎同色板子。舞者记录出拳时的位置，`MakeSample` 把它带给棋盘做判定。测试用 `PredictMotion` 离线扫描每个延迟值：会打碎板子，还是撞上去。
- **砖**（`AHueBrickBlock`）：只有同色的光能打碎，打碎它的那束光会被用掉；其他颜色被挡住。打碎在“同一时刻组”结束时（`OnTimeGroupEnd`）才生效，所以同一毫秒到达的其他脉冲仍会被挡，结果与事件队列的处理顺序无关。重置后恢复原样。
- **颜色 → 物理**：A 同色相斥，B 异色相吸，均为 `AddForce(..., bAccelChange=true)` 并限速；N 关闭颜色力并把阻尼调高，逐渐刹停。小车用 SixDOF 锁住 Y/Z 平移和全部旋转，只能沿轨道运动。
- **限制**：最多 16 个活跃信号；每个信号最多 64 次模块操作，超过就判定为循环并终止；每次运行最长 20 s。
- **胜利条件**：目标必须按顺序完成，一开始就站在目标区里不算。
- **可验证**：`AReactiveRig::PredictMotion` 用与物理相同的模型做离线积分。测试把逻辑模拟得到的颜色时间线喂进去，再检查四个教程练习的目标都能完成。

---

## 4. 按键绑定（Enhanced Input，纯 C++）

项目里没有输入资产，全部在 [HueInputConfig.cpp](../Source/Huebox/Player/HueInputConfig.cpp) 中运行时创建：

1. `NewObject<UInputAction>()` 创建动作，设置 `ValueType`（Boolean / Axis1D / Axis2D）。
2. `NewObject<UInputMappingContext>()` 创建映射上下文，用 `MapKey(Action, EKeys::W)` 绑键，并给映射加 Modifier：
   - W：`SwizzleAxis(YXZ)`，把 (1,0) 变成 (0,1)，即前进
   - S：`Swizzle` + `Negate`，后退
   - A：`Negate`，左移
   - 鼠标：`Negate(Y)`，避免上下颠倒
   - 数字键 1–6：共用一个 Axis1D 动作，用 `Scalar(n)` 区分槽位号
3. 角色在 `NotifyControllerChanged` 里把上下文加入 `UEnhancedInputLocalPlayerSubsystem`（[HueboxCharacter.cpp:262](../Source/Huebox/Player/HueboxCharacter.cpp#L262)），并在 `SetupPlayerInputComponent` 里执行 `BindAction`（[HueboxCharacter.cpp:328-344](../Source/Huebox/Player/HueboxCharacter.cpp#L328)）。移动和视角用 `Triggered`（每帧触发），按键用 `Started`（按下触发一次）。
4. 所有 Action 的 Outer 是 `UHueInputConfig`，Modifier 的 Outer 是 MappingContext。`UHueInputConfig` 又被角色的 `UPROPERTY` 持有，所以整条链都不会被 GC 回收。

| 按键 | 功能 |
|---|---|
| **W A S D** | 移动 |
| 鼠标 | 视角 |
| **鼠标左键** | 交互：拾取地上的模块 / 回收已放置的模块 / 把选中的模块放到瞄准的空格子上 |
| Q / E | 把瞄准的方块（或放置预览）旋转 -90° / +90° |
| Z / X | 把瞄准的延迟块调 -0.25 s / +0.25 s |
| 滚轮、1–6 | 选择背包槽位 |
| G | 把选中的模块丢一个到地上 |
| Space | 开始 / 暂停 / 继续当前所在棋盘的运行（GDD 规定；所以不设跳跃） |
| R | 停止并重置本次尝试（布局保留） |
| H | 显示 / 隐藏帮助 |
| Esc / P | 暂停菜单（在编辑器 PIE 中 Esc 会直接退出 PIE，请用 P） |

---

## 5. 与 GDD 不同的地方（有意为之）

- **视角**：按你的要求改为第一人称 WASD 行走。GDD 原本是环绕工作台的相机。仍然没有可见的玩家身体，符合 GDD 第 5 节。
- **转向块**：改为双向直角，两个相邻面都是端口。这样只用 Q/E 旋转就能同时得到左转和右转，否则需要额外的“镜像”键。
- **HUD**：原型阶段用 Canvas 绘制，不需要任何 UMG 资产。以后可以替换为 UMG，委托已经预留好。
- **美术**：全部使用引擎基础形状占位；GameMode 里的 `ModuleClasses` 可以换成带美术的蓝图子类。
