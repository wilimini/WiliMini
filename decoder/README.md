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

客户端配置使用 VFPU_DSP=4、整像素亮度与最近邻色度取样、关闭去块滤波、解码全部参考与非参考帧、
ME 转色/DMA 和音频时钟 RGB 队列。这是速度优先配置，画质不等同标准 H.264。
8-bit YUV420P 双向预测分区只计算 list 0。亮度与色度残差均执行完整反变换和
叠加，恢复帧间宏块的残差修正；近似运动预测仍可能造成误差传播和边缘失真。
三项运动预测近似直接用于默认路径，没有新增配置开关。需重建 FFmpeg；客户端
链接要求 v2 补丁标识符，旧的省略残差库会链接失败，避免误用。
恢复残差后的测试见[残差恢复结果](../benchmarks/bilibili/residual-restored-results.md)。
此前四项近似的历史结果见[四项近似重建](../benchmarks/bilibili/approximation-results.md)。
默认 `PSP_SKIP_FRAME=-16`（`AVDISCARD_NONE`），不再主动跳过非参考帧。
PPSSPP 完整帧测试见[测试与部署结果](../benchmarks/bilibili/allframes-results.md)。
当视频落后音频时，客户端丢弃压缩包，直到后续关键帧再清理并重建参考状态；
不会沿用实验模式的无条件 B 帧追赶。复杂画面仍可能跳跃或停顿。

视频：8-bit YUV420P H.264，最多 1280×720，目标 480×270。
640×360 复用原专用缩放内核；其他尺寸等比缩放并留黑边。
音频：AAC / HE-AAC，双声道，44.1 或 48 kHz；不提供重采样。
其他编码、无音频、单声道等组合在本 demo 仍会报错。PSP-1000 尚未验证。
ME 桥按固件标识选表，播放器同步选择自身的 FAT/T2 函数表。
刷新 ME 时保留固件已有的启动跳转与 RAM 配置，不要求重启邮箱等于猜测地址。

FFmpeg 固定版本、补丁和 LGPL 条款见主项目文档；ME 上游来源及 MIT 许可见
`me/LICENSE.MIST.txt` 和 `docs/references.lock.json`。已有实验日志保留，记录中的
旧 app 源路径是历史路径。

## PPSSPP

默认与真机共用 FFmpeg 解码 + ME 转色路径，加载 ME bridge。需要支持真正 ME
执行的 PPSSPP、启用 `EmulateMediaEngine`，并提供对应的 Sony ME 固件镜像。
旧模拟器可显式构建 `-DBILI_PPSSPP_SCE_MPEG=ON` 使用保留的 sceMpeg 后端。
详见 [模拟器后端与验证](../docs/ppsspp-player.md) 和
[ME 兼容性分析](../docs/me-compatibility.md)。
