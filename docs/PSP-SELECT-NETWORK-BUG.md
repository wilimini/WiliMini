# PSP select 位图越界导致间歇性网络失败

记录日期：2026-10-27。官方报告：[pspdev/pspsdk #401](https://github.com/pspdev/pspsdk/issues/401)。

## 现象与结论

客户端起初能正常浏览、打开视频，使用一段时间后持续出现“服务器请求失败（43）”，随后又出现错误 7。失败请求的 HTTP 状态为 0，不能据此判断 B 站服务器返回了错误。

已定位到 PSPSDK libcglue 的 `select()`：它把 newlib 的 64 位公共 `fd_set` 当作固件 socket 位图使用。原生 socket 编号超过 63 后，会越界读写栈内存。应用自己的文件描述符仍可能很小，因此按应用描述符检查范围无法发现这个问题。

项目使用本地适配替换 SDK 的 `select()`，不修改全局工具链。用户已确认 SDK 位图修正版可以正常打开视频；长期运行和完整播放覆盖仍需继续验证。

## 根因

审查源码：[PSPSDK select.c，6ef2b21](https://github.com/pspdev/pspsdk/blob/6ef2b21dc440ef8d4125f000301faaa2a470975e/src/libcglue/select.c#L24-L34)。这是审查时的上游源码版本，不代表本机安装 SDK 的确切提交。

本机 PSP 头文件定义 `FD_SETSIZE = 64`，每个掩码字为 32 位，所以 `sizeof(fd_set) = 8`。SDK 使用以下局部变量准备固件参数：

```c
fd_set scereadfds, scewritefds;
```

但 `SCE_FD_SET` 按原生 socket ID 的低 8 位索引该数组。现场映射为：

```text
应用描述符：8
PSP 原生 socket ID：98
目标字索引：98 / 32 = 3
目标字节偏移：3 * 4 = 12
实际缓冲区大小：8 字节
```

第 3 个掩码字已经在缓冲区外，随后 `sceNetInetSelect(nfds=99, ...)` 收到的位图也不足以覆盖这个编号。重复创建 socket 会暴露较大的原生 ID；不能把小应用描述符等同于小原生 ID。

不能只在适配层增加 `FD_SETSIZE`：那会同时扩大公共 `fd_set`，SDK 再将整个结构复制回调用方时，可能写出调用方原来的 8 字节对象。原生位图和公共 ABI 必须分开。

## 错误码为何误导排查

现场有两条错误路径：

- curl 43 是 `CURLE_BAD_FUNCTION_ARGUMENT`。curl 内部轮询失败时，可能把 `CURLM_UNRECOVERABLE_POLL` 转换成这个通用错误。它不是 HTTP 状态码。
- curl 7 是 `CURLE_COULDNT_CONNECT`。本次日志为连接 `api.bilibili.com:443` 约 66 毫秒后失败。PSPLink 捕获到原生 socket 98，固件 `select` 返回 -1；在同一暂停线程读取固件 errno，结果为 9，即 `EBADF`。

SDK 还存在两处相关错误处理缺陷：

1. 写就绪分支失败后没有用 `sceNetInetGetErrno()` 更新 errno，而读分支会更新。先前 `connect()` 留下的 `EINPROGRESS` 因而可能被误读为 `select()` 的错误。
2. socket 出错时仅设置 `count = -1`，没有停止遍历；后面的就绪事件可以把计数加回 0，吞掉错误。

早期曾在 curl 内对 `EINPROGRESS/EALREADY/EAGAIN` 做 PSP 专用兼容处理。该处理让两次视频打开恢复正常，但之后仍出现错误 7，说明它没有解决位图越界。不能把这轮短期成功当作根因已修复，也不能据残留 errno 认定固件本身返回了 `EINPROGRESS`。

## 项目内修复

- `client/psp_select.c`：基于上述 PSPSDK 实现，使用独立、完整清零的 256 位原生位图，保留调用方公共 `fd_set` ABI。
- 为原生编号添加边界检查；固件读写分支失败均立即返回并更新 errno，防止错误计数被覆盖。
- 校验公开输入范围，零超时且无就绪事件时清空输出集合。
- `client/psp_fdman.h`：保留 SDK 描述符映射 ABI。它属于 SDK 内部接口，升级工具链时应重新核对其布局。
- CMake 直接链接项目实现；链接映射确认不再从 `libcglue.a(select.o)` 提取旧实现。包内附 PSPSDK BSD 许可证。

curl 的暂时状态兼容补丁作为独立改动保留。SDK 位图修复本身不依赖吞掉错误：真实固件错误会被准确传递。

## 验证与可复现入口

主机测试：

```sh
sh scripts/test-psp-select.sh
```

该测试编译实际生产适配代码，启用 AddressSanitizer 和 UndefinedBehaviorSanitizer，覆盖原生 ID 0、31、32、63、64、98、127、128、255，检查位图位置及其余字清零，并检查旧 errno、硬错误立即返回、非法输入和零超时路径。测试通过。

curl 暂时状态兼容测试可单独运行：

```sh
sh scripts/test-curl-psp-select.sh
```

其 11 项检查通过；运行前需有脚本准备的 curl 源码和配置。

真机专项入口为 `tests/psp_select_device_test.c`，与 `client/psp_select.c` 一起编译为 PSPLink PRX，在客户端已完成网络初始化时运行。它连续执行 80 次 UDP socket 创建、零超时写就绪查询和关闭，不访问用户账号或远端 HTTP 接口。实测结果：

```json
{"tested":80,"native_ge_64":80,"max_native":180,"failures":0,"first_errno":0}
```

这验证了修正实现能处理实际高编号 socket，并非宣称原版 SDK 完成了相同的 80 次对照测试。原始故障证据来自正在运行的客户端现场及上游源码检查。

正常联网客户端构建、包内文件哈希校验、PSPLink 启动均通过。2026-10-27 用户反馈 SDK 位图修正版“打开正常”。这次复测范围是视频打开，不等于长期播放、所有网络端点和所有 PSP 型号都已验证。

## 部署与追踪

使用当前发布包合并覆盖记忆棒的 `PSP/GAME/WiliMini/`，保留原有登录信息、设置和缓存。不要使用此前仅含 curl 临时兼容处理的旧包。

已提交官方 Issue #401，包含越界计算、原生 ID 98 的现场、相关 errno 缺陷及验证边界。尚未据此声称上游已接受或合入修复。
