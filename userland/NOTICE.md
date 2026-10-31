# NOTICE — 剥离出处与许可证

本目录的代码不是从零写的，是从两个上游项目里剥离、改写出来的。规格书第 4 节的硬约束是
「保留原许可证头与出处注释；不引入原项目的构建系统，只取源文件级逻辑」，本文件记录每一条
的实际来源、固定版本和许可证。

## 固定版本

| 项目 | 仓库 | 固定提交 | 许可证 |
| --- | --- | --- | --- |
| HakiOS userland | `oosexgang/hackios-xnu-userland`（已并入 `oosexgang/hackios-xnu-kernel`，`main`） | `a3384e031eb1e605cf37adaefc19991f04f6274b`（2026-10-30） | MIT（`userland/src/` 部分，见上游 `LICENSE` 第 2 条） |
| ios-sweet | `dipa-studio/ios-sweet` | `45263bee51df4a0f55cdf9c649af886399488ddb`（2026-10-23） | MIT |
| HakiOS 指针仓库 | `oosexgang/hackios-xnu-userland` | `2573c4a8d80ed143e534d16b5a386efa5b06ccbb`（2026-10-25，只有 README，指向上面那条） | MIT |

两份上游都按第 7 节的要求先找 GitCode / AtomGit 镜像：UniXNU 主线有镜像
（`https://gitcode.com/gh_mirrors/un/UniXNU`，`gitcode.com/UniXNU/UniXNU` 会 301 过去），
但这两个项目**没有**镜像（`gh_mirrors/io/ios-sweet`、`gh_mirrors/ha/hackios-xnu-userland` 都是 403）。
因此按「镜像和缓存是工程手段」把仓库整体拉了一份到工作区的
`.cache/upstream/`（`ios-sweet`、`hakios`），此后所有剥离都从这份本地缓存读，不再联网。

## 逐文件对应

| 本目录的文件 | 上游来源 | 取用范围 | 改动 |
| --- | --- | --- | --- |
| `inject/fuxi_soft_render.c` | `hakios/userland/src/inject.c` | `say()`、`replace_virtual()`、`opengl_es()` 的决策逻辑（改成 profile 常量）、`window_server_fixes()` 的六个 vtable 槽、`my_renderer()` 的一次拒绝、`brightness_fixes()` 的 ALS 等待、`container_manager_fixes()` 的 personas 回答 | 去掉 eagl/Mesa 加载与全部 `gl_*` 观测钩子；地址改成 `fuxi_cache_offsets` 表（20H392 待填，0 即不挂钩）；日志前缀 `hakios:` → `fuxi:` |
| `inject/fuxi_springboard_soft.c` | `hakios/userland/src/inject_sb.c` | `plain_vibrancy()` 的 `BSUIVibrancyEffectViewDisabled` 开关、`snapshot_output_format()` 的 JPEG/PNG 选择 | `gpu_draws()` 变成 profile 常量 `false`；快照钩子的安装因为缺 20H392 类地址而显式跳过并打一行日志 |
| `rc.jobs.daemons` | `hakios/userland/build.sh` 的 `DARWIN_JOBS` | 那 21 个 label 及其顺序 | 去掉 `com.apple.wifid`（验收不含 Wi-Fi），注释改写 |
| `inject.conf` | `hakios/userland/build.sh` 的 `INJECT` | 「库 → 程序」这张表的组织方式 | 只留软件渲染的两条，去掉 `libhakios.gl/media/app/tweaks` |
| `build-softrender-userland.py` | `hakios/userland/build.sh` | `build.sh` 里 `GPU=`/`GL=` 那个开关的**做法**（选项决定选入哪些库、job list 怎么组织） | 完全重写：本仓库自己的构建脚本，不引入上游的 Makefile/环境变量体系；只编两个 dylib，并在编译前断言 profile 不是 GPU |

## 原许可证头

上游 `inject.c` / `inject_sb.c` 的文件头是说明性注释而不是 SPDX/copyright 块，许可在仓库根
`LICENSE`（MIT，Copyright (c) 2025-2026 the HakiOS authors）。本仓库据此在每个改写文件头里写明
「adapted from HakiOS, oosexgang/hackios-xnu-userland, src/inject.c (MIT)」和 HakiOS 的版权与许可，
并在这里附上 MIT 全文的指向：`.cache/upstream/hakios/LICENSE`。ios-sweet 的 `LICENSE`（MIT,
Copyright (c) 2026 dipa-studio）在 `.cache/upstream/ios-sweet/LICENSE`。

## 没有取用的部分（明确列出，避免被当成已完成）

- `hakios` 的 `src/eagl.c`、`userland/libmini`、`mkeagl.py`、OpenGL ES/Mesa/Turnip/Metal 相关：属于 GPU 路径，规格书 3.2 排除。
- `inject_sb.c` 里的激活绕过（mobileactivationd）、设置助手跳过、电池/触摸管线、调试通知：属于用户态启动工作，尚未剥离，见 `README.md` 的待办。
- `ios-sweet` 的 `xnu/patches/0046-sweet-iosurface-iomfb.patch`（IOSurface + IOMobileFramebuffer 软件实现）与 `0056/0061/0068/0079` 的后续修饰：本阶段的显示后端走 UniXNU 的 `GopFramebuffer` 组件，因此没有把这三个内核文件搬进来；需要时从上表那两个提交重新取。
