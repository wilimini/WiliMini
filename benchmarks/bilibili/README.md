# Bilibili 热门视频 DASH 基准测试

本基准测试使用 20 个热门视频。每个样本截取视频中部约 10 秒，保留 B 站原始
360p H.264 视频和 AAC 音频，并分别保存为 `video.mp4` 和 `audio.m4a`。
测试在本地播放 DASH 双轨媒体，不测 CDN、Wi-Fi 或缓存下载速度，也不重新编码媒体。
测试使用生产版 FFmpeg 和 Media Engine 播放器、音频时钟及丢帧策略。

## 已采集数据

- `popular.snapshot.json`：冻结的热门候选、BV/CID、标题和采集时间。
- `corpus.lock.json`：20 个样本的精确来源时间、编码参数、实际时长、大小和 SHA-256。
- `data/<序号-BV>/`：本地媒体和 metadata；媒体不加入 Git。
- `runs/`：独立安装包、日志和报告；不加入 Git。

数据于 2026-10-30（北京时间）采集。视频时长为 9.967 到 10.010 秒，音频时长误差约为一个 AAC 包。
H.264 样本从接近 `(第一页时长 - 10) / 2` 的关键帧开始。工具将起始位置写入
`source_start_s` 和 `midpoint_offset_s`。分 P 视频只使用第一页时长，不使用列表中的总时长。
`10-BV1Nrx76bEZ5` 的 AAC 音轨均为单声道，而播放器只支持双声道，因此该候选标记为 `excluded`，并由
`21-BV1GxsX6YEuP` 补位。工具保留原序号空缺，以便追溯。

工具通过远程 seek 下载中部窗口，再从中选取关键帧，无须下载完整视频。
`-t 10 -c copy` 会按 DTS 截断，因此可能保留重排序后的额外画面。
工具会再保留首次 PTS 大于或等于 10 秒之前的连续解码顺序前缀，不会在中途删除参考帧。
每个样本都必须通过完整 FFmpeg 解码、首包关键帧和零 PTS、时长、编码兼容性及哈希检查。
视频和音频使用相同的源时间窗口。AAC 保留 MP4 edit list 中按包计算的 preroll。

```sh
# 从仓库根目录运行，需要 Python 3、ffmpeg 和 ffprobe。
python3 benchmarks/bilibili/benchmark.py collect
python3 benchmarks/bilibili/benchmark.py verify
```

`collect` 默认使用已冻结的 snapshot，并先校验已有样本。校验失败时，工具不会静默换用其他视频。
创建 snapshot 时，工具会冻结热门榜前 50 个候选，并按顺序选择前 20 个兼容样本。
音频不兼容的候选会明确列入 `excluded`。网络、API 或解码错误会使采集失败，修复问题后可重试。
要重新抽样，请显式指定新的 `--snapshot` 和 `--data`。采集过程会更新 `corpus.lock.json`。
B 站删除视频、重新上传视频或更改接口后，后续采集可能失败。要进行精确对比，请保留当前媒体文件和哈希。
工具以匿名方式发送请求，不保存 Cookie、账号会话、签名 CDN URL 或原始 playurl 响应。

## 构建及 PSP 真机

本工作区已准备好两个完整目录：
`build/benchmark-ready/PSP/GAME/WiliMini/` 为无采样三轮基线包，
`build/benchmark-profile-ready/PSP/GAME/WiliMini/` 为内部采样一轮热点包。
每个目录都包含 20 个样本。每轮测试都要复制完整目录。


首次准备依赖时，请按根目录 README 中的 submodule 和工具链流程操作。使用独立构建目录：

```sh
make package BUILD_DIR=build/benchmark \
  CMAKE_ARGS='-DBILI_DEVICE_PLAYBACK_TEST=ON -DBILI_DIST_DIR=build/benchmark-dist'

python3 benchmarks/bilibili/benchmark.py stage \
  --package build/benchmark-dist/PSP/GAME/WiliMini \
  --destination build/benchmark-stage/PSP/GAME/WiliMini \
  --repeat 3 --buffers 2 \
  --environment 'PSP-3000 / ARK-5 / 333MHz / ms0 / Release / profile off'
```

`stage` 的目标目录必须不存在。不要将目标设为日常客户端目录。
先备份日常客户端，再将新目录完整复制到 `ms0:/PSP/GAME/WiliMini/` 或 `ef0:/PSP/GAME/WiliMini/`。
默认使用 2 帧 RGB 缓冲，也适用于 PSP-1000 内存配置。测试其他缓冲配置时，请另开一轮。
添加 `--profile` 可启用 H.264 内部热点采样。基线版和优化版必须使用相同的媒体、机型、固件、频率、存储、缓冲配置和构建参数。
正式计时前请关闭所有采样。当前默认启用精确 packed chroma。要构建标量对照版，请添加 `-DBILI_PACKED_CHROMA=OFF`。
两轮真机对比结果见 [hardware-results.md](hardware-results.md)。

先读 `docs/debug.md`。PSP 已进入 PSPLink 后，在主机根目录启动：

```sh
usbhostfs_pc -vv -b 11000 "$PWD"
pspsh -p 11000
```

在 **pspsh 内**执行以下命令，并先保存线程和内存基线：

```text
thlist
meminfo
cd host0:/
./build/benchmark/loader/bootstrap.prx
```

检查 `LOADER root=`，确认加载器选中了刚复制的完整基准测试包。不要直接加载客户端 PRX。
工具会按顺序播放每个样本指定的次数。默认共播放 60 次，约需 10 分钟，日志标记为 `BENCH begin/end`。
按圆圈可取消当前播放并停止整轮。测试结束后按 START 正常退出。
也可以在两次播放之间按 START 中止测试。再次运行 `thlist`，确认所有应用工作线程都已退出。
工具不会自动执行 `reset`。不要使用 PSPLink `scrshot` 或 VRAM `savemem`。

结果位于设备的 `WiliMini/benchmark-results/`。每次调用各有独立目录：

```text
benchmark-results/index.jsonl
benchmark-results/01-BV.../r01/results/run-001/summary.json
benchmark-results/01-BV.../r01/results/run-001/frames.csv
benchmark-results/01-BV.../r01/results/run-001/presentation.csv
benchmark-results/01-BV.../r01/results/run-001/profile.csv   # 仅 --profile
benchmark-results/01-BV.../r01/results/run-001/packets.csv   # 仅 --profile
```

工具会在每次播放后刷新索引。遇到错误、取消或未到 EOF 时，工具会停止后续播放。
如果设备上已有 `benchmark-results`，工具会拒绝开始新一轮。请先完整移出旧结果，再重新运行。
连同 `benchmark.json`、`benchmark-manifest.json`、`benchmark-build.json`、
`build.json`、启动/退出日志一起归档。保留构建目录中的 ELF、map 和 CMakeCache，
用于地址归属和构建参数核对。日志根目录必须较短，设备入口限制为 110 字符。

## PPSSPP 一键运行

需要使用支持现有 headless 参数和原生 ME 的 PPSSPP，并自行准备对应机型的 flash0。
工具不会下载或打包固件。请先按 `tools/README.md` 准备 runner：

```sh
python3 benchmarks/bilibili/benchmark.py run \
  --package build/benchmark-dist/PSP/GAME/WiliMini \
  --runner build/ppsspp-headless/PPSSPPHeadless \
  --firmware /path/to/flash0 --model 3000 \
  --output benchmarks/bilibili/runs/baseline \
  --repeat 3 --buffers 2 \
  --environment 'PPSSPP commit / macOS arm64 / native ME / Release / profile off'
```

每轮都要使用新的输出目录名。`--build` 默认值为 `build/benchmark`，`--data` 默认指向本目录下的 data。
工具默认在模拟时间达到 `20 × repeat × 15 + 30` 秒时按 START。三轮测试的时间为 930 秒，之后等待 15 秒超时；墙钟上限为 600 秒。
增加重复次数或使用较慢环境时，请调整 `--exit-at` 和 `--timeout`。
模拟器超时后，工具会以非零状态退出并写入 `execution.json`。独立 report 显示播放完成，不代表进程正常退出，因此仍需保存日志。
原生后端固定使用 `psp-native`。工具不接受 sceMpeg HLE 解码来替代软件 H.264 负载。

加 `--profile` 得到播放器内部采样。加 `--jit-profile` 则使用 `jit-ir` 核心，
输出 PPSSPP 的 `jit-profile.txt`，包含 guest PC、热点块和宿主开销。
两种采样均有成本；单独跑，勿用作无采样基线。
`host-environment.json` 记录 runner/PRX/入口哈希和运行配置。

## 报告与解释

把设备完整结果目录拷回主机后，或重新分析模拟器结果：

```sh
python3 benchmarks/bilibili/benchmark.py report \
  --game /path/to/copied/WiliMini \
  --output benchmarks/bilibili/runs/device-report
```

- `scores.csv`：每次播放的 wall/decode/render 时间、帧数、丢帧和追赶包数。
- `medians.csv`：每个样本多次播放的时间/帧数中位数。
- `hotspots.csv`：按估计开销排序的 H.264 内部阶段，仅有采样时生成。
- `validation.json`：检查样本/重复的完整覆盖、EOF、错误、音频提交、原生 ME
  作业完成、日志未截断和采样配置；缺失或失败时非零退出。

`wall_us` 包含初始化、音频时钟节拍、收尾和播放器日志写入时间。
播放器 summary 会跳过前 **30 个实际输出帧**，再开始计时。各视频的源帧率不同，生产配置中的 `skip_frame=NONREF` 也会改变测量窗口长度。
因此，不要用整段 wall 时间计算解码 FPS。`render_us` 包含转色提交、队列背压和显示节拍等待，不能直接代表 ME 内核运算时间。
`frames` 表示预热后的测量帧数。索引中的 frames 表示包含预热帧的总输出帧数。
请分别记录主动跳过的非参考帧、播放迟到丢帧和下载带宽。

工具对每个热点阶段每 31 次调用采样一次，并用
`max(0, sampled_us - samples * timer_pair_us) * calls / samples`
估算阶段总开销。该值是扣除计时器开销后的近似 wall time，包含调度干扰，不是 PMU 测得的精确 CPU 时间。
确定性采样可能产生周期偏差。样本较短或采样次数较少时，不要据此给热点排序或下结论。
`reconstruct_mb_inclusive`、motion compensation 和 qpel 等父子阶段的统计值会重叠，**不能相加，也不能视为总时间百分比**。
请比较相同样本多轮测试的中位数和分布，并检查呈现帧数、音频错误、丢帧及追赶包数，确认性能变化没有减少实际工作量。

PSPLink / gprof / 硬件计数器 / PPSSPP 的能力、限制及源码证据见
[profiling.md](profiling.md)。本次实际校验记录见 [validation.md](validation.md)。
真机热点及优化结果见[第一轮 packed chroma](hardware-results.md)和
[第二轮 CABAC、运动补偿、Cache、队列](optimization-round2.md)。

四项近似重建（整像素亮度、最近邻色度、单侧双向预测、帧间残差省略）的
实现与 PPSSPP 三轮对比见 [近似重建结果](approximation-results.md)。

关闭主动非参考帧跳过后的完整帧测试和 SDL 记忆棒部署见
[完整帧结果](allframes-results.md)。

最新版本恢复完整残差重建，测试与部署见[残差恢复结果](residual-restored-results.md)。
