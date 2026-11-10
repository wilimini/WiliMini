# 音乐模式（纯音频播放、分 P 队列与歌词）

视频详情页按 `SELECT` 进入音乐模式（`Screen::Music`）。该模式只拉取并解码 DASH 音频流，不下载视频轨道、不初始化 Media Engine、不分配视频帧缓冲，UI 主循环保持运行。

## 按键与交互

确认键和返回键遵循客户端设置。

| 按键 | 控制面板 | 歌词面板 | 播放队列面板 |
| --- | --- | --- | --- |
| 确认 | 播放 / 暂停（结束或失败时重播） | 播放 / 暂停 | 播放所选分 P 并返回控制面板 |
| L / R | 上一分 P / 下一分 P（播放超过 3 秒时 L 先回到本分 P 开头，支持连点切歌） | 上一分 P / 下一分 P | 上一分 P / 下一分 P |
| 左 / 右 | 快退 / 快进 10 秒（支持连点/长按累加，400 ms 合并跳转） | 快退 / 快进 10 秒（支持连点/长按累加，400 ms 合并跳转） | — |
| 上 / 下 | — | 切换字幕轨道 | 移动队列焦点 |
| △ | 切换循环模式（列表循环 / 单曲循环 / 顺序播放） | 切换循环模式 | 切换循环模式 |
| □ | 打开播放队列（多 P 视频） | 打开播放队列（多 P 视频） | 返回控制面板 |
| SELECT | 切换到歌词面板（有字幕时） | 返回控制面板 | 切换到歌词面板（有字幕时） |
| 返回 | 停止音乐并返回视频详情页 | 停止音乐并返回视频详情页 | 停止音乐并返回视频详情页 |

- **播放队列**：以当前视频的全部 `pages`（P1, P2, ...）作为队列，并从详情页当前选中的分 P 开始播放。单 P 视频不提供独立队列面板。
- **循环模式**：支持列表循环（`RepeatMode::All`）、单曲循环（`RepeatMode::One`）和顺序播放（`RepeatMode::Once`），退出音乐模式时保存在 `settings.json` 的 `music_repeat`。
- **预取与恢复**：距离当前分 P 结束不足 15 秒时（非单曲循环且存在下一分 P），工作线程后台预取下一分 P 的音频播放地址与 Range 连接；若用户手动切歌或跳转则取消正在进行的预取。长会话中播放地址过期或连接断开时，最多自动重新获取并从当前位置恢复 2 次（60 秒窗口内）。
- **歌词显示**：音频启动后异步请求当前分 P 的最多 3 条字幕轨道。B 站字幕提供整句起止时间，客户端将这段时间均分给可见字符，按音频时钟逐字填亮和上浮，跨行连续。当前句放大，相邻句错峰滚动，句间空白期保持上一句高亮。快进退、切歌或切换字幕轨时直接定位。歌词页左侧同时显示曲名、进度和播放控件。
- **电源管理**：不启动独立控制器线程；播放未暂停时每 5 秒调用 `scePowerTick(PSP_POWER_TICK_SUSPEND)` 阻止系统休眠断网，同时允许背光按系统设置自动关闭。

## 架构与线程生命周期

- `decoder/include/psp_audio.h` 与 `decoder/src/audio_only.h`：复用 `player.c` 内的单实例 `audio` 引擎与缓冲，提供 `psp_audio_open`、`psp_audio_seek`、`psp_audio_interrupt`、`psp_audio_pause`、`psp_audio_status`、`psp_audio_close` 与 `psp_audio_stop`。`psp_audio_seek` 在已打开的 `AVFormatContext` / `AVCodecContext` 上直接 `av_seek_frame` 并刷新解码器，无需重新探测容器头部或释放硬件音频通道；若跳转中途收到新的快进/快退指令，`psp_audio_interrupt` 立即中断当前预缓冲读取并切换至新目标位置。音频模式下预缓冲后的启动等待缩短为 100 ms。同一时刻不得与 `psp_video_play` 重叠运行。
- `client/music_session.{hpp,cpp}`：单个 `music_session` 工作线程（带 `PSP_THREAD_ATTR_VFPU`）串行拥有 `psp_audio_*` 生命周期、Range 音频源、下一分 P 预取与字幕请求；UI 线程仅通过非阻塞命令与 `view()` 快照交互。退出详情页或应用关闭时，`stop()` 取消传输并等待工作线程回收全部解码、输出和下载线程。
- `client/ui.cpp`（`drawMusic`）：从解码后的封面取色，使用 GE 对低密度网格的顶点颜色插值，绘制 `480×272` 的流动渐变。封面就绪后过渡到新配色，无封面时使用默认配色，暂停和缓冲时停止背景流动。左侧显示居中裁剪的圆角封面，右侧切换控制、歌词或队列。歌词复用大字号字形图集，以线性采样缩放，绘制时只处理视口附近的句子。

歌词使用思源黑体 CN Heavy 的 24 px 位图子集，缺字时回退到文泉驿正文。生成器需要 Pillow 和 fontTools。修改字库时，用 Adobe 发布的 `SourceHanSansCN-Heavy.otf` 运行 `python3 scripts/prepare-lyrics-font.py /path/to/SourceHanSansCN-Heavy.otf`。源文件校验值和字库覆盖范围记录在 `client/assets/font/LYRICS-MANIFEST.json`。
