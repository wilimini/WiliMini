# 内置拼音输入法

PSP 系统键盘（`sceUtilityOsk`）只在系统语言设为中文时才提供中文输入。其他语言的固件（包括多数自制系统）
无法输入中文，搜索和评论就不能用。客户端因此自带一个拼音输入法，在设备上本地运行，不需要联网。

## 选择输入法

设置 → 输入法，保存为 `settings.json` 的 `input_method`：

| 值 | 行为 |
|---|---|
| `pinyin`（默认） | 内置拼音，不检查系统语言 |
| `system` | PSP 系统键盘 |

包内没有 `pinyin.dat` 时，两种设置都会回到系统键盘。早期构建写入的 `auto` 读作 `pinyin`。

## 操作

| 按键 | 作用 |
|---|---|
| 方向键 / 模拟摇杆 | 在键盘上移动；在字母第一行按 ↑ 进入候选词栏，↓ 回到键盘；在候选词栏中左右移动并翻页 |
| 确认键 | 输入所选按键，或在候选词栏中选字 |
| 返回键 | 删除（拼音 → 已输入的文字）；文字为空时取消 |
| □ | 空格；输入拼音时选当前页第一个候选 |
| △ | 中文 / 英文切换 |
| L / R | 候选词翻页 |
| START / SELECT | 完成 / 取消 |

键盘上还有 Aa（大写一次）、123（数字与标点页）、，。和“完成”。拼音输入时按标点或数字键，会先上屏第一个候选。
文本上限 96 个字符，与原先的系统键盘相同。

候选词包含：完整拼音的词和字；首字母缩写（`nh` → 你好）；以输入开头的词（`nih` → 你好）；以及只取前半部分
（`woaini` 先选“我”，剩下的 `aini` 继续转换）。`v` 代表 ü（`lv`、`nv`）。

## 词库

`client/assets/ime/pinyin.dat`（约 2.5 MB）由 `scripts/prepare-pinyin-dict.py` 生成。数据来自
[rime-ice](https://github.com/iDvel/rime-ice)（GPL-3.0）的 `8105.dict.yaml`（单字，含全部读音）和
`base.dict.yaml`（词语，取权重最高的 9 万条）；脚本下载后校验 SHA-256，也可用 `--source-dir` 指定本地目录。
只保留字体能显示的字（BMP 汉字区）。许可证随包放在 `licenses/rime-ice.txt`，并列在设置 → 关于 → 许可证中。

文件分三张表（完整拼音、缩写、短前缀的最佳词），格式见脚本头部注释。词库留在记忆棒上，运行时只把每张表的
块索引（约 0.3 MB）和音节表读入内存，每次按键从文件读取几个约 1 KB 的块；关闭键盘时全部释放。
主机测试里每次按键约 20–200 µs，真机的记忆棒读取速度未测。

重新生成：

```sh
python3 scripts/prepare-pinyin-dict.py                 # 下载并校验
python3 scripts/prepare-pinyin-dict.py --source-dir DIR
```

## 代码与测试

- `client/pinyin_ime.{hpp,cpp}`：词库读取与候选词生成，没有平台依赖。
- `client/ui.cpp`：键盘界面（`imeScreen`）和 `ui_input_text` 中的选择逻辑。
- `tests/pinyin_ime_test.cpp`：主机测试（`sh tools/test-host.sh`），使用真实词库。
- `tools/ppsspp/test-ime-smoke.py`：PPSSPP 中用方向键输入并检查截图。

## 限制

- 不支持移动光标：只能在末尾输入和删除。
- 没有词频学习和用户词库；排序来自 rime-ice 的权重。
- 词库来自简体词库，不提供繁体输出。
- 只在 PPSSPP 中运行过；真机的手感、帧率和记忆棒读取时间未验证。
