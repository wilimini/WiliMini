# 界面文案目录

文案保存在 `resources/i18n/` 下的 JSON 静态目录中。当前提供 `zh-Hans`
和 `en-US`，构建时由 `scripts/generate-localization.py` 检查两种语言的键和
占位符，并生成 C++ 查表文件到构建目录的 `generated/`。设备运行时不解析 JSON。

目前接入范围是设置页、设置子页、按键帮助和页面底部的按键提示。其他页面文案
后续迁移时，优先复用现有目录键；新增键要同时加入两种语言。动态参数使用
`{name}` 占位符，生成脚本会拒绝缺键或占位符不匹配的目录。

界面语言随设置写入 `settings.json` 的 `locale` 字段。现有确认键配置
`confirm_with_circle` 继续兼容；新版本同时写入语义字段 `confirm_button`。

## 按键图标

手柄按键不再用 Unicode 字符（△ □ ○ × ←→ 等），而是画成图标，随文字大小缩放并使用文字颜色。
文案里写 `{icon_名称}`，生成脚本把它展开为私有区码点（U+E000–U+E023），`drawText` 看到这些码点时
调用 `client/button_icons.cpp` 按需绘制并缓存为 alpha 掩码，不需要图片资源。

| 名称 | 图标 |
|---|---|
| `cross` `circle` `square` `triangle` | ✕ ○ □ △ 线框符号 |
| `up` `down` `left` `right` | 十字键，仅按下方向的箭头末端高亮（中心不亮） |
| `up_down` `left_right` `dpad` | 十字键，上下 / 左右 / 四向高亮 |
| `l` `r` | 肩键，L 左上、R 右上为大圆角，字母用正文字体 |
| `start` `select` | 胶囊形 START / SELECT，字母用正文字体 |

`{confirm}` / `{back}` 由 `buttonGlyph()` 解析为 ✕ 或 ○ 图标。C++ 字面量使用
`client/button_icons.hpp` 的 `BTN_*` 宏（如 `"按 " BTN_SQUARE " 搜索"`）。新增图标要同时修改
`button_icons.hpp`、`button_icons.cpp` 和 `scripts/generate-localization.py` 的 `ICONS` 表。
`tests/button_icons_test.cpp` 是主机端检查（`g++ -std=c++17 tests/button_icons_test.cpp client/button_icons.cpp`）。
