# 界面文案目录

文案保存在 `resources/i18n/` 下的 JSON 静态目录中。当前提供 `zh-Hans`
和 `en-US`，构建时由 `scripts/generate-localization.py` 检查两种语言的键和
占位符，并生成 C++ 查表文件到构建目录的 `generated/`。设备运行时不解析 JSON。

目前接入范围是设置页、设置子页、按键帮助和页面底部的按键提示。其他页面文案
后续迁移时，优先复用现有目录键；新增键要同时加入两种语言。动态参数使用
`{name}` 占位符，生成脚本会拒绝缺键或占位符不匹配的目录。

界面语言随设置写入 `settings.json` 的 `locale` 字段。现有确认键配置
`confirm_with_circle` 继续兼容；新版本同时写入语义字段 `confirm_button`。
