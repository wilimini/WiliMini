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
`git clone --recurse-submodules`）。然后将
`patches/ffmpeg/0001-psp-sampled-h264-profile.patch` 应用到
`third_party/ffmpeg`，再运行 `make ffmpeg` 和 `make me`。

```c
#include "psp_video.h"
PspVideoOptions options = {
    "ms0:/PSP/GAME/WiliMini/cache/video.mp4",
    "ms0:/PSP/GAME/WiliMini", 0
};
PspVideoResult result;
int error = psp_video_play(&options, &result);
```

在带 `PSP_THREAD_ATTR_VFPU` 的线程上同步调用。路径必须是可 seek 的本地 MP4；
也可以设置 `PspVideoOptions.source`，由宿主提供随机读取、取消和大小回调，
播放器会等待尚未到达的区间。路径或 source 二者必须提供其一。
根目录必须存在。播放器独占 ME、音频 SRC 和视频帧缓冲。同一时刻仅允许一个实例。
圆圈取消；HOME 退出由宿主注册回调并调用 `psp_video_stop()`。返回前会等待音频、
RGB 显示和控制器线程结束，释放 FFmpeg 对象并恢复 ME，之后可再次调用。
播放器不退出游戏、不连接网络、不管理 B 站或下载缓存。

客户端配置保留 VFPU_DSP=4、近似双线性 qpel、关闭去块滤波、丢弃非参考帧、
ME 转色/DMA 和音频时钟 RGB 队列。这是速度优先配置，画质不等同标准 H.264。
当视频落后音频时，客户端丢弃压缩包，直到后续关键帧再清理并重建参考状态；
不会沿用实验模式的无条件 B 帧追赶。复杂画面仍可能跳跃或停顿。

视频：8-bit YUV420P H.264，最多 1280×720，目标 480×270。
640×360 复用原专用缩放内核；其他尺寸等比缩放并留黑边。
音频：AAC / HE-AAC，双声道，44.1 或 48 kHz；不提供重采样。
其他编码、无音频、单声道等组合在本 demo 仍会报错。PSP-1000 尚未验证。
ME 桥保留原固件表检测，拒绝不匹配的环境。

FFmpeg 固定版本、补丁和 LGPL 条款见主项目文档；ME 上游来源及 MIT 许可见
`me/LICENSE.MIST.txt` 和 `docs/references.lock.json`。已有实验日志保留，记录中的
旧 app 源路径是历史路径。
