# 隐私、标点与按键策略

实现依据为 [input_policy.cpp](../src/ime/input_policy.cpp)、
[TextService](../src/ime/text_service.cpp)、
[工具模式判定](../src/ime/ui/ime_ui_logic.h)和
[标点状态机](../src/ime/punctuation_state.cpp)。

## 输入隐私

- `IS_PASSWORD`、`IS_PRIVATE`、数字密码和 PIN 等敏感 InputScope 完全旁路，
  不拦截、不显示候选、不学习、不记录输入。
- 对没有公开 InputScope 的旧式控件，使用已知密码控件类名和标准编辑控件密码样式回退。
- 日志不写原始输入、候选或剪贴板正文。常规详细日志受诊断与内容日志开关约束；
  开发选项 `SHURU_INPUT_LIFECYCLE_TRACE` 默认关闭，只记录事件、状态、对象标识和错误码。
- 字数统计持久化日期与字符数，不保存输入正文。
- 复制记录是独立的本地内容保存功能，会记录用户主动复制的文本、图片和文件路径；
  其历史数据库不等同于诊断日志。
- 运行时无网络上传代码。`Offline` 是配置保留项，关闭它不会启用一个不存在的联网功能。

## 个人数据与沙箱

个人目录为 `%LOCALAPPDATA%\CaishenPinyin\`。学习、短语、置顶、统计、剪贴板及
直接上屏请求采用当前用户受保护 DACL。ACL 初始化失败时停止学习写入，
普通输入继续运行。

安装器和设置程序收敛旧的应用包授权。AppContainer 内的 DLL 使用公共系统词库、
系统快照、皮肤及必要设置，不直接读写上述个人数据。
用户从独立设置程序明确选择内容后交给目标窗口，不等同于向沙箱开放历史文件访问。

系统词库安装在 `%ProgramData%\CaishenPinyin\data\lexicon\` 的版本化目录。
文件更新、数据库事务和请求取消规则见[架构说明](architecture.md)，
用户词代次、清空及跨进程保存见[学习协议](learning-correctness.md)。

## 相关配置

由 `GetRuntimeConfig`、`ReloadRuntimeConfig` 和设置侧 `SettingsStore` 读取
`settings.ini`：

| 名称 | 默认 | 当前含义 |
|---|---:|---|
| `Offline` | `1` | 离线运行配置保留项 |
| `LearningEnabled` | `1` | 是否允许用户词与搭配学习 |
| `ContentLogging` | `0` | 详细诊断日志开关，不授权记录正文 |
| `FullWidthPunctuation` | `1` | 中文全角标点转换 |
| `ShuangpinXiaohe` | `0` | 全拼 / 小鹤双拼选择 |
| `VModeOpenWindow` | `0` | `v` 是否直接打开独立复制记录窗口 |
| `VvModeOpenWindow` | `0` | `vv` 是否直接打开独立短语窗口 |

## 标点与普通组合

`ChinesePunctuationState` 统一处理全角标点、单双引号、书名号、破折号、
省略号和人民币符号。关闭全角转换时保留半角行为。
短暂保留的已提交文本尾部只用于识别小数、URL 和邮箱，不写日志。

普通拼音组合中，主键盘 `1–9` 选择当前页候选，空格提交当前选择，
Enter 和单独 Shift 提交原始输入。Ctrl/Alt/Win 快捷键交给宿主，
Ctrl+Space 是中英文切换的例外。Shift 只在未与其他键组合的释放事件中执行单击动作。

## 数字小键盘与工具模式

数字小键盘始终不用于选词，但不同模式有明确的优先级：

| 模式 | 数字与运算键 |
|---|---|
| 普通拼音组合 | NumLock 开启时提交“原始输入 + ASCII 数字”；小键盘运算键提交 ASCII 运算符 |
| `v/vv` 记录或短语列表 | 主键盘 `0–9` 及 NumLock 开启的小键盘数字追加筛选条件，不提交记录 |
| `vvv` 计算器 | 数字、小键盘运算键及支持的括号参与表达式 |
| 无组合或导航状态 | NumLock 关闭的数字键按宿主导航语义处理 |

`v/vv` 列表用方向键浏览，Enter/Space 提交当前记录，Delete 删除记录并保留列表。
删除末条时选择上一条，空列表继续显示。启用独立窗口选项后，打开行为按配置处理。
独立窗口的搜索框保留正常文字编辑，Delete 不删除记录，直到焦点进入记录列表。

`vvv` 计算器的 Enter 提交结果；这些例外不能用“Enter 永远提交原始拼音”
或“主键盘数字永远选词”概括。

## 直接上屏与粘贴

有有效会话的独立窗口文本通过一次性请求交回原 TSF 上下文，
宿主核对窗口、焦点和敏感范围后提交。取消、超时或上下文变化会终止请求，
不会自动绕过拒绝结果改用系统粘贴。

回退文本写入使用后台 Win32 Unicode 剪贴板；图片使用 OLE PNG/Bitmap。
内部标记用于让监听器忽略自身粘贴，发送前再次核对前台和剪贴板序号。
独立窗口等待剪贴板时保持可见，可用 Esc 取消尚未提交的文本操作。

## 回归范围

`input_policy` 覆盖敏感范围、标点、小键盘和按键状态；
`composition_lifecycle`、`tsf_e2e_core` 覆盖真实 TSF 会话及组合清理；
`candidate_window`、`clipboard_helper`、`settings_logic`、`settings_ui_smoke`
覆盖列表删除、内容保真、占用重试和取消；`appcontainer_acl` 检查真实沙箱权限。

宿主没有正确提供 InputScope、跨线程焦点或特殊编辑控件时仍需实际兼容性验证。
自动化结果不能证明所有第三方软件都具有完全相同的行为。
