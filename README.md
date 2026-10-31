<p align="center">
    <img src="assets/ICON0.svg" alt="WiliMini logo" height="128" width="128"/>
</p>
<h1 align="center">
WiliMini
</h1>
<p align="center">
  一个运行在 Sony PlayStation Portable（PSP）上的第三方
  <a href="https://www.bilibili.com">B站</a> 客户端
</p>
<p align="center">
  <b><a href="#特点">特点</a></b>
  |
  <b><a href="#安装">安装</a></b>
  |
  <b><a href="#文档">文档</a></b>
  |
  <b><a href="#开发">开发</a></b>
</p>

- - -

[![GitHub release](https://img.shields.io/github/v/release/wilimini/WiliMini)](https://github.com/wilimini/WiliMini/releases)
[![GitHub license](https://img.shields.io/github/license/wilimini/WiliMini)](LICENSE)
![PSP](https://img.shields.io/badge/PlayStation-Portable-003791?style=flat&logo=PlayStation)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat&logo=cplusplus)

WiliMini 是一个面向 PSP 的原生 Bilibili 客户端。网络请求、界面渲染、视频下载、
音视频解码和显示都在 PSP 本机完成，不依赖外部串流设备。

项目以接近 wiliwili 的浏览体验为目标，并针对 PSP 的内存、带宽和 Media Engine
进行适配。目前主要在 PSP-3000 + ARK-5 上验证。

> （2026-10-31 更新）已在 PSP-1000，PSP-2000上验证。

# 特点

WiliMini 让 PSP 也可以直接浏览 B站内容：

- **原生运行**：使用 PSP 网络连接，在设备本地渲染和解码
- **内容浏览**：推荐、热门、搜索、动态、用户空间、番剧、影视和收藏
- **视频播放**：支持 360p H.264/AAC，支持 DASH 和 MP4
- **播放体验**：全屏控制栏、画质选择、弹幕、可用字幕轨道、观看历史与播放进度
- **PSP 优化**：使用 VFPU、DMA 和 Media Engine 优化性能
- **模拟器支持**：可以在启用 Media Engine 的 PPSSPP 中运行和验证
- **主题切换**：支持深色和浅色主题
- **语言选项**：设置页和按键提示支持简体中文与 English
- **全型号支持**：支持PSP-1000, PSP-2000, PSP-3000, PSP Go

## 性能优化架构

视频播放路径将 CPU 解码和 Media Engine 渲染分工处理，减少主线程等待和显存拷贝：

```mermaid
flowchart LR
    A[网络下载与缓存] --> B

    subgraph CPU["CPU + VFPU"]
        direction TB
        B[FFmpeg<br/>H.264 / AAC 解码]
        C[音频处理<br/>与播放时钟]
        D[视频帧队列]
        B --> C
        B --> D
    end

    subgraph ME["Media Engine"]
        direction TB
        E[YUV → RGB<br/>色彩空间转换]
        F[视频缩放]
    end

    D -- ME 任务 --> E
    E --> F
    C --> H[音频输出]
    F -->|DMA| G[显存] --> I[PSP 屏幕]
```

其中，CPU + VFPU 负责 FFmpeg 解码和音频播放；视频解码后的 YUV 帧进入共享队列，并通过 ME 任务交给 Media Engine。Media Engine 负责色彩空间转换和缩放，处理结果通过 DMA 写入显存。音频播放时钟作为同步基准，CPU 解码与 ME 视频渲染可以并行工作，
减少主 CPU 等待和不必要的显存拷贝。

## 界面截图

<p align="center">
  <img src="docs/screenshots/home.png" alt="首页" width="480">
  <img src="docs/screenshots/pgc.png" alt="番剧列表" width="480">
  <img src="docs/screenshots/video-detail.png" alt="视频详情" width="480">
  <img src="docs/screenshots/settings-light.png" alt="设置（浅色主题）" width="480">
  <img src="docs/screenshots/settings.png" alt="设置（深色主题）" width="480">
</p>

# 安装

### PSP

1. 从 [GitHub Releases](https://github.com/wilimini/WiliMini/releases/latest) 下载
   `WiliMini-<版本号>.zip`。
2. 解压后，将完整的 `WiliMini/` 目录复制到记忆棒或 PSP Go 内置存储的
   `PSP/GAME/` 目录。
3. 从 PSP 游戏菜单启动 WiliMini。

最终路径应为：

```text
ms0:/PSP/GAME/WiliMini/EBOOT.PBP
```

PSP Go 也可以使用：

```text
ef0:/PSP/GAME/WiliMini/EBOOT.PBP
```

### 从源码构建

构建完成后，将整个 `dist/PSP/GAME/WiliMini/` 目录复制到目标设备的
`PSP/GAME/` 目录。必须启动 `EBOOT.PBP`，不要直接加载客户端 PRX；
加载器会初始化网络模块和 Media Engine bridge。

### PPSSPP

PPSSPP 运行需要支持 Media Engine 执行，并启用 `EmulateMediaEngine`。
使用原生 ME 播放路径时，还需要准备对应机型的 PSP `flash0` 固件目录。
具体配置请参考 [PPSSPP 播放后端](docs/ppsspp-player.md)。

# 文档

- [播放器说明](decoder/README.md)：播放器 API、音视频格式和 Media Engine 生命周期
- [播放控制与弹幕](docs/playback-controls.md)：快捷键、画质菜单、跳转和叠加绘制
- [客户端服务说明](docs/client-services.md)：网络服务和客户端模块
- [界面文案目录](docs/localization.md)：静态语言表和生成方式
- [PPSSPP 播放后端](docs/ppsspp-player.md)：模拟器构建、固件和播放测试
- [Media Engine 兼容性分析](docs/me-compatibility.md)：ME bridge 与固件差异
- [内存配置说明](docs/memory-profiles.md)：PSP-1000 与扩展内存配置
- [调试指南](docs/debug.md)：PSPLink、USBHostFS 和真机实验流程
- [PSP SELECT 网络问题分析](docs/PSP-SELECT-NETWORK-BUG.md)
- [PSP 解码优化记录](docs/psp-decode-optimization-journey.md)
- [开发与验证工具](tools/README.md)
- [Media Engine 实验](experiments/me/)

# 功能状态

| 模块 | 状态 |
| --- | --- |
| PSP 客户端构建与打包 | 可用 |
| 推荐、热门和搜索 | 已接入 |
| 视频详情、UP 主信息和评论 | 已接入 |
| 番剧、影视和分集列表 | 已接入 |
| 收藏夹、稍后观看和观看历史 | 已接入 |
| 登录和扫码会话 | 已接入 |
| 360p DASH 播放 | 已接入 |
| 渐进式 MP4 自动回退 | 默认关闭，DASH 失败明确提示 |
| 音频播放 | 已接入 |
| 直播播放 | 暂不支持 |
| 续播和退出恢复 | 部分完成 |
| Media Engine 加速 | 实验性 |

当前主要针对 360p H.264/AAC 内容进行验证。复杂画面可能出现跳帧或停顿。

# 操作说明

| 按键 | 操作 |
| --- | --- |
| 方向键 | 移动焦点、滚动列表 |
| × / ○ | 确认与返回（默认 × 确认，可在设置中切换） |
| △ | 刷新当前列表 |
| □ | 打开搜索；在详情页切换稍后观看 |
| L / R | 切换主标签页；在详情页打开评论 |
| SELECT | 重新打开 PSP Wi-Fi 设置 |
| START | 退出客户端 |

播放时 START 暂停/继续，L/R 跳转 10 秒，左右细调并预览进度，△ 选择弹幕、字幕与画质入口，□ 切换弹幕。
详见 [播放控制与弹幕](docs/playback-controls.md)。

搜索页面使用 PSP 原生屏幕键盘输入关键词。视频详情页支持评论、收藏、
稍后观看和播放进度操作。

# 开发

## 环境准备

需要安装 PSPSDK 和对应的 PSP 工具链，并设置 `PSPDEV`：

```sh
export PSPDEV=/path/to/pspdev
export PATH="$PSPDEV/bin:$PATH"
```

首次克隆时初始化 Git submodule：

```sh
git clone --recurse-submodules https://github.com/wilimini/WiliMini.git
cd WiliMini
```

已有工作区可以运行：

```sh
git submodule update --init --recursive
```

## 构建依赖和客户端

从仓库根目录使用 Makefile。首次构建需要联网下载固定版本的 FFmpeg 和
libcurl 源码包，构建过程会校验 SHA-256 并自动应用 PSP 补丁。

```sh
make dependencies
make build
make package
```

常用目标：

```sh
make curl          # 只构建 PSP curl
make ffmpeg        # 只构建 PSP FFmpeg
make me            # 构建客户端和 Media Engine 组件
make assets        # 生成字体等资源
```

可以通过 `BUILD_DIR`、`BUILD_TYPE`、`PARALLEL` 和 `CMAKE_ARGS` 调整构建：

```sh
BUILD_TYPE=Release PARALLEL=4 make package
```

默认产物位于：

```text
dist/PSP/GAME/WiliMini/
```

支持两个 ME 执行后端
- Custom Core （默认，PSP-1000可用）
- MIST（PSP-2000以上可用）

详见[播放器说明](decoder/README.md#custom-core-执行后端实验性)。

也可以直接使用 CMake：

```sh
psp-cmake -S . -B build/client
cmake --build build/client --target package --parallel 4
```

如果依赖安装在其他目录，可传入 `PSP_FFMPEG_PREFIX` 和 `PSP_CURL_PREFIX`。

## PPSSPP 回放与主机检查

构建 Headless PPSSPP 并运行客户端：

```sh
sh tools/ppsspp/build-ppsspp-headless.sh
sh tools/ppsspp/run-client-headless.sh
```

内存配置和清理路径的主机检查：

```sh
sh tools/test-memory-profiles.sh
```

PPSSPP 回放可以验证 UI、下载和播放器路径，但不能替代真实 PSP Media Engine
播放验证，也不能据此推断 PSP-1000 或其他固件版本的兼容性。

## PSPLink 真机调试

详细实验要求见 [调试指南](docs/debug.md)。每次变更后先生成完整安装包：

```sh
make package
usbhostfs_pc -vv -b 11000 "$PWD"
```

另开终端连接 PSPLink：

```sh
pspsh -p 11000
```

在 `pspsh` 中使用加载器启动：

```text
cd host0:/
./build/client/loader/bootstrap.prx
```

测试前应复制完整的 `dist/PSP/GAME/WiliMini/` 目录到设备。正常退出客户端后，
再次检查线程是否全部结束；不要把 PSPLink 查询超时直接当作设备崩溃。

# 项目结构

```text
client/       Bilibili 客户端、UI、网络服务和播放控制
decoder/      FFmpeg 播放器、音频、VFPU 和 Media Engine 路径
cmake/        依赖、客户端和打包配置
platform/     PSP 平台集成
scripts/      资源生成和构建清单脚本
tools/        PPSSPP 回放、PSP 辅助工具和主机测试
third_party/  固定版本的第三方依赖
docs/         设计文档、故障分析和研究记录
experiments/  独立实验及其复现资料
results/      PPSSPP、真机和性能实验结果
```

# 反馈问题前

1. 确认 PSP 系统时间和网络设置正确。
2. 确认使用的是完整的 `WiliMini/` 安装目录，而不是源码。
3. 先搜索已有的 [Issues](https://github.com/wilimini/WiliMini/issues) 和讨论。
4. 尽量提供完整复现步骤、设备型号、固件/自制系统版本、日志和截图。
5. 如果是 PPSSPP 问题，请同时提供模拟器版本、机型配置和 Media Engine 设置。

# 贡献

欢迎提交 Issue、改进文档和 Pull Request。

提交代码前请：

- 先确认改动对应的功能或问题，并说明测试环境；
- 保持 PSP 特定代码、平台无关服务逻辑和实验性调优相互隔离；
- 不提交账号 Cookie、扫码登录会话、令牌或私人设备日志；
- 使用 `make package` 验证构建，并在可能时补充 PPSSPP 或真机验证结果。

如果要改进播放器、Media Engine、VFPU 或 DMA 路径，请先阅读
[播放器说明](decoder/README.md) 和相关实验文档，特别注意缓冲区所有权、
缓存一致性、线程退出和固件差异。

# 许可证

WiliMini 采用 GNU General Public License v3 协议开源。第三方依赖及其许可证
见安装包中的 `licenses/` 目录和 [致谢](#致谢)。

# 致谢

感谢以下项目及其贡献者：

| 项目 | 使用内容 | 许可证 |
| --- | --- | --- |
| [FFmpeg](https://ffmpeg.org/) | 音视频解码与容器解析 | LGPL-2.1-or-later |
| [curl](https://curl.se/) | PSP 网络请求 | curl license（MIT-like） |
| [PSP Media Engine MIST](https://github.com/mcidclan/psp-media-engine-mist) | Media Engine 集成 | MIT |
| [PSP Media Engine Custom Core](https://github.com/mcidclan/psp-media-engine-custom-core) | Media Engine core mapper 与辅助库 | MIT |
| [Wiliwili](https://github.com/xfangfang/wiliwili) | Bilibili API/result 代码及界面资源 | GPL-3.0-only |
| [WenQuanYi Micro Hei](https://sourceforge.net/projects/wqy/) | 客户端字体 | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析 | MIT |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c) | 登录二维码生成 | MIT |
| [PSPSDK](https://github.com/pspdev/pspsdk) | PSP SDK 兼容代码 | BSD-3-Clause |
