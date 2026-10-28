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

Headless 启动器使用生产客户端和当前网络配置；截图路径可通过 `BILI_HEADLESS_SCREENSHOT` 指定。
默认通过 `--appendconfig` 设置 PSP-1000；可用 `BILI_HEADLESS_MODEL=2000` 或 `3000`
切换为扩展内存机型。需要支持 `--appendconfig`、`--memstick`、`--screenshot-at`、
`--press` 和 `--me-core` 的 Headless 构建，旧构建不满足测试条件。
检查日志中的 `LOADER` 和 `CLIENT` 行，确认实际选择的机型与内存配置。
PPSSPP 启动与 UI 测试不能替代真实 ME 播放验证。

## 内存配置主机检查

```sh
sh tools/test-memory-profiles.sh
```

检查加载器的机型/内存选择边界、两个客户端的设置默认值和保存迁移，
并用 ASan/UBSan 检查 RGB 队列复用、清理和启动失败路径。
这些检查不代表 PSP-1000 真机兼容性或播放性能。
