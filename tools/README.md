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
