# 财神输入法（Caishen Pinyin IME）

[![Windows CI](https://github.com/pipabcc/CaishenPinyin/actions/workflows/ci.yml/badge.svg)](https://github.com/pipabcc/CaishenPinyin/actions/workflows/ci.yml)
[![GitHub Release](https://img.shields.io/github/v/release/pipabcc/CaishenPinyin?display_name=tag)](https://github.com/pipabcc/CaishenPinyin/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4)](https://github.com/pipabcc/CaishenPinyin/releases/latest)

财神输入法面向 Windows 10/11 x64，使用 C++17 和 Windows TSF 实现进程内输入服务，
通过 Win32、GDI+ 与 DirectWrite 绘制无焦点候选窗，使用 .NET 8 WPF 提供设置中心。
输入法运行时全离线，支持全拼、小鹤双拼、模糊音、用户词学习、自定义短语、皮肤和复制记录。

本文介绍当前源码。最新公开发行版为 [v2.0.2](https://github.com/pipabcc/CaishenPinyin/releases/tag/v2.0.2)；
尚未随发行包提供的修改列在 [更新日志的未发布部分](CHANGELOG.md#未发布)。

## 下载与安装

| 发行文件 | 使用方式 |
|---|---|
| `CaishenPinyin-2.0.2-win-x64-Setup.exe` | 双击安装；支持升级、同版本修复、开始菜单入口和标准卸载 |
| `CaishenPinyin-2.0.2-win-x64-Portable.zip` | 完整解压后，以管理员身份运行包内安装脚本，并保留解压目录 |

两个包都包含 x64 输入法 DLL、供 WOW64 应用使用的 x86 DLL，以及自带 .NET 8
桌面运行时的设置程序。支持的是 x64 Windows 上的 64 位和 32 位应用；不提供
32 位 Windows 或 ARM64 原生发行包，也不支持其他操作系统。

安装后按 `Win + Space` 选择“财神输入法”。升级后应完全退出使用过输入法的应用，
包括托盘后台，再重新打开；仍有进程使用旧 DLL 时，可保存工作后注销并重新登录。
重注册 DLL 不能替换应用中已经加载的旧模块，详见[升级后的重载与排查](docs/installer.md#升级后的重载与排查)。

当前发行包未进行 Authenticode 代码签名，Windows 可能显示“未知发布者”或
SmartScreen 提示。下载后核对随附的 `.sha256`：

```powershell
Get-FileHash .\CaishenPinyin-2.0.2-win-x64-Setup.exe -Algorithm SHA256
Get-FileHash .\CaishenPinyin-2.0.2-win-x64-Portable.zip -Algorithm SHA256
```

哈希用于核对文件完整性，不能替代发布者签名。Setup 从 Windows“已安装的应用”
卸载，默认保留个人数据；Portable 使用原解压目录中的卸载脚本注销组件。

## 当前功能

- **拼音输入**：全拼、小鹤双拼、简拼及全拼与声母交错的混拼，支持中英混输和模糊音纠错。
- **候选选择**：普通候选支持翻页、展开、方向键选择和置顶；追加加载时保持已显示候选的位置。
- **本地学习**：学习词和词间搭配保存在当前用户目录，支持撤销、导入、导出和清空。
- **词库与模型**：内置白霜派生中文词库、英文词库、字符 N-gram 回退模型和短词先验，
  可选加载用户自行提供的完整墨奇模型。
- **自定义短语**：输入码精确匹配时插入指定候选位置，也可通过 `vv` 浏览和搜索。
- **复制记录**：`v` 浏览文本、图片和文件路径，支持搜索、上下键浏览和 `Delete` 删除；
  删除后保留列表并选中相邻记录。记录不保存 HTML 或 RTF 富文本格式。
- **文本上屏**：独立窗口有有效 TSF 会话时直接提交文本；回退粘贴在后台准备剪贴板，
  等待期间可以取消，失败后保留窗口。图片仍通过系统剪贴板传递。
- **外观与工具**：内置皮肤、SSF 导入、候选字体、多显示器/DPI 适配、`F9` 软键盘和
  `vvv` 计算器。设置中心提供常规、输入方案、短语、剪贴板、外观与皮肤、词库与隐私、帮助七页。
- **冷加载缓存**：EngineSnapshot v2 将系统词条和索引映射为只读数据；
  每个宿主进程共享一份引擎，缓存失效时回退到源词库重建。

当前词库清单的逻辑版本为 `2.0.1`，与应用版本独立维护，包含 `677,441` 条基础字词、
`8,247` 条单字读音和 `250,504` 条英文记录。权威数据见[词库清单](data/lexicon/manifest.json)。

## 常用按键

| 场景 | 按键 | 行为 |
|---|---|---|
| 普通拼音组合 | `Space` | 提交当前选中的候选 |
| 普通拼音组合 | 主键盘 `1–9` | 选择当前页候选 |
| 普通拼音组合 | `Enter` | 提交原始输入字母 |
| 普通候选 | `← / →` | 前后选择候选 |
| 普通候选 | `↑ / ↓`、逗号/句号 | 展开候选，展开后按行导航 |
| 普通候选 | `Tab` | 展开或收起候选 |
| 候选列表 | `PageUp / PageDown`、`- / =` | 翻页 |
| `v / vv` 原生列表 | `↑ / ↓`、`Delete` | 浏览或删除选中记录，保留列表 |
| `v / vv` 原生列表 | 数字键 | 追加数字筛选条件，不用数字选记录 |
| `v / vv` 原生列表 | `Space / Enter` | 提交选中记录；启用独立窗口选项时按配置打开窗口 |
| 独立记录窗口 | 搜索框内 `↑ / ↓` | 将焦点移入记录列表；`Delete` 在搜索框内仍编辑文字 |
| `vvv` 计算器 | `Enter` | 提交计算结果 |
| 组合或记录窗口 | `Esc` | 取消组合或关闭窗口 |
| 任意非敏感输入框 | 单击 `Shift` | 有组合时提交原始字母，无组合时切换中英文 |
| 任意非敏感输入框 | `Ctrl + Space` | 切换中英文 |
| 任意非敏感输入框 | `F9 / F10` | 切换软键盘 / 全拼与小鹤双拼 |

数字小键盘不用于选词。普通组合中按数字小键盘会提交原始输入和 ASCII 数字或运算符；
在 `v/vv` 中，NumLock 开启的数字键用于筛选；在计算器中用于表达式输入。
`VModeOpenWindow` 和 `VvModeOpenWindow` 默认关闭，可在设置中启用独立窗口。
完整规则见[隐私与输入策略](docs/privacy-input-policy.md)。

## 数据与隐私

输入查询、排序、学习和统计都在本机完成。密码、私密及 PIN 等敏感输入范围完全旁路。
诊断日志不应记录原始输入或候选正文；字数统计只持久化日期和计数。

| 数据 | 默认位置 |
|---|---|
| 设置、皮肤、学习词和统计 | `%LOCALAPPDATA%\CaishenPinyin\` |
| 复制记录数据库和图片 | `%LOCALAPPDATA%\CaishenPinyin\clipboard\` |
| 系统词库及版本指针 | `%ProgramData%\CaishenPinyin\data\lexicon\` |
| Setup 程序及版本指针 | `%ProgramFiles%\CaishenPinyin\` |

复制记录功能会在本地保存用户复制的内容。个人数据使用受保护 DACL；学习权限初始化
失败时停止学习写入，基础输入继续运行。AppContainer 宿主使用公共词库和资源，
不直接访问个人学习、短语、置顶、统计或复制记录。

JSON/TSV/INI 等文件采用临时文件和原子替换；共享写入按协议使用命名互斥量。
复制记录使用 SQLite 事务与 WAL，旧版 JSON 首次迁移时使用独立迁移锁。
安全问题请按[安全政策](SECURITY.md)私密报告。

## 从源码构建

在仓库根目录执行。环境需要 Windows 10/11 x64、VS2022 Build Tools 的 x64/x86 C++
工具链与 Windows SDK、CMake 3.20+、.NET 8 SDK 和 Python 3.11+。
Ninja 可选，缺少可用 Ninja/MSVC 环境时构建脚本使用 Visual Studio Generator；
生成 Setup 还需要 NSIS 3.x。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_env.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

正式入口加载可选的 `tools/env.ps1`，验证词库，构建两种架构并执行相应 CTest，
发布 WPF 自包含程序，最后组装 `artifacts/release/`。`-Incremental` 复用编译结果，
仍执行测试。构建目录中的 DLL 位置取决于 Generator，发布目录中的路径固定。

```powershell
# 对已经通过正式构建的发布目录打包，仍执行发行完整性校验。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_portable.ps1
```

仅开发设置逻辑时可运行：

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

构建参数、测试标签、CI 跳过规则和发行产物见[构建说明](docs/build.md)。
安装、开发部署、健康检查及回滚见[安装与部署说明](docs/installer.md)。

## 源码与文档导航

| 目录或文档 | 职责 |
|---|---|
| [src/ime](src/ime) | TSF/COM、编辑会话、按键状态机和候选窗 |
| [src/engine](src/engine) | 词典索引、拼音查询、模型排序、学习和快照 |
| [src/common](src/common) | 配置、日志、DACL 和统计 |
| [settings](settings) | WPF 设置中心、复制记录监听和独立窗口 |
| [架构说明](docs/architecture.md) | 模块边界、数据流、候选窗与上屏流程 |
| [组合生命周期](docs/composition-lifecycle.md) | COM 重入、首键恢复、显示属性和对应回归 |
| [学习一致性](docs/learning-correctness.md) | 代次、增量保存、撤销及跨进程协议 |
| [词库治理](docs/lexicon-governance.md) | 来源、清单、派生缓存和模型分发 |
| [性能与测量](docs/performance-optimization.md) | 查询预算、后台工作及基准复测方法 |

## 许可与参与

源码采用 [GPL-3.0-only](LICENSE)。第三方词库、格式兼容实现和 .NET Runtime 的归属与
许可证见[第三方声明](THIRD_PARTY_NOTICES.md)；完整墨奇模型不随仓库和发行包提供。

贡献流程见[贡献指南](CONTRIBUTING.md)，社区协作遵循[行为规范](CODE_OF_CONDUCT.md)。
可通过 [Bug 模板](https://github.com/pipabcc/CaishenPinyin/issues/new?template=bug_report.md)
报告问题，通过[功能建议模板](https://github.com/pipabcc/CaishenPinyin/issues/new?template=feature_request.md)
提出需求。更新记录见[CHANGELOG](CHANGELOG.md)。

## 友情链接

[LinuxDo](https://linux.do)
