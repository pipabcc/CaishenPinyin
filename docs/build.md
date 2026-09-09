# 构建、测试与发行

本页描述构建入口和产物。安装、升级、开发部署及重载见[安装与部署](installer.md)。
编译目标和测试注册以 [CMakeLists.txt](../CMakeLists.txt) 为准。

## 环境

- Windows 10/11 x64。
- VS2022 Build Tools：MSVC v143 的 x64/x86 工具链和 Windows SDK。
- CMake 3.20+、.NET 8 SDK、Python 3.11+。
- Ninja 可选；缺少可用 Ninja/MSVC 环境时使用 Visual Studio 17 2022 Generator。
- NSIS 3.x 仅用于生成 Setup。

C++ 当前使用 C++17、`/utf-8 /W4` 和静态 CRT：
Release 为 `/MT`，Debug 为 `/MTd`。WPF 目标框架为 `net8.0-windows`，
正式发布携带 `win-x64` 桌面运行时。

命令从仓库根目录运行。`scripts/build.ps1` 会加载存在的 `tools/env.ps1`；
本地工具链不纳入 Git。环境检查：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

## 正式入口

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -SigningPolicy Off
```

脚本依次执行：

1. 停止 `ShuruSettings` 进程，以便覆盖设置程序构建输出；校验词库清单并读取应用版本。
2. 编译 WPF 设置程序，配置并构建 x64 CMake 目标，执行按环境筛选后的 CTest。
3. 配置并构建 x86 CMake 目标，执行兼容性测试子集。
4. 发布 WPF 自包含程序，核对两份 DLL 的架构与应用版本。
5. 未指定 `-NoPackage` 时组装发行目录并生成 `release-manifest.json`。

任一步骤失败会停止后续阶段。构建不会自动部署系统输入法；
部分交互测试会打开测试窗口，首次输入测试也可能按需启动剪贴板后台进程。

| 参数 | 含义 |
|---|---|
| `-Config Release\|Debug` | 构建类型，默认 Release |
| `-BuildDir` / `-X86BuildDir` | x64 / x86 构建目录，默认 `build-release` / `build-release-x86` |
| `-OutputDir` | 固定结构的发行目录，默认 `artifacts/release` |
| `-Incremental` | 复用编译结果，仍配置双架构、测试和发布 |
| `-NoPackage` | 仍执行编译、测试及 WPF 自包含发布，只跳过发行目录组装 |
| `-GrammarPath` | 校验并复制本机可选完整模型供开发使用，该模型不随发行包提供 |
| `-SigningPolicy Off\|IfPresent\|Required` | 发布清单的签名校验策略，当前默认 Off |

当前 Setup 流程要求 `SigningPolicy=Off`，并检查最终安装器确实未签名。
改变签名方案时需要同步构建、包校验和安装流程。

## 输出路径

| 产物 | Ninja | Visual Studio Generator |
|---|---|---|
| x64 DLL | `build-release/ShuruIme.dll` | `build-release/Release/ShuruIme.dll` |
| x86 DLL | `build-release-x86/ShuruIme32.dll` | `build-release-x86/Release/ShuruIme32.dll` |
| C++ 测试和工具 | 相应构建目录根 | 相应构建目录的 `Release/` |
| WPF 自包含发布 | `settings/bin/Release/net8.0-windows/win-x64/publish/` | 相同 |
| 发行目录 | `artifacts/release/` | 相同 |

Debug 或自定义目录按参数替换。`artifacts/release/` 中的路径不受 Generator 影响，
主要包含两份 DLL、`ShuruSettings.exe` 及运行时、`engine_snapshot_build_tool.exe`、
`data/lexicon/`、`data/skins/`、法律声明和发布清单。
安装或打包应使用这份完整目录。

## 测试与跳过条件

查看当前测试和执行单项：

```powershell
ctest --test-dir build-release -C Release -N
ctest --test-dir build-release -C Release -R "candidate_window|clipboard_helper|composition_lifecycle" --output-on-failure
ctest --test-dir build-release -C Release -R learning_persistence -V
```

- x64 在交互环境默认运行全部注册测试。
- `CAISHEN_SKIP_INTERACTIVE_TESTS=1` 跳过标记为 `interactive-tsf` 的测试：
  五项 `firstkey_recovery*` 和 `settings_ui_smoke`。
- `ANTIGRAVITY_AGENT=1` 或 `System.Environment.UserInteractive` 为假时，
  脚本同时跳过交互测试和 `p1_engine`。
- x86 执行 `input_policy`、`engine_snapshot`、`release_health`、
  `tsf_e2e_core`、`composition_lifecycle`、`candidate_window`。
- `p1_engine` 使用全量词库，当前超时上限为 600 秒。具体测试数和超时以 CMake 为准。
- `installer_lexicon_recovery` 使用隔离目录，覆盖旧词库访问拒绝、损坏、修复目录复用、
  失败回滚、可选模型保留和源包完整性检查。

GitHub CI 显式设置 `CAISHEN_SKIP_INTERACTIVE_TESTS=1`，不代表已完成真实宿主的前台验证。
输入兼容性、不同 DPI 和显示器仍需按改动进行实测。

仅 C# 侧：

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

界面冒烟测试复用已编译的设置程序。手动运行时必须隔离复制记录目录：

```powershell
$previousClipboardDirectory = $env:CAISHEN_CLIPBOARD_DATA_DIR
try {
    $env:CAISHEN_CLIPBOARD_DATA_DIR = Join-Path (Get-Location) 'artifacts\settings-ui-test\clipboard'
    dotnet run --project settings\ShuruSettings.csproj -c Release --no-build -- -self-test
} finally {
    $env:CAISHEN_CLIPBOARD_DATA_DIR = $previousClipboardDirectory
}
```

该测试覆盖实际 WPF 事件路由、删除焦点、直接上屏请求和剪贴板占用时取消。
CTest 已设置隔离目录。仅 Markdown 修改检查文档事实、链接和示例语法即可，
不需要为此重跑全量词库测试。

## 生成 Setup 与 Portable

```powershell
# 默认重新执行正式构建和测试。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -Config Release

# 使用同一份已验证发行目录生成 Portable。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_portable.ps1 -SigningPolicy Off
```

若已完成正式构建，可给安装器脚本传 `-SkipBuild` 复用产物；
它仍验证清单、文件哈希、版本、架构、运行时、许可证和禁入文件。
`-MakeNsisPath` 可指定 NSIS，默认使用系统安装位置。

输出为 `artifacts/installer/CaishenPinyin-<version>-win-x64-Setup.exe`、
`Portable.zip` 及各自的 `.sha256`。Portable 脚本只打包，不自动编译。

## 版本、模型与发布验收

应用版本只在 `src/common/version.h` 修改，保持 ASCII；构建将它写入原生资源和 WPF
程序集。词库版本按 `data/lexicon/manifest.json` 独立维护，不随应用版本自动变更。

完整墨奇和旧版 `*.gram` 模型不提交、不入包；`system_ngram.bin` 是必需回退模型。
源数据、派生缓存和可选模型规则见[词库治理](lexicon-governance.md)。

发布前核对：

1. 正式构建和适用测试通过，明确被跳过的宿主验证。
2. `scripts/test_release_package.ps1` 校验通过。
3. Setup 和 Portable 来自同一份发行目录，版本、标签与二进制一致。
4. 两种包及 SHA-256 完整，包含法律声明，不含个人数据或禁入模型。
5. 安装、修复、卸载、回滚和默认输入法策略按[安装说明](installer.md)验收。
6. 历史发行说明对应发布标签；尚未发行的变更继续放在 CHANGELOG 的未发布部分。

性能复测见[性能说明](performance-optimization.md)，贡献流程见[贡献指南](../CONTRIBUTING.md)。
