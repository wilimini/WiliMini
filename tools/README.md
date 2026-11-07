# 开发与验证工具

从仓库根目录运行以下命令。客户端构建、打包和资源生成统一通过根目录 Makefile 调用 CMake；`scripts/` 保留资源生成和构建清单的 Python 脚本。

## PPSSPP：`tools/ppsspp/`

- `build-ppsspp-headless.sh`：构建用于回放的 PPSSPPHeadless。
- `run-client-headless.sh`：启动客户端，支持按键回放、截图和隔离 memstick。
- `test-psp-playback.py`：通过 PPSSPP 验证生产播放器的 MP4、DASH、FLV 路径；名字中的 PSP 不表示真机测试。

```sh
sh tools/ppsspp/build-ppsspp-headless.sh
sh tools/ppsspp/run-client-headless.sh
```

Headless 启动器运行生产版客户端，并为每次运行创建独立的 memstick。
它支持机型配置、截图、按键回放和音频导出；名字中的 PSP 不表示真机测试。

### 基本运行

```sh
sh tools/ppsspp/build-ppsspp-headless.sh
sh tools/ppsspp/run-client-headless.sh
```

`BILI_HEADLESS_MODEL` 控制模拟的 PSP 机型：

| 值 | 模拟机型 | 客户端 profile |
|---|---|---|
| `1000`（默认） | PSP-1000 | 12 MiB 客户端 |
| `2000` | PSP-2000 | 扩展内存客户端 |
| `3000` | PSP-3000 | 扩展内存客户端 |

启动器通过 `--appendconfig` 设置机型，通过 `--memstick` 使用隔离的
memstick。所用 Headless 构建必须支持 `--appendconfig`、`--memstick`、
`--screenshot-at` 和 `--press`；不支持这些参数的旧构建不能用于本流程。
检查日志中的 `LOADER` 和 `CLIENT` 行，确认实际机型、内存和客户端 profile。

### 播放后端

默认后端是原生 ME：

```text
BILI_HEADLESS_PLAYER=psp-native
```

运行 native ME 前，需要：

1. 客户端按默认配置构建；
2. PPSSPP 启用 ME 模拟；
3. 设置 `BILI_HEADLESS_FIRMWARE_DIR`，指向解出的 `flash0` 目录；
4. 该目录包含所选机型的 `kd/resource/meimg.img`（PSP-1000）或
   `kd/resource/me_t2img.img`（PSP-2000/3000）。

旧的 `sceMpeg` 后端是可选兼容路径。它必须使用以下配置构建和运行：

```sh
cmake -S . -B build/client -DBILI_PPSSPP_SCE_MPEG=ON
BILI_HEADLESS_PLAYER=ppsspp-sceMpeg \
  sh tools/ppsspp/run-client-headless.sh
```

该模式不加载 native ME bridge，但 Headless 仍需使用启用了 FFmpeg 的构建。
`BILI_HEADLESS_PLAYER` 必须是 `psp-native` 或 `ppsspp-sceMpeg`。

### UI 夹具

`tools/ppsspp/seed-ui-fixture.py MEMSTICK [--tiles | --metro] [--dark]` 向隔离的 memstick 写入离线夹具：
缓存的首页数据、生成的封面、本地历史和稍后观看，以及可选的 `settings.json`（`--tiles` 或 `--metro` 选择首页样式）。
数据为合成数据，用于在没有账号和网络时检查 UI。按键回放的 MASK 是十六进制 sceCtrl 掩码，
例如 `0x4000`（×）、`0x2000`（○）、`0x10`/`0x40`/`0x80`/`0x20`（上/下/左/右）。

评论页需要账号和网络数据，离线时可用诊断构建：`cmake -S . -B build/client -DBILI_UI_FIXTURE=ON`（默认关闭）。
在 memstick 的客户端目录写入 `ui-fixture.txt`，内容为 `comments` 或 `replies`，客户端启动后直接进入带示例评论的评论页或回复页。
该选项只用于模拟器截图，不要用于发布包。

帧耗时：`-DBILI_UI_FRAME_STATS=ON`（默认关闭）会在日志里为每一帧写一行 `UIFRAME <阶段> <微秒>`，
是该帧的 CPU 绘制时间（不含 vblank 等待），阶段 E/H/L 为磁贴主页（进入/空闲/启动磁贴），P/S 为页面（进入/稳定）。
模拟器里的时间按指令数计，不含真机上读取 VRAM 等的额外开销，只能作为下限和前后对比。

GE 绘制：界面全部由 GE 绘制（见 `docs/ui-rendering.md`），同一标志还会每帧输出
`UIGE sprites=… batches=… list_bytes=… glyph_misses=… restarts=… wait_us=…`：精灵数、批次数、显示列表用量、
字形图集未命中数、列表重启次数和等待 GE 的时间。封面解码在独立线程完成，每次解码记录为 `cover decode … us=…`
（`UI` 通道，需要 `log_level` 为 `debug`）。

### 输出和诊断

- `BILI_HEADLESS_SCREENSHOT`：指定截图路径。
- `BILI_HEADLESS_SCREENSHOT_AT`：指定截图时间和路径，例如 `5.0:results/home.png`。
- `BILI_HEADLESS_PRESS_SCRIPT`：指定按键回放脚本。
- `BILI_HEADLESS_LOG=1`：保存完整 PPSSPP 日志。
- `BILI_HEADLESS_DUMP_AUDIO=/absolute/path/output.wav`：导出混音音频。
- `BILI_HEADLESS_FIRMWARE_DIR`：native ME 使用的解出 `flash0` 目录。
- `BILI_HEADLESS_CPU`：可选 CPU 核心，例如 `jit-ir`。
- `BILI_HEADLESS_JIT_PROFILE`：可选 JIT 热点报告路径；需 runner 支持 `--jit-profile`。
- `BILI_HEADLESS_MEMSTICK`：指定 memstick；默认使用 `build/headless/memstick`。

自定义 runner 默认从相邻的 `assets/flash0/font` 查找字体。
播放器测试构建和覆盖范围见 [PPSSPP 播放后端](../docs/ppsspp-player.md)。
PPSSPP 启动与 UI 测试不能替代真实 ME 播放验证。

## 全部测试与 CI

`sh tools/test-host.sh` 依次运行全部主机检查（内存配置、日志、ME 桥、HTTP 传输、弹幕、字幕、播放控件）。
`python3 tools/ppsspp/test-ui-smoke.py` 在 PPSSPP 中以四种首页样式启动客户端并检查截图，不需要账号、网络或固件。
`python3 tools/ppsspp/test-ime-smoke.py` 在 PPSSPP 中用方向键在内置拼音键盘上输入 “nihao”，检查光标所在按键和候选词栏。
`.github/workflows/test.yml` 在每个 PR 上运行：主机检查；以及 PPSSPP（固定提交，开启 ME 模拟）+ 原生 ME 的
播放套件（PSP-1000 与 Slim 两种机型）、播放控件回放和 UI 冒烟测试。ME 镜像由
`tools/ppsspp/prepare-me-firmware.py` 在运行时从官方 6.61 更新包解出并校验 SHA-256，不缓存、不上传、不打包；
如需换镜像源，设置仓库变量 `PSP_661_UPDATER_URL`（HTTPS）。
这些结果是模拟器结果，不代表真机行为或性能。

## 内存配置主机检查

```sh
sh tools/test-memory-profiles.sh
```

检查加载器的机型/内存选择边界、两个客户端的设置默认值和保存迁移，
并用 ASan/UBSan 检查 RGB 队列复用、清理和启动失败路径。
这些检查不代表 PSP-1000 真机兼容性或播放性能。

## 日志主机检查

```sh
sh tools/test-logging.sh
```

用 ASan/UBSan 检查 `platform/src/wili_log.c` 的行格式、按通道过滤、级别配置串、
环形缓冲、多线程写入和超长截断，确认加载器使用的无环形缓冲配置能编译，
并检查 `results/run-NNN` 的命名与清理规则。流式文件写入（`wili_log_sink.c`）
另外在 ASan/UBSan 和 ThreadSanitizer 下检查顺序、突发丢行计数、轮转、开关和重启。
该检查只在主机运行，不覆盖 PSP 构建或真机输出。

## ME 桥故障检查

```sh
sh tools/test-me-bridge.sh
```

使用硬件调用替身与 ASan/UBSan 检查固件表选择、DMA 超时前的状态保存、
任务超时后缓冲所有权、入口恢复和诊断环回绕。此检查不运行真实 ME 或 DMA。
同一脚本还检查 Custom Core 的启动探针、连续播放、任务/停止超时和重试拒绝；
SRAM 启动、缓存操作、电源锁及固件恢复仍需真机验证。
发给真机测试者的完整安装包由 `make package` 生成；播放后收集的
`diagnostics.txt` 及字段说明见 [播放器故障日志](../decoder/README.md#播放故障日志)。

## 热门视频性能数据集

[`benchmarks/bilibili/`](../benchmarks/bilibili/README.md) 提供冻结的 20 个热门视频
中部约 10 秒的原始 H.264/AAC DASH 双轨采集、校验、独立诊断包准备、
PPSSPP 批量播放及 CSV 报告。支持现有 H.264 内部阶段采样和可选的 PPSSPP
JIT 热点输出；真机 PSPLink、硬件计数器及 gprof 的能力与限制见该目录的
`profiling.md`。媒体和每轮结果仅保存在本地。
## 播放控件回放

`tools/ppsspp/test-playback-controls.py` 使用 `BILI_DEVICE_PLAYBACK_TEST=ON` 的测试包，
默认构建目录为 `build/playback-controls`，包目录为
`build/playback-controls-dist/PSP/GAME/WiliMini`。可通过
`BILI_HEADLESS_CLIENT_DIR` 和 `BILI_HEADLESS_PACKAGE` 覆盖。
需配置 native ME 固件及 `PPSSPP_HEADLESS`；脚本生成媒体、按键回放和验证 JSON。

`BILI_HEADLESS_REALTIME=1` 让启动器以实时速度运行，适用于网络下载测试。
`tools/serve-playback-fixture.py --video <mp4> --audio <m4a>` 提供限定文件的 HTTP Range
测试服务，运行结束后关闭。它不验证 Bilibili CDN 或真实画质切换。

`test-playback-controls.py` 的 `async-overlays`、`overlay-cancel`、`overlay-failure`
使用延迟传输替身验证首帧后加载、只请求选中字幕、取消与错误隔离；不是实际 CDN 测试。
主机请求策略和错误提示检查分别为 `tests/playback_request_policy_test.c`、
`tests/playback_error_test.cpp`。
