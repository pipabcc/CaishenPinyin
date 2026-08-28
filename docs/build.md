# 构建与发行说明

## 支持环境

项目只支持 Windows 10/11 x64。标准开发环境需要：

- Visual Studio 2022 Build Tools，包含 MSVC v143、C++ 桌面开发组件和 Windows SDK；
- CMake 3.20 或更高版本；
- Ninja，或 Visual Studio 17 2022 CMake Generator；
- .NET 8 SDK；
- Python 3.11 或兼容版本；
- NSIS 3.x，仅构建 Setup 时需要。

`scripts/build.ps1` 会优先加载仓库中的 `tools/env.ps1`。维护者可以把便携工具链放在
`tools/` 下；该目录除环境入口外不会提交。外部贡献者无需复制维护者目录结构，只要标准
工具已加入 `PATH`，且 Visual Studio 可由 `vswhere` 找到即可。

环境自检：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

## 正式构建入口

唯一正式入口为 `scripts/build.ps1`。它会验证词库清单、构建 WPF 设置中心、配置并编译
CMake 工程、执行 CTest，然后生成自包含发行目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 `
  -Config Release `
  -SigningPolicy Off
```

默认输出：

- `build-release/ShuruIme.dll`；
- `build-release/engine_playground.exe`；
- `artifacts/release/ShuruIme.dll`；
- `artifacts/release/ShuruSettings.exe` 及完整 `win-x64` .NET 8 Desktop Runtime；
- `artifacts/release/data/lexicon/`；
- `artifacts/release/release-manifest.json`。

常用参数：

- `-BuildDir`：CMake 构建目录；
- `-OutputDir`：发行目录；
- `-NoPackage`：只编译和测试，不生成发行目录；
- `-GrammarPath`：仅供本地测试使用的完整墨奇模型；
- `-SigningPolicy Off|IfPresent|Required`：发行签名策略。

当前官方无签名发行固定使用 `Off`。未来启用 Authenticode 时，应同时更新构建、安装器、
发行验证和文档，切换到 `Required`，不能只签最外层 Setup。

## 测试

正式入口运行已注册的 CTest。首次完整构建后可以排查单项：

```powershell
ctest --test-dir build-release -C Release -R p1_engine -V
ctest --test-dir build-release -C Release -R settings_logic -V
ctest --test-dir build-release -C Release -R deployment_core -V
```

仅运行 C# 设置逻辑：

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

GitHub Actions 在无桌面的托管环境运行可自动化的测试。需要真实前台焦点、候选窗观察或多
DPI/多显示器的场景仍需 Windows 真机验收，不能用 CI 结果替代。

## 词库和完整模型

构建前会运行 `scripts/lexicon_manifest.py validate`，检查 `data/lexicon/manifest.json` 中
记录的大小、SHA-256、条目格式和二进制模型结构。

`rime-moqi-zh.gram` 超过 GitHub 普通 Git 文件限制，且上游没有明确再分发许可证，因此：

- 不提交到 Git；
- 不进入 `artifacts/release`；
- 不进入 Setup 或 Portable；
- 仅允许用户根据自身权利自行下载并放入已安装词库目录。

官方包始终包含可公开再分发的 `system_ngram.bin` 回退模型。

## 构建发行包

Setup 的正式入口会重新执行完整 Release 构建与测试，然后验证发行清单、哈希、自包含运行
时、版本号、法律声明和禁入文件，最后调用 NSIS：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1 `
  -Config Release
```

在同一份已验证的 `artifacts/release` 上生成 Portable：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_portable.ps1 `
  -SigningPolicy Off
```

输出：

- `artifacts/installer/CaishenPinyin-<version>-win-x64-Setup.exe`；
- `artifacts/installer/CaishenPinyin-<version>-win-x64-Setup.exe.sha256`；
- `artifacts/installer/CaishenPinyin-<version>-win-x64-Portable.zip`；
- `artifacts/installer/CaishenPinyin-<version>-win-x64-Portable.zip.sha256`。

只修改 NSIS 包装层时可以给 `build_installer.ps1` 传 `-SkipBuild`，但脚本仍会完整验证现有
发行目录，且拒绝非 `SigningPolicy=Off` 的清单。

## 安装调试

正式用户优先使用 Setup。开发期直接部署发行目录时，在管理员 PowerShell 中执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
  -Action Install `
  -DllPath artifacts\release\ShuruIme.dll `
  -SettingsPath artifacts\release `
  -PackagePath artifacts\release\data\lexicon `
  -Version 2.0.1 `
  -SigningPolicy Off `
  -SetDefaultInputMethod `
  -HealthCheckExe build-release\release_health_check.exe
```

仅需重注册开发 DLL 时使用 `scripts/register_ime.ps1`。输入法 DLL 被宿主加载后不会自动
换新，验证新 DLL 前应关闭所有已加载旧版本的应用，或注销并重新登录。

## 发布验收

每次公开发行至少确认：

1. 正式 Release 构建和 CTest 通过；
2. `scripts/test_release_package.ps1` 验证通过；
3. Setup 产品名、版本、图标和未签名状态正确；
4. ZIP 可完整解压，安装/卸载脚本及使用说明存在；
5. 两个包均不含用户词、真实剪贴板数据或 `rime-moqi-zh.gram`；
6. 两个 `.sha256` 与最终上传文件一致；
7. 标签 `v<version>`、`src/common/version.h` 和二进制文件版本一致；
8. Release 明确说明未签名及 SmartScreen 风险。
