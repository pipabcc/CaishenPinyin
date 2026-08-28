# 财神输入法（Caishen Pinyin IME）

[![Windows CI](https://github.com/pipabcc/CaishenPinyin/actions/workflows/ci.yml/badge.svg)](https://github.com/pipabcc/CaishenPinyin/actions/workflows/ci.yml)
[![GitHub Release](https://img.shields.io/github/v/release/pipabcc/CaishenPinyin?display_name=tag)](https://github.com/pipabcc/CaishenPinyin/releases/latest)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4)](https://github.com/pipabcc/CaishenPinyin/releases/latest)

财神输入法是一款面向 Windows 10/11 x64 的开源中文拼音输入法。核心使用 C++20 和
Windows TSF（Text Services Framework）实现，候选窗基于 Win32/DirectWrite，设置中心
使用 .NET 8 WPF。输入法运行时全离线，支持全拼、双拼、模糊音、本地用户词学习、白霜
拼音词库、自定义短语、皮肤和剪贴板历史。

English summary: an open-source, offline and privacy-focused Windows Pinyin IME built with
C++20, TSF, DirectWrite and .NET 8 WPF.

> 当前发行包尚未进行 Authenticode 代码签名。UAC 会显示“未知发布者”，SmartScreen
> 也可能拦截首次运行。下载后应先核对 Release 页面公布的 SHA-256；校验值一致只能证明
> 文件完整性，不能替代发布者身份签名。

## 下载与安装

最新稳定版：[财神输入法 2.0.1](https://github.com/pipabcc/CaishenPinyin/releases/tag/v2.0.1)

| 发行文件 | 适用场景 | 安装方式 |
|---|---|---|
| `CaishenPinyin-2.0.1-win-x64-Setup.exe` | 推荐给普通用户；支持安装目录、升级/修复、开始菜单和标准卸载 | 双击运行，按安装向导操作 |
| `CaishenPinyin-2.0.1-win-x64-Portable.zip` | 适合测试或手动管理文件；仍需注册 Windows TSF 组件 | 完整解压后，以管理员身份运行包内安装脚本 |

两种包都是 `2.0.1`，区别是部署形式，不是两个不同版本。仅支持 AMD64/x64 Windows，
不支持 ARM64、x86、macOS、Linux、Android 或 iOS。

安装完成后按 `Win + Space`，选择“财神输入法”。输入法 DLL 被宿主进程加载后通常不会
自动换成新文件；升级后若仍显示旧行为，请关闭所有用过输入法的程序，或注销并重新登录。

### 校验下载文件

Release 会同时提供两个 `.sha256` 文件。在 PowerShell 中执行：

```powershell
Get-FileHash .\CaishenPinyin-2.0.1-win-x64-Setup.exe -Algorithm SHA256
Get-FileHash .\CaishenPinyin-2.0.1-win-x64-Portable.zip -Algorithm SHA256
```

输出应与 Release 页面及对应 `.sha256` 文件完全一致。若不一致，请不要运行。

### 卸载与个人数据

Setup 版可从 Windows“已安装的应用”卸载。默认卸载保留设置、皮肤、剪贴板记录和用户词；
选择删除个人数据时会再次确认。Portable 版使用原解压目录中的卸载脚本注销组件，因此安装
后不要随意移动或删除该目录。

## 核心特性

- **原生系统级集成**：基于微软 TSF 文本服务框架，接入 Windows 10/11 桌面应用和支持 TSF 的 UWP/WinUI 宿主；候选窗使用 Win32 与 DirectWrite 绘制，不抢占输入焦点。具体兼容性取决于宿主对 TSF 的实现。
- **纯净离线与隐私保护**：查询、候选排序、用户词学习和统计均在本机完成；运行时不包含网络上传模块，不收集用户击键正文，内容日志默认关闭。
- **白霜拼音与英文词库**：v2.0.1 内置白霜派生词库，共 `677,441` 条字词记录和 `8,247` 条单字读音，并附带常用英文词库。数量以 [词库清单](data/lexicon/manifest.json) 为准。
- **分级回退语言模型**：支持完整模型、内置 `system_ngram.bin` 和字符先验的分级回退；缺少可选大模型时仍可完成基础候选检索和排序。
- **丰富个性化配置**：
  - 支持全拼以及自然码、微软双拼、小鹤双拼等主流双拼方案，可通过设置和快捷键切换
  - 支持用户词自学习、自定义短语（`custom_phrases.txt`）、模糊音和候选置顶
  - 支持皮肤管理、DirectWrite 候选字体调节、多显示器和 DPI 跟随
  - 支持剪贴板历史、文本直接上屏以及图片/富文本粘贴辅助（`v` 模式）
- **现代化图形安装包**：提供 NSIS 单文件 Setup、自包含 .NET 8 设置中心、Portable 便携包，以及带健康检查、回滚和个人数据保护的卸载流程。
- **冷启动快照**：词库二进制索引和 EngineSnapshot 使用只读映射，源数据变化后自动失效并重建，减少多宿主进程的重复解析工作。

## 性能与资源说明

项目只发布能够由仓库工具复现或由发行产物直接核对的指标。v2.0.1 的发布包包含 `release-manifest.json`，记录文件大小和 SHA-256；`p1_engine` 测试会报告文本与缓存加载时间、查询 P50/P95/P99，运行方式如下：

```powershell
ctest --test-dir build-release -C Release -R p1_engine -V
```

EngineSnapshot 的开发机参考数据见 [P1 引擎说明](docs/p1-engine.md)。在一台 Windows 11 开发机的 Release 构建中，白霜全量词库传统装载约需 11 到 12 秒，快照命中后引擎整体约 150 到 160 毫秒。结果受硬件、文件缓存、Windows 版本和宿主进程影响，不代表所有设备的性能保证。

项目没有把微软输入法的内存、延迟和 QPS 写成固定对比值。微软输入法会随 Windows 版本、宿主应用、网络策略和采样口径变化，仓库也没有同机同条件的可复现采集工具。需要横向比较时，请在目标机器上使用相同输入样本、相同宿主和相同采样方法重新测量。

### v2.0.1 可复现实测与对比口径

下表只列出本项目能够由仓库文件或测试命令复核的指标。数值来自 v2.0.1 的发布清单和一次本机 Release 测试输出，硬件、文件缓存、Windows 版本与宿主进程变化后，结果会随之变化。

| 评估维度 | 财神输入法 v2.0.1 | Windows 系统内置微软输入法 | 复核方式 |
|---|---:|---|---|
| 核心 DLL 体积 | 1,472,000 bytes（约 1.40 MiB） | 未在同一采样工具中测量 | `release-manifest.json` |
| 白霜字词记录 | 677,441 条 | 系统词库版本和口径随 Windows 版本变化 | `data/lexicon/manifest.json` |
| 单字读音记录 | 8,247 条 | 未在同一词库口径下测量 | `data/lexicon/manifest.json` |
| 英文词库记录 | 250,504 条 | 未在同一词库口径下测量 | `data/lexicon/manifest.json` |
| 文本词库加载时间 | 本机一次示例约 106,823 ms | 未测 | `ctest -R p1_engine -V` |
| 二进制缓存校验加载 | 本机一次示例约 360 ms | 未测 | `ctest -R p1_engine -V` |
| 查询延迟 P50 | 本机一次示例 358 μs | 未测 | `p1_engine` 输出 `query_us P50` |
| 查询延迟 P95 / P99 | 本机一次示例 442 / 607 μs | 未测 | `p1_engine` 输出 `query_us P95/P99` |
| 连续输入查询 P95 / P99 | 本机一次示例 2,354 / 2,668 μs | 未测 | `p1_engine` 输出 `typing_query_us P95/P99` |
| 运行网络请求 | 0，代码路径不包含联网功能 | Windows 功能、版本和策略可能不同 | 源码审计与运行配置 |

这张表不能推出“财神输入法一定比微软输入法快多少倍”。如果需要正式横向结论，应固定 Windows 版本、宿主程序、输入样本、冷启动状态、网络策略和采样脚本，再分别测量 Working Set、Private Bytes、P50/P95/P99 与吞吐量。

## 隐私与安全边界

- 输入法运行时不需要网络，拼音查询、候选排序、学习和统计都在本机完成。
- 密码及私密 `InputScope` 完全旁路，不拦截、不学习、不记录。
- 内容日志默认关闭；日志设计上不写原始输入串或候选正文。
- 用户词和配置位于 `%LOCALAPPDATA%\CaishenPinyin\`，ACL 初始化失败时会禁用学习写入，
  但基础输入仍可继续使用。
- 系统词库安装在 `%ProgramData%\CaishenPinyin\data\lexicon\` 的版本化目录中。
- 文件更新采用临时文件加原子替换；跨进程共享数据使用命名互斥量协调。

完整规则见 [隐私与输入策略](docs/privacy-input-policy.md)。发现安全问题时，请不要公开披露，
请按照 [安全政策](SECURITY.md) 使用 GitHub 私密漏洞报告。

## 常用按键

| 按键 | 行为 |
|---|---|
| `a-z` | 进入或继续拼音组合 |
| `Space` | 上屏首选候选 |
| 主键盘 `1` - `9` | 组合状态下选择候选；数字小键盘不选词 |
| `Enter` | 上屏原始拼音字母 |
| `Esc` | 清空组合 |
| `Shift` 单击 | 有组合时上屏原始拼音；无组合时切换中英文 |
| `Ctrl + Space` | 切换中英文 |
| `F9` | 打开或关闭软键盘 |
| `F10` | 组合状态下切换全拼/双拼 |

## 架构概览

```text
按键事件
   │
   ▼
TSF 接入层（src/ime）
   ├── 隐私/InputScope 策略
   ├── 组合与编辑会话
   └── Win32 + DirectWrite 候选窗
   │
   ▼
拼音引擎（src/engine）
   ├── 拼音切分、双拼、模糊音
   ├── 双数组 Trie 与候选打分
   ├── N-gram / 字符先验
   └── 用户词、短语与置顶候选
   │
   ▼
只读系统词库 + 本地用户数据
```

主要目录：

- `src/ime`：TSF/COM 生命周期、编辑会话、按键状态机和候选窗口。
- `src/engine`：拼音检索、纠错、语言模型、用户学习和快照缓存。
- `src/common`：运行时配置、日志、私有 DACL 和打字统计。
- `settings`：.NET 8 WPF 设置中心。
- `data/lexicon`：受清单和 SHA-256 约束的系统词库及派生模型。
- `installer`：NSIS 安装器和 Portable 辅助脚本。
- `tests`：C++、C#、TSF 与部署事务测试。

进一步阅读：[架构说明](docs/architecture.md)、[词库治理](docs/lexicon-governance.md)、
[构建说明](docs/build.md)、[安装器说明](docs/installer.md)。

## 从源码构建

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022 Build Tools，含 MSVC v143 与 Windows 10/11 SDK
- CMake 3.20 或更高版本
- Ninja，或 Visual Studio CMake Generator
- .NET 8 SDK
- Python 3.11 或兼容版本
- NSIS 3.x，仅生成 Setup 时需要

仓库中的 `tools/env.ps1` 会优先使用项目本地便携工具链；外部贡献者也可以直接使用已加入
`PATH` 的标准工具链。环境检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

正式构建入口会编译 WPF 设置中心、配置 CMake、编译 C++ 目标、运行 CTest，并生成
`SigningPolicy=Off` 的发行目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

生成 Setup 和 Portable：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -Config Release
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_portable.ps1
```

主要输出：

- `build-release/ShuruIme.dll`
- `artifacts/release/`
- `artifacts/installer/CaishenPinyin-<version>-win-x64-Setup.exe`
- `artifacts/installer/CaishenPinyin-<version>-win-x64-Portable.zip`

仅运行 C# 设置逻辑：

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

## 词库与第三方许可

`data/lexicon/manifest.json` 固定词库版本、来源提交、文件大小和 SHA-256。可编辑文本是权威
源，`.bin` 文件是确定性派生缓存。完整墨奇模型 `rime-moqi-zh.gram` 因上游没有明确许可证，
不会进入源码仓库或官方发行包；用户可以根据自身权利自行安装。

项目源码采用 [GPL-3.0-only](LICENSE)；第三方词库、格式兼容实现和 .NET Runtime 许可见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 与 `licenses/`。提交代码或数据前，请确认你
有权按相应许可证再分发，避免提交来源不明的皮肤、截图、词库或二进制文件。

## 参与项目

- 缺陷和兼容性问题：[创建 Bug Report](https://github.com/pipabcc/CaishenPinyin/issues/new?template=bug_report.md)
- 功能建议：[创建 Feature Request](https://github.com/pipabcc/CaishenPinyin/issues/new?template=feature_request.md)
- 贡献流程：[CONTRIBUTING.md](CONTRIBUTING.md)
- 社区行为规范：[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- 更新历史：[CHANGELOG.md](CHANGELOG.md)

## 友情链接

- LinuxDo — [https://linux.do](https://linux.do)（真诚、友善、团结、专业，共建你我引以为荣之社区）

## 相关技术与检索词

Windows 拼音输入法、Windows 中文输入法、开源输入法、离线输入法、隐私输入法、全拼、
双拼、自然码、小鹤双拼、微软双拼、白霜拼音、Rime Frost、Windows Pinyin IME、Chinese
input method、offline IME、privacy-first IME、TSF input method、Text Services Framework、
C++20、Win32、DirectWrite、WPF、.NET 8。

这些关键词描述本项目的真实平台和技术范围；本项目不是 Rime 前端，也不支持 Windows
以外的平台。
