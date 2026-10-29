# PPSSPP 播放后端

## 默认：原生 ME

默认构建不链接 sceMpeg；启动器在 PPSSPP 中也加载 `me_bridge.prx`，
播放器与真机共用 FFmpeg 视频/音频解码和 ME 转色/缩放路径。
日志为 `LOADER player=psp-me`、`PLAYER backend=psp-native`；summary 的
`backend` 为 `psp-native`。

PPSSPP master 的 `Core/MIPS/MeCore.cpp` 已提供真正的 ME 指令执行。
需要启用 `[CPU] EmulateMediaEngine = True`，并向 PPSSPP 的 flash0 提供
PSP-1000 的 `kd/resource/meimg.img`，或 Slim 的 `kd/resource/me_t2img.img`。
MIST 复用固件调度器，因此只有 ME CPU、没有这些镜像仍不能启动 MIST。

Headless 运行器默认启用 ME 模拟；用 `BILI_HEADLESS_FIRMWARE_DIR` 指向解出的
flash0 目录，运行器仅把所选机型的 ME 镜像复制到隔离 NAND。
它不会下载固件，也不会把固件加入安装包。构建和运行示例：

```sh
make package BUILD_DIR=build/playback-test \
  CMAKE_ARGS='-DBILI_DEVICE_PLAYBACK_TEST=ON -DBILI_DIST_DIR=build/playback-test-dist'
BILI_HEADLESS_FIRMWARE_DIR=/path/to/flash0 python3 tools/ppsspp/test-psp-playback.py
```

要求运行器支持 `--appendconfig` 等现有测试参数。旧 Headless 二进制不能据此
推断新 PPSSPP 的 ME 能力。原生测试检查 backend、输出帧、音频和退出结果；
原生 first_frame 回调只保证入队，因此不沿用 HLE 回调即时截图的像素断言。

2026-10-29 已在 PPSSPP commit `83e616eaf97bd493f91944e5eced70d914b4bc18`
实测原生 ME。Headless 配置为 Release、`HEADLESS_CROSS=ON`、`USE_FFMPEG=OFF`，
客户端为 Release、`BILI_DEVICE_PLAYBACK_TEST=ON`、`BILI_PPSSPP_SCE_MPEG=OFF`。
使用官方 6.61 updater 解出的加密 `meimg.img` / `me_t2img.img`，由模拟器加载、
解密并执行。固件只保存在本地 build 测试目录，不随客户端包发布。

| 用例 | PSP-1000 / 12 MiB / 2 帧缓冲 | Slim / 24 MiB / 8 帧缓冲 |
| --- | --- | --- |
| MP4 从 3 秒续播 | 75 帧，EOF | 75 帧，EOF |
| DASH 从 3 秒续播 | 75 帧，EOF | 75 帧，EOF |
| FLV 非 seek 输入 | 120 帧，EOF | 120 帧，EOF |
| High Profile / B 帧 | 52 帧，EOF | 52 帧，EOF |
| 连续播放三次 | 每次 120 帧，EOF | 每次 120 帧，EOF |
| 圆圈取消 | cancelled=1，位置 4493 ms | cancelled=1，位置 4493 ms |
| 8 秒视频 / 4 秒音频 | 120 帧，EOF | 120 帧，EOF |

两种配置各 40 项断言通过，包括真实 ME 作业提交数与完成数一致。
High Profile 素材编码 96 帧；原生生产配置跳过非参考帧，因此呈现 52 帧，
不能把它解释为完整画质解码 96 帧。两种续播的首段视频及音频均为 3,000,000 µs。
三次播放的后两轮堆占用在调用前后无增长。

检查了 14 张播放截图的有效像素，两张独立定时截图的画面有变化。
额外录制的双声道 WAV 有非零音频信号；未做主观听音判断。
另一次 PSP-1000 配置测试播放两轮后按 START，正常调用 `sceKernelExitGame`，
未超时，退出时内核对象日志中已无 `video_cancel`、`aac_decode`、
`pcm_output`、`pts_present` 播放线程。常规七用例停留在结果页后由 Headless
超时结束，不能与这次正常退出测试混为一谈。

这次测试还修正两个实际失败：测试入口硬编码 8 帧缓冲导致 FAT 的 FLV
用例内存不足，现沿用机型默认值；原生音频时钟在音频先 EOF 时停止推进，
现于所有 PCM 提交后继续推进，让较长视频正常结束。

日志、summary、截图和断言在 `results/native-me/`，运行器、固件、客户端清单
及素材的哈希在 `environment.json`。此前缺少 ME 固件的日志不是本次测试结果。
尚未进行 PSP-1000 真机验证，不能据模拟器结果推断硬件性能或固件兼容范围。
详细原因和验证边界见 [ME 兼容性分析](me-compatibility.md)。

## 可选：旧 sceMpeg 后端

使用 `-DBILI_PPSSPP_SCE_MPEG=ON` 构建；此选项下的行为如下。

启动器和播放器使用
`sceIoDevctl("kemulator:", 0x00000003, NULL, 0, NULL, 0) == 0`
识别 PPSSPP。启动日志中的 `LOADER player=ppsspp-sceMpeg` 和播放日志中的
`PLAYER backend=ppsspp-sceMpeg` 表示使用模拟器后端。

PPSSPP 下加载 AVCODEC、MPEGBASE 模块，跳过 `me_bridge.prx`；包内缺少
ME bridge 文件也可以启动。真机仍要求完整 ME 包并使用原播放器。
两条路径共用 `psp_video_play` / `psp_video_stop` API。

模拟器后端位于 `decoder/src/ppsspp_player.h`，作为 `player.c` 的内部实现，
复用已有随机访问输入、AAC 输出、控制器取消和音频时钟：

1. FFmpeg 解析 MP4、DASH 或 FLV，保留原 H.264 压缩数据。
2. 把 avcC 长度前缀转换为 Annex B，在关键帧前补 SPS/PPS。
3. 将视频封装为 2048 字节 MPEG-PS 包，经 sceMpeg ringbuffer 送入解码器。
   PSMF 头只用于初始化流信息，输入按需读取，不保存完整转封装文件。
4. `sceMpegAvcDecode` 输出 RGBA，客户端等比缩放显示，并按音频时钟同步。
5. EOF 或取消后关闭音频线程，销毁 sceMpeg 实例和 ringbuffer，释放输入及缓冲。

视频解码使用 sceMpeg；容器解析与 AAC/HE-AAC 解码仍使用 FFmpeg。
这是针对 PPSSPP HLE 的适配，不是在真机上启用固件硬解。
PPSSPP 的 `sceMpegGetAvcNalAu` 尚未实现，因此不能直接投递 MP4 NAL 包。
模拟器本身必须启用 FFmpeg；没有视频解码支持的 Headless 构建不能用于播放验证。

支持范围沿用 H.264、最大 1280×720、双声道 44.1/48 kHz AAC 的输入限制。
实际验证覆盖 640×360 Baseline 和 High Profile/B 帧；不据此推断其他分辨率或真机性能。
`rgb_buffer_frames` 保留接口兼容性，此后端直接双缓冲显示，不分配 ME 的 RGB 队列。
每次播放仍在 `results/run-NNN/summary.json` 中记录后端、帧数、首帧/音频时间戳和错误。

## 构建与验证

正常客户端通过 `make package` 构建，不需要专门的模拟器客户端 PRX。

受控测试构建：

```sh
make package BUILD_DIR=build/playback-test \
  CMAKE_ARGS='-DBILI_DEVICE_PLAYBACK_TEST=ON -DBILI_PPSSPP_SCE_MPEG=ON -DBILI_DIST_DIR=build/playback-test-dist'
BILI_HEADLESS_PLAYER=ppsspp-sceMpeg python3 tools/ppsspp/test-psp-playback.py
```

测试依赖主机 ffmpeg/libx264、启用 FFmpeg 的 PPSSPPHeadless，以及
`tools/README.md` 列出的 Headless 参数。可以用 `PPSSPP_HEADLESS` 指定程序，
用 `BILI_HEADLESS_CLIENT_DIR`、`BILI_HEADLESS_PACKAGE` 和 `BILI_PLAYBACK_RESULTS`
覆盖测试目录。每个测试使用隔离 memstick，不访问账户或网络 API。

测试覆盖 MP4/DASH 3 秒续播、FLV 非 seek 输入、圆圈取消、High Profile/B 帧及
三次连续播放；同时断言日志选中了 sceMpeg 后端。
High Profile 测试通过 source 的 first_frame 回调保存
`playback-first-frame.ppm`，并检查像素数量和颜色变化，避免只凭成功返回值判断解码成功。
受控测试还支持 `cancel_after_ms`，用于不依赖按键注入的停止/清理检查。

2026-10-28 本机验证使用现有 PPSSPPHeadless（FFmpeg 已启用）、PSP-1000 内存配置、
Release/`BILI_DEVICE_PLAYBACK_TEST=ON`，结果保存在 `results/ppsspp-player/`：

- 8 秒 15 fps Baseline MP4：120 帧，EOF，384,000 个有效音频采样。
- MP4/DASH 从 3 秒续播：首帧、首段音频时间戳均为 3,000,000 µs。
- FLV 非 seek 输入：120 帧，EOF。
- 4 秒 24 fps High Profile，3 个 B 帧：96 帧，EOF；已检查首帧图像。
- 三次连续播放：各 120 帧、三个独立 summary，后两次堆用量无增量。
- 播放期间定时调用 stop：返回 cancelled=1、eof=0，音频错误为 0。
- 删除 me_bridge.prx 后仍能启动、播放 120 帧并到达 EOF。
- 扩展内存/Slim 配置：High Profile 素材 96 帧，EOF。
- 8 秒视频配 4 秒音频：视频尾段继续播放，120 帧，最终位置 8 秒。

结果断言保存在 `results/ppsspp-player/verification.json`，43 项通过；
运行器哈希、配置和素材信息见同目录的 `environment.json`。

现有本机 Headless 使用旧参数接口，通过隔离 host0/memstick 运行以上测试；
客户端完成测试后停留在结果页，Headless 到超时时保存日志。
这些超时不表示客户端崩溃，也不代表测过 START 正常退出或圆圈按键注入。
本次没有物理 PSP 测试，也没有验证线上下载速度或硬件性能。
EOF 测试验证了音频解码与 SRC 提交，未做听音判断。
EOF 后的内核对象列表中已无 video_cancel、aac_decode、pcm_output 工作线程。
