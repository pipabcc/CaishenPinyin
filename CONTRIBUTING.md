# 贡献指南

本项目接受代码、测试、文档和可确认来源的数据贡献。社区协作遵循
[行为规范](CODE_OF_CONDUCT.md)，安全漏洞按[安全政策](SECURITY.md)私密报告。

## 提交问题

先搜索已有 Issue。报告输入或安装问题时，提供 Windows 版本、应用版本、宿主名称及位数、
可复现步骤、预期行为和实际结果。涉及升级时说明是否完全退出过旧宿主；
只重注册 DLL 不能替换仍在运行的模块。

截图、日志和测试样例应移除个人输入、真实剪贴板内容、用户名、凭据及无关本机信息。
涉及架构或较大交互调整时，先在 Issue 或 PR 中说明需求、现状和方案。

## 开发环境

- Windows 10/11 x64。
- VS2022 Build Tools，包含 MSVC v143 的 x64/x86 编译工具和 Windows SDK。
- CMake 3.20+、.NET 8 SDK、Python 3.11+；Ninja 可选。
- 打包 Setup 时需要 NSIS 3.x。

C++ 标准为 `CMakeLists.txt` 中的 C++17。维护者可使用 `tools/env.ps1` 加载可选的
仓库工具链，外部贡献者也可使用系统安装的工具。所有命令在仓库根目录执行。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_env.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

日常迭代可加 `-Incremental`，仍会运行相应测试。构建路径、参数和 CI 跳过条件见
[构建说明](docs/build.md)，不要另建不执行验证的正式发布入口。

## 按改动选择验证

| 改动范围 | 重点验证 |
|---|---|
| 拼音检索、模型、学习 | `p1_engine`、`engine_phase1`、学习持久化/压力测试、相关模型测试 |
| TSF、组合、首键恢复 | `input_policy`、`composition_lifecycle`、`tsf_e2e_core`，以及可用的首键交互测试 |
| 候选窗或复制记录 | `candidate_window`、`clipboard_helper`、`settings_logic`、`settings_ui_smoke` |
| 安装、权限、发行 | 部署事务、ACL、`release_health` 与发行包验证 |
| 仅 Markdown | 代码事实、示例参数、相对链接、标题锚点和删除文档后的引用 |

完整构建后可用 `ctest --test-dir build-release -C Release -N` 查看当前注册测试；
测试数量会随代码变化，不以旧报告中的固定数量为准。只改文档不需要重跑耗时的词库测试。

```powershell
ctest --test-dir build-release -C Release -R "candidate_window|clipboard_helper|composition_lifecycle" --output-on-failure
dotnet run --project tests\settings_logic
```

`settings_ui_smoke` 和五项 `firstkey_recovery*` 测试需要交互桌面。自动化用例可检查
真实 TSF 对象、模拟通知和合成记录，但不能代替每个宿主编辑器的实际兼容性验收。
报告验证时说明实际执行的范围、跳过原因和测试环境。

只有 C# 工具链时可开发设置中心及逻辑测试：

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
dotnet run --project tests\engine_playground_cs
```

C# 演练程序不能替代原生 C++ 引擎或 TSF 兼容性测试。

## 代码与数据约定

- 保持函数职责明确；查询、持久化、TSF 编辑会话和 UI 渲染分离。
- 输入法运行在宿主进程的高频路径中，避免整份复制系统词库、同步等待磁盘或无界搜索。
- 严格校验输入范围、文件边界、记录 ID 和跨进程请求；关键错误保留诊断，不吞掉业务失败。
- C++ 使用 `/utf-8 /W4` 和静态 CRT，命名沿用模块风格；注释解释约束和原因。
- C#/WPF 使用 .NET 8；新增私有字段优先 `_camelCase`，布局应适应高 DPI。
- 含中文且由 Windows PowerShell 5.1 执行的脚本使用 UTF-8 BOM，参数显式声明类型。
- 修改应用版本只改 `src/common/version.h`，并保持该文件 ASCII；
  词库版本和哈希按[词库治理](docs/lexicon-governance.md)单独维护。
- 新测试使用隔离用户目录和合成数据。需要操作剪贴板时使用隔离窗口站或不改动内容的受控测试。
- 不提交本机工具链、构建产物、真实用户词、剪贴板数据库、密钥或来源不明的素材。

提交第三方词库、皮肤或其他资源前，确认来源与再分发权限，并同步
[第三方声明](THIRD_PARTY_NOTICES.md)和相应许可证文件。

## 提交与合并流程

有仓库写权限时直接使用功能分支，其他贡献者使用 Fork。分支基于最新 `main`，
将不同目的的修改分成可审查的提交：

```powershell
git switch -c codex/your-change
git add -- README.md
git commit -m "docs: 更新使用说明"
git push -u origin codex/your-change
```

提交使用 Conventional Commits 和中文描述：

| 类型 | 示例 |
|---|---|
| `fix` | `fix: 修复删除记录后误上屏` |
| `feat` | `feat: 增加记录列表键盘导航` |
| `docs` | `docs: 对齐安装与重载说明` |
| `refactor` | `refactor: 提取词典查询公共逻辑` |
| `perf` | `perf: 减少输入路径的同步写盘` |
| `test` | `test: 覆盖剪贴板占用时取消操作` |
| `chore` | `chore: 更新发行包校验脚本` |

向 `main` 发起 PR，填写[模板](.github/PULL_REQUEST_TEMPLATE.md)，说明触发条件、
改动后的行为和实际验证结果。涉及行为或接口变化时同步相关文档，将尚未发布的功能
写入 [CHANGELOG](CHANGELOG.md#未发布)。合并前完成代码审查，包括异常、重入、取消、
并发和边界场景；根据 CI 和审核意见继续修订。

提交 PR 表示内容由你原创，或你有权按项目的 [GPL-3.0-only](LICENSE) 及相应第三方
许可证提供。项目不要求单独签署贡献者许可协议。
