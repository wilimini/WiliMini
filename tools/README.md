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

## 内存配置主机检查

```sh
sh tools/test-memory-profiles.sh
```

检查加载器的机型/内存选择边界、两个客户端的设置默认值和保存迁移，
并用 ASan/UBSan 检查 RGB 队列复用、清理和启动失败路径。
这些检查不代表 PSP-1000 真机兼容性或播放性能。

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
