# 隐私、标点与数字键盘策略

## 隐私

- 默认离线（`Offline=1`），无网络路径。
- `IS_PASSWORD`、`IS_PRIVATE`、PIN 等敏感 InputScope 以及 Win32 密码控件完全旁路：不拦截、不候选、不学习、不记录输入。
- 用户词目录和文件使用受保护 DACL，仅当前用户完全控制。ACL 加固失败时禁用学习写入，但输入和系统词查询继续工作。
- 日志不写原始输入/候选。Info/Debug 还要求同时启用诊断和内容日志。

`%LOCALAPPDATA%\CaishenPinyin\settings.ini` 配置：

| 名称 | 默认 | 说明 |
|---|---:|---|
| `Offline` | 1 | 离线运行保留位 |
| `LearningEnabled` | 1 | 用户词学习 |
| `ContentLogging` | 0 | 详细诊断日志总开关 |
| `FullWidthPunctuation` | 1 | 中文全角标点 |

配置由 `GetRuntimeConfig/ReloadRuntimeConfig` 提供底层接口。

## 标点

`ChinesePunctuationState` 集中维护映射和单双引号状态，支持常用中文标点、书名号、单双引号、破折号、省略号和人民币符号。半角策略关闭时不转换。已提交文本的短尾部仅用于识别小数、URL 和邮箱，不写日志；这些上下文中的点、斜杠、冒号保持半角。

## 数字键盘

- 主键盘 `1..9` 仅在组合态选词。
- `VK_NUMPAD0..9` 永不选词；NumLock 开启时输入数字，关闭时交给宿主处理导航。
- `DECIMAL/DIVIDE/MULTIPLY/ADD/SUBTRACT` 输入 ASCII 运算符。
- 组合态按数字键盘时提交“原始拼音 + 数字/运算符”，不学习。

## 生命周期与测试缺口

Activate 每获得一个资源就记录状态，失败或 Deactivate 对称撤销 text/key/thread sinks、context、UI 和共享引擎引用；UI 只在激活线程操作。DllMain 不等待线程或销毁窗口，避免 Loader Lock。

`input_policy_test` 覆盖标点、引号、NumPad、敏感范围分类和可逆激活资源顺序。真实 TSF 宿主的 COM 失败注入、Word/浏览器密码控件和跨线程宿主行为仍需手工/自动化宿主矩阵验证。
