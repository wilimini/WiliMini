# psp-video

PSP 上的 H.264 / AAC 播放依赖。客户端只包含
`include/psp_video.h`，链接 `libpspvideo.a`，不接触 FFmpeg 类型、音频环形缓冲或 ME 作业。

- `src/`：FFmpeg 调用、ME 帧流水线、VFPU DSP、音频输出、音频时钟与帧队列。
- `me/`：ME 内核桥、用户态导入与 MIST 入口跳板。内核桥先于播放器 PRX 加载。
客户端构建：`make package`。CMake 在 `build/client/` 生成静态库、
客户端 PRX、ME 桥和加载器，
同时生成保留重链接所需的对象、ELF、map 和配置。依赖已有的 PSPDEV、
`build/ffmpeg-audio-install`、FFmpeg 的配置目录和 `build/me` 的 core mapper 对象。
从干净环境准备源码时，在仓库根目录运行
`git submodule update --init --recursive`（首次克隆也可使用
`git clone --recurse-submodules`），初始化两个 ME 依赖。
再运行 `make dependencies` 和 `make package`。FFmpeg 由 CMake 下载固定版本的
源码包到 `build/ffmpeg-source`，校验 SHA-256 并自动应用 PSP 补丁，无需手动打补丁。
默认依赖只构建客户端使用的 H.264 和浮点 AAC 解码器。使用预构建 FFmpeg 的环境
需要重建依赖才能获得这项内存节省。

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
视频呈现和控制器线程结束，释放 FFmpeg 对象并恢复 ME，之后可再次调用。
播放器不退出游戏、不连接网络、不管理 B 站或下载缓存。

宿主也可通过 `PspVideoOptions.cancel_requested` 提供在调用期间有效的
取消标志（使用原子写入）。播放器控制线程读取该标志并取消媒体源；
电源回调不需要直接操作源或等待工作线程。开始播放前已置位的标志会
直接返回已取消。`PspVideoResult.cleanup_error` 单独报告 ME 清理失败，
取消不会覆盖此错误；宿主在该错误下不可释放保护 ME 的休眠锁。
正常客户端持有独立应用电源锁直到播放器、媒体源、叠加层和 UI 恢复
全部结束；唤醒后由用户重新打开视频，不保留运行中的解码器上下文。


`PspVideoOptions.diagnostic_profile` 非零时，在前 30 个输出帧预热后开启
现有 FFmpeg H.264 阶段采样，额外保存 `profile.csv` 和 `packets.csv`。
默认关闭；采样有额外开销。独立数据集及批量诊断入口见
[`benchmarks/bilibili/`](../benchmarks/bilibili/README.md)。

播放器初始化只安装解码内核，不运行亮度、色度、音频或 CABAC 自检。

## 视频流水线

原生播放分三处执行：

1. SC（主 CPU）解封装、解码音频，并只做 H.264 熵解码（CABAC/CAVLC）。
   `me_frame_pipeline.c` 通过 FFmpeg 钩子（`patches/ffmpeg/0004-psp-me-frame-pipeline.patch`）
   把每个宏块的预测模式、运动补偿分区和稀疏残差记录到整帧命令槽，不触碰像素。
2. ME 上的常驻工作函数（`mefp_recon.c`）按 FIFO 命令环重建整帧：帧内预测、
   精确 6 抽头亮度 qpel、精确 packed 色度、边缘模拟、加权预测和整数 IDCT。
   ME 端不执行 VFPU 指令。重建写回 YUV 平面后推进完成序号。
3. 帧队列（`frame_pacer.h`）按音频时钟把 YUV420P 帧交给 presenter；
   客户端 `GeYuvRenderer` 用 GE 五遍 CLUT 绘制直接从主内存读取三个平面、
   双线性缩放并转成 RGBA8888，写入第三个 VRAM 页面，再叠加 OSD、弹幕和字幕。
   CPU 不做逐像素转色，也不分配 RGB 帧缓冲。

呈现线程只读取帧描述和 ME 完成序号，不调用 FFmpeg；AVFrame 引用只在解码线程增减。
流水线只接收 8-bit 4:2:0 逐行帧、无去块滤波（默认 `BILI_SKIP_LOOP_FILTER=48`）、
宽高不超过 1680 个宏块、行距不超过 1024 的片；其余帧（隔行、MBAFF、开启去块等）
先排空 ME，再由 FFmpeg 在 SC 上重建，同样经 GE 呈现。

两种 ME 后端运行同一工作函数：Custom Core 直接调用，并把边缘模拟和双向预测
暂存区放在 ME eDRAM；MIST 经 `me/me_entry.S` 的跳板进入，暂存区改在主内存
分配，不改动固件的 eDRAM 布局。MIST 路径在整个播放期间占用固件的 ME 处理函数；
播放器音频为软件解码，不依赖固件 ME 服务。MIST 上的流水线尚未在真机验证。

`BILI_DEVICE_PLAYBACK_CPU_RENDER=ON`（仅诊断构建）关闭 ME 流水线，所有帧由 SC 重建，
仍经帧队列和 GE 呈现，用于没有 ME 固件的环境。

`PspVideoOptions.buffer_frames` 支持 2、4、8，零值保留 API 默认的 8 帧。
每个排队帧持有一份解码图像引用（640×360 约 350 KiB），而非额外的 RGB 输出缓冲。
客户端在 PSP-1000 上固定使用默认的 2 帧，播放缓冲设置不可修改；扩展内存机型可选 2/4/8。
在线内存不足复现和缓解结果见 [内存调查](../docs/playback-memory-investigation.md)。
原生后端要求提供 `presenter`。

2026-11-24 在 PPSSPPHeadless（固定提交 `44f1f44`，软件渲染）运行 SC 重建诊断构建
（`BILI_DEVICE_PLAYBACK_CPU_RENDER=ON`，未执行 ME 流水线，模拟器无 ME 固件），
640×360/24fps High Profile（B 帧）8 秒 H.264/AAC 片源，默认跳帧设置。
PSP-1000 与 PSP-3000 机型各连续播放 2/4/8 帧队列：均 code=0、EOF，呈现 100 帧、
丢弃 0、追赶包 0，结束后堆回落到播放前水平。预热后堆占用分别为 6.34、7.08、8.57 MB，
每增加一个排队帧约 363 KiB（旧 RGB 队列每帧 RGB565 272 KiB、RGBA8888 544 KiB）。
截图确认 GE 转色、缩放正确。模拟器结果不代表真机内存或性能，ME 流水线本身
（约 0.47 MiB 命令槽，MIST 另加 60 KiB 暂存区）未在此测量。

音频与视频共用同一封装时，音频沿用视频上下文的探测结果（流参数与 `start_time`），
避免再次探测时打开无用的 H.264 解码器；独立的 DASH 音频输入始终调用
`avformat_find_stream_info()`，以确定起始时间并识别隐式 HE-AAC SBR/PS。
`PspVideoOptions.backend` 由客户端根据平台环境选择；播放器本身不检测 PPSSPP。
默认使用 `PSP_VIDEO_BACKEND_NATIVE_ME`，只有启用
`BILI_PPSSPP_SCE_MPEG` 且运行在 PPSSPP 时才选择 `PSP_VIDEO_BACKEND_PPSSPP_SCE_MPEG`。
帧队列槽在播放期间复用，等待呈现线程和 ME 停止后才释放。
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

默认配置关闭去块滤波并跳过非参考帧，画质不等同标准 H.264；ME 流水线内的运动补偿
（含双向预测）与 FFmpeg 位精确。只有诊断用的 SC 重建构建仍对双向预测分区只计算
list 0（`0003-psp-approximate-reconstruction.patch`），流水线运行时其钩子关闭该近似。
此前近似重建的结果见[残差恢复结果](../benchmarks/bilibili/residual-restored-results.md)
和[四项近似重建](../benchmarks/bilibili/approximation-results.md)。
默认 `PSP_SKIP_FRAME=8`（`AVDISCARD_NONREF`），主动跳过非参考帧以减少真机解码负担；
`-DBILI_SKIP_FRAME=0` 解码全部帧。
此前关闭主动跳帧的 PPSSPP 测试见[测试与部署结果](../benchmarks/bilibili/allframes-results.md)，
其结果不代表当前跳帧配置的真机播放表现。
当视频落后音频时，客户端丢弃压缩包，直到后续关键帧再清理并重建参考状态；
不会沿用实验模式的无条件 B 帧追赶。复杂画面仍可能跳跃或停顿。

视频：8-bit YUV420P H.264，最多 1280×720，由 GE 等比缩放到 480×270 以内并留黑边。
音频：AAC / HE-AAC，单声道或双声道，44.1 或 48 kHz；不提供重采样。
单声道输入复制到 PSP 的左右输出声道。其他编码、无音频等组合在本 demo 仍会报错。
PSP-1000 尚未验证。
ME 桥按固件标识选表，播放器同步选择自身的 FAT/T2 函数表。
现有 MIST 刷新保留固件 RAM 配置，并将启动参数地址改写为 `0x88400000`；
该流程与下述 Custom Core 启动方式不同。

## Custom Core 执行后端（实验性）

原生播放器默认使用 Custom Core。每次 `make package` 都生成两个桥 PRX：
`me_bridge.prx`（MIST）和 `me_bridge_custom_core.prx`（Custom Core），
二者导出相同的 `me_lab` 接口，另有 `meLabBackend()` 返回后端编号。

选择方式：设置 → “ME 后端”（保存在 `settings.json` 的 `me_backend`，
取值 `mist` 或 `custom-core`，缺省或无法识别时为 `custom-core`）。启动器（`bootstrap.c`）在加载
客户端前读取该值，只加载对应的桥；客户端在每次播放初始化时用
`meLabBackend()` 选择 MIST 返回跳板（`me_mist_entry`）或 Custom Core 直接调用的
C 工作函数（`mefp_me_worker`）。因此修改后必须退出（START）并重新打开 WiliMini 才生效，运行中
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
恢复保存的启动 SRAM 并释放复位，随后播放器才释放帧队列。任务或停止超时
会拒绝后续播放并保留仍可能被访问的内存，需完全关机重启。
此后端尚未实现休眠上下文恢复，持有 ME 期间锁定电源开关，恢复固件后解锁；
播放中拨动开关的休眠请求可能在解锁后执行。

主机替身测试只能验证生命周期与超时保护，不能证明启动、缓存或恢复固件的真机
正确性。PSP-3000 验证流程和记录见 [Custom Core 验证](../docs/custom-core-backend.md)。

## 播放故障日志

原生 ME 播放每次在 `results/run-XXX/diagnostics.txt` 保存诊断节点。
设置页“日志与结果写入文件”默认关闭；关闭时不创建 `results/`，也不写 `diagnostics.txt`。开启后，两种后端的每次播放还会在同一目录持续写入 `log.txt`（本次播放期间的日志）。`results/` 默认只保留最近 20 次运行，
诊断分析（profile）运行不会被清理，详见 [调试文档](../docs/debug.md#playback-result-folders)。
原生播放器只在写入结果时分配帧和呈现记录缓冲，逐包记录只在 profile 模式分配。
记录缓冲分配失败时，相应 CSV 可能只有表头。播放器会记录警告并继续尝试播放。
文件记录软件 Git 版本（含 dirty 标记）、构建配置时间（UTC）、构建类型、
ME 桥自身的构建标识、设备本地时间、机型 ID、基础固件 devkit 版本、
进程内尝试次数、系统运行时间和可用内存。机型 ID 使用 SDK 原始值
（0 为 PSP-1000，1 为 PSP-2000）；devkit 值不能识别 PRO-C、ARK 等完整
自制系统名称，反馈时仍需手动注明。构建时间是 CMake 配置时间。

初始化前后、ME 工作函数启动、错误和清理阶段会写文件并关闭文件。
ME 桥使用最多 24 条记录的固定内存环；流水线只提交一次常驻任务，逐帧命令
不经过桥，因此不写记忆棒。桥记录在初始化和收尾时导出。
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
`ready` 的非负结果是固件表 ID。常驻工作函数结束时 `value` 为 `4d450001`。
应结合 done 和完成邮箱判断，不能仅凭一个标记认定 ME 已安全停止。

`summary.json` 同时记录构建与设备字段；`audio_stats_valid=false`
表示本次尚未进入音频初始化，此时省略音频及呈现统计，不沿用前次值。
`frames_queued`、`presented`、`dropped` 记录帧队列；`mefp_*` 记录流水线
（ME 重建帧数、退回 SC 的帧数、SC 等待 ME 的时间等），`ge_wait_us` 和
`ge_present_us` 记录呈现线程等待 ME 和 GE 绘制的时间。
日志不保存 Cookie、签名 URL 或媒体内容。测试反馈请提供整个 `results/`、
`last-error.txt`、`last-playback-error.txt` 和安装包的 `build.json`，并注明
完整系统版本、插件启用情况，以及各次尝试之间是否完全关机重启。
硬死机或断电可能丢失尚未落盘的信息；日志无法保证捕获所有硬件故障。

FFmpeg 固定版本、补丁和 LGPL 条款见主项目文档；ME 上游来源及 MIT 许可见
`me/LICENSE.MIST.txt` 和 `docs/references.lock.json`。已有实验日志保留，记录中的
旧 app 源路径是历史路径。

## PPSSPP

默认与真机共用 FFmpeg 熵解码 + ME 重建 + GE 呈现路径，加载 ME bridge。需要支持真正 ME
执行的 PPSSPP、启用 `EmulateMediaEngine`，并提供对应的 Sony ME 固件镜像。
旧模拟器可显式构建 `-DBILI_PPSSPP_SCE_MPEG=ON` 使用保留的 sceMpeg 后端。
详见 [模拟器后端与验证](../docs/ppsspp-player.md) 和
[ME 兼容性分析](../docs/me-compatibility.md)。

## 播放控制与显示回调

`PspVideoOptions.presenter` 接收 `PspVideoYuvFrame`（YUV420P）和控制状态；空帧表示重绘上一帧。
PPSSPP sceMpeg 诊断后端改为传入 RGBA8888 图像。宿主负责 GE 同步及换页，
返回前必须读完平面。
原生 ME 跳转保留视频/ME 实例；音频管线会重新打开。源的
`interrupt_reads` 回调用于中断旧读取，不得销毁或取消下载源。
快捷键、菜单、缓冲所有权和限制见 [播放控制与弹幕](../docs/playback-controls.md)。
