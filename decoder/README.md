# psp-video

PSP 上的 H.264 / AAC 播放依赖。客户端只包含
`include/psp_video.h`，链接 `libpspvideo.a`，不接触 FFmpeg 类型、音频环形缓冲或 ME 作业。

- `src/`：FFmpeg 调用、VFPU DSP、音频输出、音频时钟与 RGB 队列。
- `me/`：ME 内核桥、用户态导入、转色与缩放任务。内核桥先于播放器 PRX 加载。
客户端构建：`make package`。CMake 在 `build/client/` 生成静态库、
客户端 PRX、ME 桥和加载器，
同时生成保留重链接所需的对象、ELF、map 和配置。依赖已有的 PSPDEV、
`build/ffmpeg-audio-install`、FFmpeg 的配置目录和 `build/me` 的 core mapper 对象。
从干净环境准备源码时，在仓库根目录运行
`git submodule update --init --recursive`（首次克隆也可使用
`git clone --recurse-submodules`），初始化两个 ME 依赖。
再运行 `make dependencies` 和 `make package`。FFmpeg 由 CMake 下载固定版本的
源码包到 `build/ffmpeg-source`，校验 SHA-256 并自动应用 PSP 补丁，无需手动打补丁。

```c
#include "psp_video.h"
PspVideoOptions options = {
    .path = "ms0:/PSP/GAME/WiliMini/cache/video.mp4",
    .log_root = "ms0:/PSP/GAME/WiliMini",
    .backend = PSP_VIDEO_BACKEND_NATIVE_ME
};
PspVideoResult result;
int error = psp_video_play(&options, &result);
```

在带 `PSP_THREAD_ATTR_VFPU` 的线程上同步调用。路径必须是可 seek 的本地 MP4；
也可以设置 `PspVideoOptions.source`，由宿主提供随机读取、取消和大小回调，
播放器会等待尚未到达的区间。路径或 source 二者必须提供其一。
根目录必须存在。真机播放器独占 ME、音频 SRC 和视频帧缓冲。同一时刻仅允许一个实例。
`PspVideoOptions.cancel_button` 控制取消键，零值默认为圆圈；客户端会传入设置中的返回键。
HOME 退出由宿主注册回调并调用 `psp_video_stop()`。返回前会等待音频、
RGB 显示和控制器线程结束，释放 FFmpeg 对象并恢复 ME，之后可再次调用。
播放器不退出游戏、不连接网络、不管理 B 站或下载缓存。

`PspVideoOptions.diagnostic_profile` 非零时，在前 30 个输出帧预热后开启
现有 FFmpeg H.264 阶段采样，额外保存 `profile.csv` 和 `packets.csv`。
默认关闭；采样有额外开销。独立数据集及批量诊断入口见
[`benchmarks/bilibili/`](../benchmarks/bilibili/README.md)。

播放器初始化只安装解码内核，不运行亮度、色度、音频或 CABAC 自检。

`PspVideoOptions.rgb_buffer_frames` 支持 2、4、8，零值保留 API 默认的 8 帧。
客户端传入用户设置，新设置默认值由启动器选中的客户端决定。
`PspVideoOptions.backend` 由客户端根据平台环境选择；播放器本身不检测 PPSSPP。
默认使用 `PSP_VIDEO_BACKEND_NATIVE_ME`，只有启用
`BILI_PPSSPP_SCE_MPEG` 且运行在 PPSSPP 时才选择 `PSP_VIDEO_BACKEND_PPSSPP_SCE_MPEG`。
RGB 队列使用一次对齐分配，在播放期间复用，等待显示线程和 ME 停止后才释放。
音频先于视频 EOF 时，所有 PCM 提交后时钟继续推进，视频可播放至自身结尾。
若 ME 停止超时，保留其仍可能访问的缓冲，并拒绝再次启动，避免释放后写入。

H.264 色度默认使用精确 packed 整数插值和对齐 copy/average 内核；
`-DBILI_PACKED_CHROMA=OFF` 可构建标量对照。真机两轮总体呈现帧增加约 10%，
但存在单样本解码耗时退化，详见
[真机对比](../benchmarks/bilibili/hardware-results.md)。

重建 FFmpeg 时默认开启精确 Allegrex CABAC 条件移动与状态别名优化，
`-DBILI_FFMPEG_CABAC_MOVN=OFF` 可重建标量对照。已有外部 FFmpeg 安装不会
仅因客户端重新链接而改变，需重新构建依赖。第二轮真机两轮合计呈现增加
2.51%，追赶包减少 6.83%，也存在单样本退化；其他 cache、copy/average、
队列候选仍默认关闭，见[第二轮实测](../benchmarks/bilibili/optimization-round2.md)。

客户端配置使用 VFPU_DSP=4、整像素亮度与最近邻色度取样、关闭去块滤波、跳过非参考帧、
ME 转色/DMA 和音频时钟 RGB 队列。这是速度优先配置，画质不等同标准 H.264。
8-bit YUV420P 双向预测分区只计算 list 0。亮度与色度残差均执行完整反变换和
叠加，恢复帧间宏块的残差修正；近似运动预测仍可能造成误差传播和边缘失真。
三项运动预测近似直接用于默认路径，没有新增配置开关。需重建 FFmpeg；客户端
链接要求 v2 补丁标识符，旧的省略残差库会链接失败，避免误用。
恢复残差后的测试见[残差恢复结果](../benchmarks/bilibili/residual-restored-results.md)。
此前四项近似的历史结果见[四项近似重建](../benchmarks/bilibili/approximation-results.md)。
默认 `PSP_SKIP_FRAME=8`（`AVDISCARD_NONREF`），主动跳过非参考帧以减少真机解码负担。
此前关闭主动跳帧的 PPSSPP 测试见[测试与部署结果](../benchmarks/bilibili/allframes-results.md)，
其结果不代表当前跳帧配置的真机播放表现。
当视频落后音频时，客户端丢弃压缩包，直到后续关键帧再清理并重建参考状态；
不会沿用实验模式的无条件 B 帧追赶。复杂画面仍可能跳跃或停顿。

视频：8-bit YUV420P H.264，最多 1280×720，目标 480×270。
640×360 复用原专用缩放内核；其他尺寸等比缩放并留黑边。
音频：AAC / HE-AAC，双声道，44.1 或 48 kHz；不提供重采样。
其他编码、无音频、单声道等组合在本 demo 仍会报错。PSP-1000 尚未验证。
ME 桥按固件标识选表，播放器同步选择自身的 FAT/T2 函数表。
现有 MIST 刷新保留固件 RAM 配置，并将启动参数地址改写为 `0x88400000`；
该流程与下述 Custom Core 启动方式不同。

## Custom Core 执行后端（实验性）

原生播放器默认使用 MIST。每次 `make package` 都生成两个桥 PRX：
`me_bridge.prx`（MIST）和 `me_bridge_custom_core.prx`（Custom Core），
二者导出相同的 `me_lab` 接口，另有 `meLabBackend()` 返回后端编号。

选择方式：设置 → “ME 后端”（保存在 `settings.json` 的 `me_backend`，
取值 `mist` 或 `custom-core`，缺省为 `mist`）。启动器（`bootstrap.c`）在加载
客户端前读取该值，只加载对应的桥；客户端在每次播放初始化时用
`meLabBackend()` 选择 MIST 返回跳板（`me_task`）或 Custom Core 的 C 工作函数
（`me_worker`）。因此修改后必须退出（START）并重新打开 WiliMini 才生效，运行中
不会切换已加载的桥；设置页在已保存值与已加载值不同时显示“需重启”。
这样每次启动 ME 都处于干净状态，不在同一进程内交叉使用两种复位流程。

若 Custom Core 桥加载或启动失败，启动器会输出日志并回退到 MIST；这不涵盖
首次播放时的 ME 启动失败（如固件表不匹配），后者按原有播放错误处理。
完整安装包须同时包含两个桥（启动器的完整性检查要求），并来自同一次打包。
`build.json` 的 `me_backends` 列出包内后端，播放诊断及 summary 的 `me_backend`
记录运行时实际加载的后端。

Custom Core 使用固定版本上游的 `me-core.h` 启动代码和 `me-lib.c` 缓存函数，
由现有内核桥安装，无需额外安装 `kcall.prx`。任务通过一个 64 字节对齐的非缓存
邮箱提交，不注入固件 syscall 表。启动先等待 ME 就绪，再执行不访问视频缓冲的
空任务，确认工作函数可以完整返回后才继续播放。

退出先等待任务返回，再请求常驻循环停止；收到停止确认后，主 CPU 复位 ME，
恢复保存的启动 SRAM 并释放复位，随后播放器才释放 RGB 缓冲。任务或停止超时
会拒绝后续播放并保留仍可能被访问的内存，需完全关机重启。
此后端尚未实现休眠上下文恢复，持有 ME 期间锁定电源开关，恢复固件后解锁；
播放中拨动开关的休眠请求可能在解锁后执行。

主机替身测试只能验证生命周期与超时保护，不能证明启动、缓存或恢复固件的真机
正确性。PSP-3000 验证流程和记录见 [Custom Core 验证](../docs/custom-core-backend.md)。

## 播放故障日志

原生 ME 播放每次在 `results/run-XXX/diagnostics.txt` 保存诊断节点。
文件记录软件 Git 版本（含 dirty 标记）、构建配置时间（UTC）、构建类型、
ME 桥自身的构建标识、设备本地时间、机型 ID、基础固件 devkit 版本、
进程内尝试次数、系统运行时间和可用内存。机型 ID 使用 SDK 原始值
（0 为 PSP-1000，1 为 PSP-2000）；devkit 值不能识别 PRO-C、ARK 等完整
自制系统名称，反馈时仍需手动注明。构建时间是 CMake 配置时间。

初始化前后、首个 ME 任务提交/完成、错误和清理阶段会写文件并关闭文件。
ME 桥使用最多 24 条记录的固定内存环；正常播放只记录首个任务，避免逐帧
写记忆棒。实验性 ME 队列的工作线程不写文件，桥记录在初始化和收尾时导出。
节点中的 `bridge_seq` 是桥加载以来的递增序号；`time_us` 是 32 位系统
微秒计数，会回绕。跨次重试会导出环中尚存的前次上下文。

桥事件的 `a`、`b`、`c` 为十六进制值：

| 事件 | a | b | c |
| --- | --- | --- | --- |
| init | 已初始化 | 仍有待完成任务 | 任务入口地址 |
| profile | 固件表 ID | ME 表地址 | 0 |
| bootstrap | BFC00060 RAM 配置 | BFC00064 | BFC00068 |
| backend | 0 | 0 | 0 |
| dma_begin | 读取的 ME 地址 | 目标物理地址 | BC8001B0 DMA 状态 |
| dma_end | 清理前 DMA 状态 | 等待微秒 | 成功时保存的原入口 |
| inject_begin | 注入索引 | 新入口 | 原入口 |
| submit | 操作 | 宽 | 高 |
| trigger | 任务地址 | done | value |
| wait_timeout / wait_done | BFC00700 完成邮箱 | done | value |
| shutdown | 已初始化 | 仍有待完成任务 | 0 |
| restore_begin | 注入索引 | 原入口 | 0 |

`submit` 被拒绝（result=-1）时 a/b 为 ready/pending；`dma_begin`
分配失败（result=-7）时 b 为分配错误。`result` 保留原始返回码，
`ready` 的非负结果是固件表 ID。任务超时的 `value` 对应 `me_task.h`
中的进度标记：1 进入工作函数，2–4 输入平面处理，5 转色/缩放前，
6 转色/缩放后，`4d450001` 工作函数结束。应结合 done 和完成邮箱判断，
不能仅凭一个标记认定 ME 已安全停止。

`summary.json` 同时记录构建与设备字段；`audio_stats_valid=false`
表示本次尚未进入音频初始化，此时省略音频及 RGB 统计，不沿用前次值。
日志不保存 Cookie、签名 URL 或媒体内容。测试反馈请提供整个 `results/`、
`last-error.txt`、`last-playback-error.txt` 和安装包的 `build.json`，并注明
完整系统版本、插件启用情况，以及各次尝试之间是否完全关机重启。
硬死机或断电可能丢失尚未落盘的信息；日志无法保证捕获所有硬件故障。

FFmpeg 固定版本、补丁和 LGPL 条款见主项目文档；ME 上游来源及 MIT 许可见
`me/LICENSE.MIST.txt` 和 `docs/references.lock.json`。已有实验日志保留，记录中的
旧 app 源路径是历史路径。

## PPSSPP

默认与真机共用 FFmpeg 解码 + ME 转色路径，加载 ME bridge。需要支持真正 ME
执行的 PPSSPP、启用 `EmulateMediaEngine`，并提供对应的 Sony ME 固件镜像。
旧模拟器可显式构建 `-DBILI_PPSSPP_SCE_MPEG=ON` 使用保留的 sceMpeg 后端。
详见 [模拟器后端与验证](../docs/ppsspp-player.md) 和
[ME 兼容性分析](../docs/me-compatibility.md)。

## 播放控制与显示回调

`PspVideoOptions.presenter` 接收视频帧和控制状态；空帧表示重绘上一帧。
宿主负责 DMA/GE 同步及换页。未提供回调时沿用默认视频显示。
原生 ME 跳转保留视频/ME 实例；音频管线会重新打开。源的
`interrupt_reads` 回调用于中断旧读取，不得销毁或取消下载源。
快捷键、菜单、缓冲所有权和限制见 [播放控制与弹幕](../docs/playback-controls.md)。
