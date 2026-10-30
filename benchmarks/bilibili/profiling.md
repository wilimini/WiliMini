# PSP 的性能分析能力

调研日期：2026-10-30。PSP 上不能直接运行 Linux `perf record/stat`：系统不是
Linux，没有该工具的内核接口。可用的路径包括软件阶段计时、PSP 硬件计数器、
SDK gprof，以及模拟器的 guest block / host 分析，观测的时间含义各不相同。

## 现在可以直接跑的路径

本目录的 `--profile` 已接通生产播放器原有的 H.264 采样插桩。
不需要内核权限，也不需要重编 FFmpeg 才能切换开关：当前固定 FFmpeg 补丁
已有 CABAC/CAVLC、重建、运动补偿、残差、qpel/chroma 内核等 180 个计数槽。
前 30 个输出帧预热后开启。`profile.csv` 可定位阶段，`packets.csv` 定位重包，
`frames.csv` / `presentation.csv` 关联显示延迟。采样结果的估计规则见 README。
生产默认仍关闭采样，所有已有零初始化调用者行为保持一致。

内部采样只覆盖主 CPU 的 H.264，AAC、内核、ME 不会凭空获得函数级热点。
AAC 有 summary 中的 decode/convert/output/underrun 统计；ME 有作业提交/完成、
渲染和队列时序。要细分 ME 转色、缩放、DMA，应另做 ME 侧计时/共享结果回传，
并遵守缓存一致性、buffer ownership 和完整停止要求。本次未改 ME 固件。

## PSPLink：线程和硬件 profiler 模式

核对了上游 `psplink/shell.c`、`shellcmd.h`：

- `thlist`、`thinfo <UID>`：线程状态、等待对象、runClocks、抢占次数。
  在相同时间窗口读取两次可判断解码/音频/显示线程的运行分布；不是函数采样。
- `thmon u`：监控用户线程生命周期事件，不是定时 PC 采样。
- `sysstat`：系统状态；用于背景负载和调度上下文。
- `profmode`：只查询当前 profiler mode。
- `profmode t|g|o`：设置 thread/global/off 调试寄存器标志；上游实现明确输出
  “Profiler mode set, you must now reset psplink”。不要在正在跑的实验中切模式。
  若需启用，先保存证据、正常退出，再在独立准备阶段按所用 PSPLink 的要求
  重启/重新进入 PSPLink，重新取得线程基线。它不会自动产生函数热点文件。

SDK 还声明 `sceKernelReferThreadProfiler()` / `sceKernelReferGlobalProfiler()`，
返回 profiler 寄存器指针。是否启用、权限和固件行为须在目标设备单独验证，
不能假定所有自制固件都可用。本次真机已实际使用 `thlist` 保存启动前和正常退出后的线程证据；
没有启用硬件 profiler mode 或调用上述 profiler 指针 API。

## Allegrex 硬件计数器

`pspdebug.h` 提供 `pspDebugProfilerEnable/Disable/Clear/GetRegs`，
`PspDebugProfilerRegs` 包含 CPU/system cycles、memory/VFPU stalls、sleep、
cache miss/writeback、cached/uncached 访问、FPU/VFPU 指令等累计量。
SDK 实现直接读写 `0xBC400000` 的 21 个 32-bit MMIO 寄存器，SDK 示例是
kernel module / kernel main thread，**不能直接把调用塞进当前 user PRX**。
后续可以用小型诊断 kernel bridge 按“清零→开启→短窗口→关闭→读取”导出 JSON，
而不是在每个宏块读寄存器。全局模式受其他线程/USB/音频干扰；不是自动按函数归因。

32-bit 累计计数需处理 wrap：333 MHz 时全速 cycle counter 约 12.9 秒回绕。
整轮 200–600 秒直接相减会丢多次回绕，应按片段或更短窗口采样并用无符号差值。
相同 workload 的 cycle/cache/VFPU 指标可辅助判别算力或访存问题；不能把
模拟器里相同 MMIO 地址的数值视为硬件 cache/stall 模型，也不能把主 CPU
计数器当作独立的 ME 函数 profile。本次只调研，不宣称已完成真机 PMU 支持。

## PSPSDK gprof：可选的独立插桩构建

本机 SDK 有 `pspprof.h`、`gprof_start()`、`gprof_stop(path, 1)` 和
`samples/gprof/custom`；示例编译与链接都使用 `-pg -g`，用配套
`psp-gprof` 分析匹配的 ELF 和 gmon 数据：

```sh
psp-gprof path/to/matching.elf path/to/gmon.out > gprof.txt
```

要观察 FFmpeg 调用图，FFmpeg 本身也需以 `-pg` 重建，不能仅给客户端加标志。
`-pg` 会增加函数入口成本、代码体积、内存和调度扰动；汇编和 inline 函数不会
自动获得同样精度。PRX 重定位、线程和 SDK 版本都需用小程序先确认。

特别检查了当前上游 `src/prof/prof.c`：`__mcount()` 设置 `gp.pc = selfpc`，
约 1 ms VTimer 的 handler 增加这个**最后记录的函数入口地址**的 histogram，
而不是读取被中断指令的精确 PC。因而不要把它称作 Linux perf 等价的无插桩
指令采样；跨线程入口、未插桩库、阻塞等均会影响归因。优先使用现有阶段插桩，
需要更广调用图时再单独构建 gprof。本次未启用 `-pg`，未产出真机 gmon 文件。

## PPSSPP：已验证 JIT 热块输出

本机验证的 PPSSPP commit：`83e616eaf97bd493f91944e5eced70d914b4bc18`。
其 headless `--help` 提供 `--cpu=jit-ir` 和 `--jit-profile=FILE`。
本目录 `run --jit-profile` 已实际导出热点报告，全部 20 个片段仍走 native ME。
不要求安装 Linux perf，也不要求改 PSP 客户端代码。

源码 `IRNativeCommon.cpp` 的 native IR JIT 约每 50 µs 请求采样宿主 emulator
thread PC，将 JIT 地址映射到 guest block；报告区分 guest blocks、其调用例程、
dispatcher、outside-JIT 宿主函数。`IRJit.h` 还说明 IR interpreter 模式逐块计时。
支持情况依平台和 CPU backend 而异；使用当前 runner `--help` 和实际输出确认。
普通旧 JIT 或较旧 PPSSPP 不保证有这个功能。

报告的 guest PC 可在模拟器 debugger 中反汇编并关联当前模块符号。
对 PRX 用 `psp-addr2line -f -C -e matching.elf <link-address>` 时，先用模块
实际 `.text` 加载地址校正重定位：
`link-address = runtime-PC - runtime-text-base + ELF-text-VMA`。
保留对应 ELF/map，勿直接把 runtime PC 交给以零地址链接的 PRX ELF。
也可通过 PSPLink GDB stub / PSP GDB 作带重定位符号的断点和栈检查；反复暂停
会干扰实时音视频，不应当作精确采样基线。

宿主 Linux 可以用 perf、macOS 可以用 Instruments 继续看模拟器本身；其结果是
宿主 JIT/HLE/图形/I/O 成本。既不是 PSP 真实周期，也不包含准确的 PSP cache、
总线、DMA 并发代价。即使 guest 热块对应到 qpel，也只能提供候选优化方向，
最终收益必须用相同样本在 PSP 真机无采样基线确认。

## 源码依据

以下文件均在本次读取并核对；SDK / PSPLink 上游 blob 哈希在
`research-sources.json`。它们记录调研依据，不是本项目新增依赖。

- [PSPSDK profiler API 与实现](https://github.com/pspdev/pspsdk/tree/master/src/prof)
- [PSPSDK MMIO profiler](https://github.com/pspdev/pspsdk/blob/master/src/debug/profiler.c)
- [PSPSDK kernel profiler 示例](https://github.com/pspdev/pspsdk/blob/master/src/samples/debug/profiler/main.c)
- [PSPLink shell 命令实现](https://github.com/pspdev/psplinkusb/blob/master/psplink/shell.c)
- [PSPLink 命令列表](https://github.com/pspdev/psplinkusb/blob/master/psplink/shellcmd.h)
- [PPSSPP 命令行参数](https://github.com/hrydgard/ppsspp/blob/83e616eaf97bd493f91944e5eced70d914b4bc18/Core/CmdLine.cpp)
- [PPSSPP IR JIT profile](https://github.com/hrydgard/ppsspp/blob/83e616eaf97bd493f91944e5eced70d914b4bc18/Core/MIPS/IR/IRNativeCommon.cpp)
- [PPSSPP host sampler](https://github.com/hrydgard/ppsspp/blob/83e616eaf97bd493f91944e5eced70d914b4bc18/Core/MIPS/JitCommon/JitProfiler.cpp)
