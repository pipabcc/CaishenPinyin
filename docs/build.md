# 构建说明（E 盘工具链）

## 原则

- 工程与工具统一放在 `E:\shurufa`
- 不向 C/D 主动安装大型开发套件
- 每次构建前激活：`. tools\env.ps1`

## 已安装工具位置

| 工具 | 路径 |
|---|---|
| VS 2022 Build Tools / MSVC | `E:\shurufa\tools\vs2022` |
| Windows SDK 10.0.26100 | `E:\shurufa\tools\winsdk\Windows Kits\10` |
| CMake 3.31.6 | `E:\shurufa\tools\cmake\cmake-3.31.6-windows-x86_64` |
| Ninja 1.12.1 | `E:\shurufa\tools\ninja` |
| 安装缓存 | `E:\shurufa\tools\cache` |
| 临时目录 | `E:\shurufa\tools\tmp` |

说明：本机原有 `D:\windowskits` 已复制到 E；注册表 `KitsRoot10` 指向 E。`C:\Program Files (x86)\Windows Kits` 与 VS Packages 已做目录联接导向 E。

## 环境检查

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

## 编译与测试 IME

唯一正式入口为 `scripts\build.ps1`；脚本会激活 `tools\env.ps1`，先构建 WPF 设置程序，再完成 CMake 配置、编译和完整 CTest。开发者无需单独调用 CMake 或 CTest。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

默认输出：

- `build-release\ShuruIme.dll`
- `build-release\engine_playground.exe`
- `build-release\data\lexicon\*.txt`
- `artifacts\release\ShuruIme.dll`
- `artifacts\release\ShuruSettings.exe`、`ShuruSettings.dll`、`.deps.json`、`.runtimeconfig.json`
- `artifacts\release\data\lexicon\*` 与 `release-manifest.json`

可选参数：`-BuildDir`、`-OutputDir`、`-SigningPolicy Off|IfPresent|Required`、`-NoPackage`。正式发布必须使用 `-SigningPolicy Required`；只需本地编译测试时可使用 `-NoPackage`。

中文 Windows 下，CMakeLists 已显式配置 Ninja 识别 MSVC 的 UTF-8 中文
`/showIncludes` 前缀，避免增量构建遗漏头文件依赖。

## 编译设置

```powershell
dotnet build settings\ShuruSettings.csproj -c Release
```

## 安装与注册

管理员：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
  -Action Install `
  -DllPath artifacts\release\ShuruIme.dll `
  -SettingsPath artifacts\release `
  -PackagePath artifacts\release\data\lexicon `
  -Version 0.4.5-lexeme-r2-20260814 `
  -HealthCheckExe build-release\release_health_check.exe
```

该入口会校验发布清单、注册 DLL、部署设置程序并创建公共开始菜单快捷方式。开发阶段只需重新注册单个 DLL 时，才使用 `scripts\register_ime.ps1`。

## 日志

默认写到 `%TEMP%\ShuruIme.log`（激活 env 后为 `E:\shurufa\tools\tmp`）。

## P1 性能与兼容性测试

`p1_engine` 报告文本/缓存加载耗时、缓存大小以及查询 P50/P95/P99。阈值采用相对加载时间和宽松 P99 上限：

性能、设置、自定义短语、部署、隐藏状态栏、字数统计及 TSF 编辑会话测试均包含在正式入口执行的完整 CTest 中。`tsf_e2e_core` 使用 `TF_TMAE_NOACTIVATETIP` 隔离机器上已安装的输入法版本。需要排查单项时，可在一次正式构建后运行：

```powershell
ctest --test-dir build-release -C Release -R p1_engine -V
```

首次文本加载会原子生成带 magic/version、源文件 SHA-256 与负载 SHA-256 的 `.bin` 派生缓存；校验失败时安全回退文本并重建。
