# AGENTS.md — 财神输入法（Caishen IME）

Windows 10/11 原生拼音输入法：C++20 TSF（Text Services Framework）进程内 COM DLL + .NET 8 WPF 独立设置中心。全离线、无网络代码、隐私优先。仅支持 Windows x64。

## 目录结构

- `src/ime` — TSF 接入层：COM 生命周期、编辑会话、按键状态机、`ui/` 下 Win32 + DirectWrite 无焦点候选窗/状态窗
- `src/engine` — 拼音引擎：双数组 Trie 检索、模糊音纠错、N-gram 语言模型、用户词学习、剪贴板/自定义短语/置顶候选
- `src/common` — 日志、运行时配置（settings.ini）、私有 DACL、打字统计
- `settings/` — WPF 设置中心（`ShuruSettings.csproj`，net8.0-windows），含剪贴板监听与 `v` 模式粘贴协议
- `data/lexicon` — 系统词库与二进制模型（由 `manifest.json` 固定版本与哈希）
- `scripts/` — 构建、注册、词库生成（Python）、安装打包脚本
- `tests/engine_playground` — C++ 测试（CTest）；`tests/settings_logic`、`tests/engine_playground_cs` — C# 测试/演练
- `installer/CaishenPinyin.nsi` — NSIS 安装包；`docs/` — 架构与专题文档

## 构建与测试

本机工具链在 `E:\shurufa\tools`（VS2022 BuildTools、CMake、Ninja、dotnet 均为本地便携版），构建前需激活 `. tools\env.ps1`。

```powershell
# 唯一正式入口：配置 CMake + 编译 + 完整 CTest（含 WPF 设置程序发布）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release

# 单项测试排查（先完成一次正式构建）
ctest --test-dir build-release -C Release -R p1_engine -V

# 仅 C# 侧（无需 MSVC）
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

- 产物：`build-release/ShuruIme.dll`；发布输出 `artifacts/release/`
- 安装包：`scripts/build_installer.ps1`（NSIS，输出到 `artifacts/installer/`）
- 环境自检：`scripts/check_env.ps1`

## 架构边界（改动须遵守）

- 分层：TSF 接口层（`src/ime`）→ 引擎层（`src/engine`）→ 数据层（词库 mmap）。UI 渲染与引擎逻辑严格解耦。
- DLL 被加载进每个宿主进程的高频输入路径：保持 KISS、防御性编程、不吞异常；`DllMain` 不等待线程、不销毁窗口（避免 Loader Lock）。
- `SharedEngine` 保证词库每进程只加载一次；词库/模型用只读内存映射，不整份拷入堆。
- 隐私红线：密码/私密 InputScope 完全旁路（不拦截、不学习、不记录）；日志永不写原始输入与候选正文；用户词目录用受保护 DACL，ACL 失败时禁用学习写入但输入照常。
- 用户数据在 `%LOCALAPPDATA%\CaishenPinyin\`；系统词库从 `%ProgramData%\CaishenPinyin\data\lexicon` 版本化目录加载。写文件一律临时文件 + 原子替换，跨进程共享文件加命名互斥量。
- 词库治理：`data/lexicon/manifest.json` 固定来源提交与 SHA-256；`*.gram` 大模型不入库不入包（超 GitHub 100MB 限制）；`user_dict.txt` 是用户数据，升级/安装不得覆盖。

## 编码约定

- C++：MSVC `/utf-8 /W4`，静态链接 CRT（DLL 注入任意宿主，不可依赖目标机 VC++ Redist）；成员变量下划线后缀或 `m_` 前缀随所在模块风格；注释解释“为什么”。
- C#/WPF：.NET 8，私有字段 `_camelCase`；XAML 不硬编码绝对尺寸，保证高 DPI 自适应。
- PowerShell：UTF-8，参数显式声明类型。
- 提交信息：Conventional Commits，**中文描述**（如 `feat: 增加xxx`、`fix: 修复xxx`）。
- 版本号统一改 `src/common/version.h`；该文件被 `.rc` 包含，**只能用 ASCII 字符**，中文产品名走 `RuntimeConfig::display_name`。

## 已知坑

- 中文 Windows 下 Ninja 会错误解码 MSVC 的 UTF-8 `/showIncludes` 前缀——CMakeLists 已显式设置 `CMAKE_CL_SHOWINCLUDES_PREFIX`，勿删。
- IME DLL 一旦被宿主加载就不会自动换新；重载需关闭所有用过它的窗口或注销重启。开发期重注册单个 DLL 用 `scripts/register_ime.ps1`。
- `tsf_e2e_core` 用 `TF_TMAE_NOACTIVATETIP` 隔离机器上已装版本；`p1_engine` 加载白霜全量词库，TIMEOUT 600s 属正常。
- NumPad 数字键永不选词；主键盘数字仅组合态选词（见 `docs/privacy-input-policy.md`）。

## 改动前应读的文档

- 改架构/交互 → `docs/architecture.md`（模块职责、按键状态机、光标跟随回退链）
- 改隐私/标点/数字键盘 → `docs/privacy-input-policy.md`
- 改词库生成/模型 → `docs/lexicon-governance.md`
- 改构建/打包 → `docs/build.md`、`docs/installer.md`
