# WiliMini

![WiliMini Logo](assets/ICON0.svg)

WiliMini 是一个运行在 Sony PlayStation Portable（PSP）上的第三方 B站 客户端。

它使用 PSP 原生网络访问服务，在设备本地完成页面渲染、视频下载、音视频解码和显示，不依赖外部串流设备。

项目的目标是在 PSP 硬件限制下，提供接近 Wiliwili 风格的 B站 浏览体验，并持续研究 H.264/AAC 解码、VFPU、DMA 和 Media Engine 加速。

> 当前项目处于持续开发阶段，当前版本可以运行。
> 仅在 PSP-3000 + ARK-5 上测试。

# 特点

WiliMini 拥有非常接近 wiliwili 客户端的B站浏览体验

- 原生支持PSP：使用PSP连接网络，本地进行解码，不依赖外部设备串流
- 原速播放视频：
  - 跳过参考帧解码，关闭去块滤波，使用双线性近似运动插值
  - 实现`360p H.264 High`视频原速播放，流畅播放`HE-ACC`音频
- 性能优化：使用主CPU解码、副CPU（ME）渲染，DMA写入显存。帧率相比 `ffplay` 基线提升17.8%
- 功能齐全：推荐、热门、动态、用户、收藏、设置等
- 主题色：拥有深浅两色主题

## 界面截图

<p>
  <img src="docs/screenshots/home.png" alt="首页" width="480">
  <img src="docs/screenshots/pgc.png" alt="番剧列表" width="480">
  <img src="docs/screenshots/video-detail.png" alt="视频详情" width="480">
  <img src="docs/screenshots/settings-light.png" alt="视频详情" width="480">
  <img src="docs/screenshots/settings.png" alt="视频详情" width="480">
</p>

## 功能概览

### 内容浏览

- 推荐、热门和搜索
- 视频详情、UP 主信息和投稿列表
- 动态、图文内容和用户空间
- 收藏夹、稍后观看和观看历史
- 番剧、影视和分集列表
- 评论、回复、楼中楼和分页加载
- 消息通知和私信入口

### 播放功能

- 360p H.264/AAC 视频播放
- DASH 视频和音频分离下载
- HTTP Range 按需读取
- 渐进式 MP4 回退
- 视频缓存和观看进度保存
- PSP 原生音频输出
- FLV 直播播放实验

### PSP 特性

- PSP 原生网络连接
- PSP 原生屏幕键盘搜索
- 深色和浅色主题
- 中文点阵字体
- 封面缓存
- VFPU、DMA 和 Media Engine 渲染实验

## 当前状态

| 模块 | 状态 |
| --- | --- |
| PSP 客户端构建 | 可构建 |
| 推荐、热门、搜索 | 已接入 |
| 视频详情和评论 | 已接入，部分线上流程待验证 |
| 360p DASH 播放 | 已接入 |
| 渐进式 MP4 回退 | 已接入 |
| 音频播放 | 已接入，持续验证中 |
| 登录和扫码会话 | 已接入，失效恢复待验证 |
| 番剧和影视页面 | 已接入，播放流程待验证 |
| 直播播放 | 实验性，仍有失败样本 |
| 续播和退出恢复 | 部分完成 |
| Media Engine 加速 | 实验性 |
| PSP-1000 兼容性 | 尚未完成验证 |

详细进度和已知限制见
[当前开发进度](docs/CURRENT-PROGRESS.md)。

## 构建

### 环境准备

需要安装 PSPSDK 和对应的 PSP 工具链，并设置 `PSPDEV`：

```sh
export PSPDEV=/path/to/pspdev
export PATH="$PSPDEV/bin:$PATH"
```

两个 Media Engine 源码库作为 Git submodule 固定版本。首次克隆时初始化它们：

```sh
git clone --recurse-submodules <repo-url>
```

如果主仓库已经克隆，则在仓库根目录运行：

```sh
git submodule update --init --recursive
```

FFmpeg 和 libcurl 不使用 submodule。`make dependencies` 会下载固定版本的源码包、
校验 SHA-256 并自动应用补丁；也可以分别运行 `make ffmpeg` 和 `make curl`。
FFmpeg 保持原 n6.1.2 对应的 commit，不下载 Git 历史。
下载缓存位于 `build/downloads/`，解压源码位于 `build/ffmpeg-source/` 和
`build/curl-source/`，需要联网完成首次下载。

客户端依赖 PSP 版本的 FFmpeg、libcurl 和 Media Engine 组件。默认情况下，
CMake 从以下目录查找已构建的依赖：

```text
build/ffmpeg-audio-install/
build/curl-install/
third_party/psp-media-engine-mist/
third_party/psp-media-engine-custom-core/
```

### 构建客户端

使用项目根目录的 Makefile。默认目标会先准备依赖，再编译打包：

```sh
make
```

Makefile 会调用 CMake 编排配置、编译和打包流程。常用目标包括：

```sh
make dependencies  # 构建 PSP curl 和 FFmpeg
make curl          # 只构建 curl
make ffmpeg        # 只构建 FFmpeg
make build         # 编译客户端
make package       # 编译并生成 dist/PSP/GAME/WiliMini
make me            # 编译客户端和 Media Engine 组件
make assets        # 生成已配置的字体资源
```

通过 `BUILD_DIR`、`BUILD_TYPE` 和 `PARALLEL` 调整构建目录、类型和并行度；
需要传入额外 CMake 选项时使用 `CMAKE_ARGS`。

也可以直接使用 CMake：

```sh
psp-cmake -S . -B build/client
cmake --build build/client --target package --parallel 4
```

构建完成后，打包目录为：

```text
dist/PSP/GAME/WiliMini/
```

如果依赖安装在其他目录，可以通过以下 CMake 选项指定：

```sh
psp-cmake -S . -B build/client \
  -DPSP_FFMPEG_PREFIX=/path/to/ffmpeg \
  -DPSP_CURL_PREFIX=/path/to/curl
cmake --build build/client --target package --parallel 4
```

### 安装到 PSP

从 [GitHub Releases](https://github.com/wilimini/WiliMini/releases/latest) 下载
`WiliMini-<版本号>.zip` 并解压。将解压得到的 `WiliMini/` 整个目录复制到记忆棒的
`PSP/GAME/`，最终路径应为 `ms0:/PSP/GAME/WiliMini/EBOOT.PBP`。然后从 PSP 游戏菜单启动
WiliMini。

如果从源码构建，则将 `dist/PSP/GAME/WiliMini/` 整个目录复制到记忆棒的
`PSP/GAME/`。

目录中的主要文件包括：

- `EBOOT.PBP`：加载器，负责启动网络模块和 Media Engine bridge。
- `wili_mini.prx`：24 MiB 堆的客户端，默认 8 帧播放缓冲。
- `wili_mini_1000.prx`：12 MiB 堆的客户端，默认 2 帧播放缓冲。
- `cacert.pem`：CA 证书集合。
- `cache/`：封面和视频缓存。
- `licenses/`：第三方许可证。
- `build.json`：源文件和产物的 SHA-256 清单。

不要直接加载客户端 PRX。客户端依赖加载器初始化的网络模块和
`me_bridge.prx`。

加载器根据机型和可用连续内存自动选择客户端：PSP-1000 使用 12 MiB 版本；
后续机型至少有 36 MiB 连续空闲内存时使用 24 MiB 版本，否则回退到小堆版本。
两个 PRX 必须一起安装。SFO 保留 `MEMSIZE=1`；它用于后续机型的扩展用户内存，
不会让 PSP-1000 获得额外物理内存。实际支持取决于固件；PPSSPP 的 PSP-1000 模式会忽略该标志。

设置中的“播放缓冲”可选择 2、4、8 帧，分别占用约 1.06、2.13、4.25 MiB RGB 内存。
缓冲在每次播放开始时分配，播放结束后释放，不逐帧申请和释放。
已保存的设置优先于对应客户端的默认值。堆大小是分配上限，不代表实时占用，也不包含
程序代码、静态数据、系统模块和线程等开销。内存配置完成不代表 PSP-1000 的 ME 播放已验证；
目前真机验证仍限于 PSP-3000，ME 桥保留固件匹配检查。

## 操作说明

| 按键 | 操作 |
| --- | --- |
| 方向键 | 移动焦点、滚动列表 |
| × | 打开详情、播放、确认 |
| ○ | 返回、取消 |
| △ | 刷新当前列表 |
| □ | 打开搜索；在详情页切换稍后观看 |
| L / R | 切换主标签页；在详情页打开评论 |
| SELECT | 重新打开 PSP Wi-Fi 设置 |
| START | 退出客户端 |

搜索页面使用 PSP 原生屏幕键盘输入关键词。视频详情页支持评论、收藏、
稍后观看和播放进度操作。

## 开发调试

通过 PSPLink 调试时，可以使用 USBHostFS 将项目目录映射到 PSP：

```sh
usbhostfs_pc -vv -b 11000 "$PWD"
pspsh -p 11000
```

在 `pspsh` 中启动加载器：

```text
./build/client/loader/bootstrap.prx
```

加载器从 `host0:` 运行客户端时，仍从
`ms0:/PSP/GAME/WiliMini/me_bridge.prx` 加载 Media Engine bridge。

## 项目结构

```text
client/       Bilibili 客户端、UI、网络服务和播放控制
decoder/      FFmpeg 播放器、音频、VFPU 和 Media Engine 路径
scripts/      资源生成和构建清单的 Python 脚本
tools/        PPSSPP 回放、PSP 辅助工具和主机测试
third_party/  固定版本的第三方依赖
docs/         设计文档、故障分析和研究记录
experiments/  独立实验及其复现资料
results/      PPSSPP、真机和性能实验结果
```

## 已知限制

- 当前主要针对 360p H.264/AAC 内容进行验证。
- 直播播放目前仍有真实样本失败。
- PSP-1000 与 PSP Slim 的兼容性需要分别验证。

## 相关文档

- [当前开发进度](docs/CURRENT-PROGRESS.md)
- [总体实施计划](PLAN.md)
- [播放器说明](decoder/README.md)
- [PSP select 网络问题分析](docs/PSP-SELECT-NETWORK-BUG.md)
- [UI 设计计划](docs/UI-DESIGN-PLAN.md)
- [UI 待办事项](docs/UI-TODO.md)
- [PSP 解码优化记录](docs/psp-decode-optimization-journey.md)
- [Media Engine 实验](experiments/me/)
- [实验结果](results/)

## 许可证

WiliMini 采用 GNU General Public License v3 协议开源

## 致谢

感谢以下项目及其贡献者。

| 项目 | 使用内容 | 许可证 |
| --- | --- | --- |
| [FFmpeg](https://ffmpeg.org/) | 音视频解码与容器解析 | LGPL-2.1-or-later |
| [curl](https://curl.se/) | PSP 网络请求 | curl license（MIT-like） |
| [PSP Media Engine MIST](https://github.com/mcidclan/psp-media-engine-mist) | Media Engine 集成 | MIT |
| [PSP Media Engine Custom Core](https://github.com/mcidclan/psp-media-engine-custom-core) | Media Engine core mapper 与辅助库 | MIT |
| [Wiliwili](https://github.com/xfangfang/wiliwili) | Bilibili API/result 代码及界面资源 | GPL-3.0-only |
| [WenQuanYi Micro Hei](https://sourceforge.net/projects/wqy/) | 客户端使用的字体 | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析 | MIT |
| [QR Code generator by Project Nayuki](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c) | 登录二维码生成 | MIT |
| [PSPSDK](https://github.com/pspdev/pspsdk) | PSP SDK 兼容代码及许可证声明 | BSD-3-Clause |
