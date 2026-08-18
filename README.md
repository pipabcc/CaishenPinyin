# 财神输入法（Caishen IME）

Windows 11 本地拼音输入法，包含 TSF 文本服务、拼音引擎、候选窗口、系统词库和独立设置程序。

## 当前状态

| 模块 | 状态 |
|---|---|
| TSF 输入法 DLL 源码 | 已就绪（需 MSVC 编译） |
| 拼音引擎 / 词库 | 已就绪（约 67.7 万白霜多字词 + 8,247 条单字读音） |
| 候选窗 | 已就绪（Win32 无焦点窗） |
| 设置程序 (WPF) | 常规、全拼/双拼、自定义短语、词库与隐私设置已连接运行时 |
| C# 引擎演练 | 可运行 |
| 本机 C++ 工具链 | **已安装到 E:\\shurufa\\tools，可编译** |
| 发布版本 | **2.0.1**（模型自动回退、单字全量召回与通用纠错增强） |

## 架构

```
按键 → TSF TextService → PinyinEngine → Dictionary
                 ↓
           CandidateWindow
                 ↓
              上屏汉字
```

- `src/ime`：TSF / COM / 候选窗
- `src/engine`：拼音查询与用户词学习
- `data/lexicon`：词库
- `settings`：设置 UI
- `scripts`：构建 / 注册 / 环境检查

## 快捷键（MVP）

- 字母：进入拼音组合
- `Space`：上屏首选
- `1-9`：选词
- `Esc`：清空
- `Enter`：上屏原始拼音
- 组合输入时单独按 `Shift`：上屏原始拼音；无组合时单独按 `Shift`：切换中英文
- `Ctrl+Space`：中英切换
- `←/→`：切换候选
- `F9`：开关软键盘
- 组合输入时按 `F10`：切换全拼/双拼

## 开发环境要求

1. Visual Studio 2022 Build Tools（MSVC v143）
2. Windows 10/11 SDK（含 `msctf.h`）
3. CMake >= 3.20
4. .NET 8 SDK（设置程序 / C# 演练）
5. NSIS 3（仅构建图形安装包时需要）

检查环境：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

## 构建

唯一正式构建入口是 `scripts\build.ps1`。它会配置并编译、运行完整 CTest，并在未指定 `-NoPackage` 时生成发布包；不要直接调用 CMake 或历史批次脚本。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

默认构建目录为 `build-release`，发布包目录为 `artifacts\release`。设置中心以
`win-x64` 自包含方式发布，目标电脑无需预装 .NET 8 Desktop Runtime。如需在本地运行
完整 Grammar 测试，可用 `-GrammarPath <rime-moqi-zh.gram>` 指定并校验模型；该文件
始终不会进入发布包或安装目录。其他参数包括 `-BuildDir`、`-OutputDir`、
`-SigningPolicy Off|IfPresent|Required` 和 `-NoPackage`。当前无签名安装包方案显式使用
`-SigningPolicy Off`；取得 Authenticode 证书后，签名发布应改为 `Required`。

## 安装与注册

构建单文件图形安装包（内部会先执行正式构建和完整测试）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```

输出为 `artifacts\installer\CaishenPinyin-<version>-win-x64-Setup.exe` 及对应
SHA-256 文件。安装页的“设为默认输入法”默认勾选；安装后会在 Windows“已安装的应用”
中注册 `Uninstall.exe`。当前安装包未签名，分发时 Windows 可能显示“未知发布者”或
SmartScreen 提示，这属于既定的无签名方案限制。

开发环境也可在管理员 PowerShell 中直接部署发布包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
  -Action Install `
  -DllPath artifacts\release\ShuruIme.dll `
  -SettingsPath artifacts\release `
  -PackagePath artifacts\release\data\lexicon `
  -Version 2.0.1 `
  -SetDefaultInputMethod `
  -HealthCheckExe build-release\release_health_check.exe
```

安装后可从开始菜单打开“财神输入法设置”，也可在候选框内右键直接打开设置。原“中 / 全 / 键 / 设”悬浮状态栏默认永久隐藏，软键盘仍可通过 `F9` 使用。

正式卸载请使用“设置 → 应用 → 已安装的应用”中的财神拼音卸载程序。默认保留设置、
皮肤、剪贴板记录、词库和用户自行安装的 Grammar；只有显式勾选删除个人数据时才会
删除这些目录。开发阶段仅注销当前 DLL 时可使用：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\unregister_ime.ps1
```

## 无 C++ 工具链时的验证

```powershell
dotnet run --project tests\engine_playground_cs
dotnet build settings\ShuruSettings.csproj -c Release
```

在演练程序输入 `nihao`、`shurufa`、`women` 可验证词库排序。

## 词库格式

`data/lexicon/base_dict.txt`：

```text
pinyin<TAB>词<TAB>词频
nihao	你好	9000
```

## 版本化词库与部署

词库 manifest 生成/校验、独立版本目录、side-by-side DLL 安装、健康检查和回滚见：

- `docs/lexicon-governance.md`
- `docs/deployment.md`
- `docs/installer.md`
- `docs/privacy-input-policy.md`

`user_dict.txt`、`custom_phrases.txt` 和字数统计始终保存在 `%LOCALAPPDATA%`，升级流程不会覆盖。

## 设置与生效

设置保存在 `%LOCALAPPDATA%\CaishenPinyin\settings.ini`，采用同目录临时文件 + 原子替换。设置程序可控制全拼/双拼、学习、内容日志（默认关闭）、模糊拼音、全/半角标点、候选数量、字体和输入法列表名称，并可管理系统词库、自动学习词及自定义短语。修改输入法列表名称时会请求管理员权限重新注册；它不会修改 Windows 根据 `zh-CN` 显示的“简体”。自定义短语保存在 `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\custom_phrases.txt`，格式为 `输入码<TAB>短语<TAB>候选位置`。字数统计保存在 `%LOCALAPPDATA%\CaishenPinyin\data\typing_stats.txt`，只记录日期和当日累计计数，不保存输入正文；旧版速度计数桶会在读取时忽略。

## 语言模型说明

`rime-moqi-zh.gram` 是用户自行下载的可选本地统计 N-gram 模型，正式发布包不会携带。
用户可将它复制到当前词库目录 `%ProgramData%\CaishenPinyin\data\lexicon\versions\<current>`；
输入时会通过只读内存映射查询，用于长句词间搭配排序。它不是神经网络或生成式 AI，
也不会联网。运行时按“完整墨奇 → `system_ngram.bin` → 旧版小墨奇”的顺序加载；
未安装完整模型时自动使用随包安装的 `system_ngram.bin`。单字和双字的无上下文常用度由 `system_lexeme_prior.bin` 负责，
与语言模型互补。模型来源和再分发限制见 `THIRD_PARTY_NOTICES.md`。
