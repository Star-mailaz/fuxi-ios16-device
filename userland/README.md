# fuxi 用户态注入（软件渲染 profile）

launchd → backboardd → SpringBoard 这一段的 fuxi 侧代码。它的存在只为一件事：让窗口服务器
在没有 GPU 驱动的前提下用 CoreAnimation 自己的软件光栅器作画，并把画面交给 CPU 呈现路径。

## 这里有什么

| 文件 | 作用 |
| --- | --- |
| `inject/fuxi_soft_render.h` | profile 开关（`FUXI_RENDERER`，只允许 `FUXI_RENDERER_SOFTWARE`）与 20H392 共享缓存的地址表 |
| `inject/fuxi_soft_render.c` | backboardd：渲染器请求、显示更新钩子、ALS 等待、containermanagerd 的 personas |
| `inject/fuxi_springboard_soft.c` | SpringBoard：vibrant 视图开关（锁屏时钟变白）、离开屏幕时的快照格式（JPEG/PNG，不用 ASTC） |
| `inject/fuxi_activation.c` | SpringBoard：mobileactivationd 缺席时的「已激活」回答、设置助手永不进入（按 ObjC 类名/selector 定位，不用共享缓存地址） |
| `rc.jobs.daemons` | 到达桌面所需的最小 launchd job 子集（21 个 stock label，按 bring-up 顺序） |
| `inject.conf` | 「哪个库注入哪几个程序」的表，运行时的库路径是 payload 里的 `/fuxi/lib/...` |
| `build-softrender-userland.py` | 构建这两个 dylib 的设备脚本；在非 macOS 主机上直接拒绝运行 |
| `NOTICE.md` | 每个片段的出处、固定提交、许可证 |

激活链的定位方式是 ObjC 运行时（`objc_getClass` + `class_getInstanceMethod`），不是共享缓存里的地址：类名和 selector 名在同一个 iOS 大版本里比地址稳定，地址每次缓存构建都会变；状态 ivar 的偏移也从类元数据读，不写死 `+8`。找不到类、方法或 ivar 时只打一行日志、不挂钩，不会写错位置。

## 这一层怎么接进构建 profile

规格书第 5 节要求「软件渲染 profile 是配置，不是注释掉的 GPU 代码」。这里的落点是三处，都是配置：

1. `board.toml` 的 `[components]` 不含 `Adreno740`/`AdrenoMetal`/`AdrenoPM4`/`AdrenoFirmware`/`ArmSmmu4K`/`Sm8550Platform`——GPU 驱动模块在同一构建中被不选入，不是在源文件里被 `#if 0` 掉。
2. `configs/fuxi_softrender_defconfig` 是渲染器 profile 本身；选 GPU 时是另一个 defconfig 加另一份组件表。
3. 本目录的 `FUXI_RENDERER` 是第三个落点：`#if FUXI_RENDERER != FUXI_RENDERER_SOFTWARE` 直接 `#error`，所以这个库不可能在没意识到的情况下带着 GPU 路径编译出来。

验证命令（在 UniXNU 主仓库执行）：

```sh
./unixnu defconfig fuxi_softrender_defconfig -D ../fuxi
./unixnu status --json -D ../fuxi | grep -c Adreno     # 期望 0
```

## 现状：它能做什么，不能做什么

能做的：

- 编译出两个 dylib 并把「库 → 程序」的表交给 payload 的 launchd job 集（步骤本身在 macOS 上跑；Linux 主机会拒绝）。
- 在 backboardd 里拒绝第二次渲染器请求，让它落到 `sw_renderer()`；ALS 的五秒等待被移除；SpringBoard 的 vibrant 视图置为 plain。
- 让 SpringBoard 把设备当作已激活，并且不进入设置助手（`SBLockdownManager` / `SBSetupManager` 的两个 hook，按类名和 selector 装）。

还不能做的（这一层最大的缺口，写在这里而不是藏在代码里）：

1. **20H392 的共享缓存地址还没填。** `fuxi_soft_render.h` 的 `fuxi_cache_16_7_16` 全是 0，`fuxi_replace_virtual()` 于是逐个跳过并打日志：窗口服务器不会崩，但显示更新钩子没有挂上，快照格式钩子也没装。填法是对 iPhone10,3 的 16.7.16 (20H392) 共享缓存做符号查找，再用 `replace_virtual()` 自带的一致性检查验证——表项写错时它只打一行「found …, expected …; left alone」，不会改内存。
2. **激活绕过已剥离，但还没在 20H392 上验证过。** `fuxi_activation.c` 按类名/selector 装 `-[SBLockdownManager _setupActivationState]` 和 `-[SBSetupManager _setSetupRequiredReason:]`，把状态写成「已激活」并把设置助手的原因压成 0；找不到类、selector 或 `_state` int ivar 时只打一行日志、退回 SpringBoard 自己的行为。这两个都是 SpringBoard 的私有方法，名字不保证跨版本不变，所以在真机看到锁屏之前不能算完成。地址表里没有它们（运行时定位），因此这里也没有 20H392 的常量要填。
3. **触摸还没有来源。** 上游自己造了一个虚拟 HID 触摸服务；fuxi 的 `NativeHidInput` 还没接到注入层，所以现在只能看，不能操作。

## 与 §6 验收的关系

这三条缺口决定 §6 的第 2 条（屏幕上出现锁屏或主屏幕）能否成立：第 1 条不成立时窗口服务器的呈现路径没有被观测和接管；第 2 条的代码已经在了，但 20H392 上的两个私有方法名和状态 ivar 还没有在真机上证实，所以「仍然到不了锁屏」是可能的结果；第 3 条只影响可操作性，不影响「有画面」。按规格书，帧率、触摸、Wi-Fi、音频、摄像头都不在验收条件里，所以第 3 条不是阻塞项。

## 不变量

- 不引入上游的构建系统：库怎么编、装到哪，由本目录的脚本和 `board.toml` 的 payload 策略决定。
- 不改上游文件再提交：剥离的地方在本目录重写，出处记在 `NOTICE.md`。
- 不堆未启用的源文件：`inject/` 里只有这三个 `.c`，且每个都在 `build-softrender-userland.py` 的 `SOURCES` 里；GPU 路径的代码不在这里。
