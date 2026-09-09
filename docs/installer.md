# 安装、部署与重载

当前支持 Windows 10/11 x64。正式包同时包含 x64 与 WOW64 x86 输入法 DLL，
设置中心携带 .NET 8 桌面运行时，原生组件使用静态 CRT。

## 两种交付方式

| 方式 | 安装与卸载 |
|---|---|
| Setup | NSIS 负责向导、提权、释放文件和卸载入口；部署事务由 `scripts/install_ime.ps1` 执行 |
| Portable | 在解压目录注册 DLL，通过包内脚本安装或注销；需要保留该目录，不创建标准应用卸载入口 |

Setup 默认使用 `%ProgramFiles%\CaishenPinyin`。全新安装可以选择本机专用目录，
升级和修复使用已有安装根。安装器拒绝磁盘根、系统目录、用户配置根以及已有无关文件的目录。

Portable 同样需要管理员权限注册 TSF 组件。两种方式都不将设置中心加入开机启动；
设置和输入法按需启动剪贴板后台进程，修复或升级会清理旧版遗留的 `CaishenSettings` 启动项。

构建与打包命令见[构建说明](build.md)。公开下载及哈希校验见 [README](../README.md#下载与安装)。

## 版本、目录与事务

Setup 程序结构：

```text
CaishenPinyin/
  current
  previous
  versions/<应用版本或修复标识>/
    ShuruIme.dll
    ShuruIme32.dll
    ShuruSettings.exe
    运行时、内置资源和组件清单
  logs/
  Uninstall.exe
```

系统词库位于 `%ProgramData%\CaishenPinyin\data\lexicon\`，
物理目录为 `versions/<词库逻辑版本>-<manifest 哈希前缀>`，由独立的 `current` 指针选择。
应用版本、修复目录名和词库逻辑版本是不同概念。

部署顺序为暂存文件、验证组件及清单、移动到新版本目录、注册双架构 DLL、
可选设置默认输入法、切换版本指针、更新快捷方式和健康检查。
失败时恢复旧注册、指针、快捷方式及本次改变的默认输入法状态。

版本目录不可变：同名目录只有内容完全一致时才能复用。
NSIS 遇到相同数值版本时创建带唯一后缀的修复目录；
更高版本执行升级，更低版本被阻止。直接调用部署脚本时由调用方提供版本目录标识。

两种位数共用同一 CLSID/Profile。NSIS 通过 Sysnative 调用 64 位 PowerShell，
部署脚本分别使用 System32 和 SysWOW64 的 `regsvr32.exe` 注册，
使不同位数的宿主选择正确 DLL。

## 默认输入法

Setup 默认勾选“设为默认输入法”。安装器只有实际改变
`HKCU\Control Panel\International\User Profile\InputMethodOverride` 时才管理原值；
卸载时也只在当前值仍为财神输入法时恢复，避免覆盖用户后来手动选择的默认项。

```powershell
# 示例使用当前公开发行文件；替换为实际下载路径。
& .\CaishenPinyin-2.0.2-win-x64-Setup.exe /S
& .\CaishenPinyin-2.0.2-win-x64-Setup.exe /S /NODEFAULTIME
```

上面两条是可选的不同安装方式，不需要依次执行。
直接调用 `install_ime.ps1` 只有传入 `-SetDefaultInputMethod` 时才申请修改默认输入法。

## 开发部署

先完成正式构建。在管理员 PowerShell 中从仓库根执行；
如使用自定义安装根，修改 `$installDirectory`，后续命令保持一致。

```powershell
$releaseDirectory = (Resolve-Path 'artifacts\release').Path
$installDirectory = Join-Path $env:ProgramFiles 'CaishenPinyin'
$releaseManifest = Get-Content (Join-Path $releaseDirectory 'release-manifest.json') -Raw | ConvertFrom-Json
$repairVersion = [string]$releaseManifest.version + '-repair-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$healthCheck = 'build-release\release_health_check.exe'
if (Test-Path 'build-release\Release\release_health_check.exe') {
    $healthCheck = 'build-release\Release\release_health_check.exe'
}
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
    -Action Install `
    -DllPath (Join-Path $releaseDirectory 'ShuruIme.dll') `
    -X86DllPath (Join-Path $releaseDirectory 'ShuruIme32.dll') `
    -SettingsPath $releaseDirectory `
    -PackagePath (Join-Path $releaseDirectory 'data\lexicon') `
    -Version $repairVersion `
    -InstallRoot $installDirectory `
    -SigningPolicy Off `
    -HealthCheckExe $healthCheck
```

唯一修复标识避免覆盖已存在或已被宿主加载的版本。安装日志位于安装根的 `logs/`，
进度写入 `deploy-progress.json`。脚本不会终止持有旧 DLL 的宿主。

直接注册仅供开发验证，不代替版本化部署事务：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\register_ime.ps1 `
    -DllPath artifacts\release\ShuruIme.dll `
    -X86DllPath artifacts\release\ShuruIme32.dll
```

## 健康检查与回滚

健康检查核对当前指针、组件文件、词库、快捷方式和注册；
指定 `-HealthCheckExe` 可再执行原生 DLL/COM/词库健康测试。

```powershell
$installDirectory = Join-Path $env:ProgramFiles 'CaishenPinyin'
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action HealthCheck -InstallRoot $installDirectory
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Rollback -InstallRoot $installDirectory
```

回滚使用 `previous` 指针恢复上一程序及词库版本，不撤销用户此后新增的个人数据。
只有需要回滚时才运行第二条。清理旧版本是单独的显式操作：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Cleanup
```

自定义安装根同样传 `-InstallRoot`。当前版和上一版被保留；
占用中的旧文件不能通过清理强制替换。部署核心回归使用临时根和 `-NoRegister`，
不会注册到真实系统。

## 升级后的重载与排查

输入法 DLL 驻留在每个已使用它的应用进程中。文件和注册更新后，
已打开的程序仍可能执行旧代码：

1. 完全退出目标应用，包括托盘后台，再重新打开。
2. 涉及资源管理器、开始菜单或多个长期运行宿主时，保存工作后注销并重新登录。
3. 核对实际注册路径与安装根的 `current`，不要固定检查某个旧版本目录。
4. 原生复制记录删除、候选窗及按键问题需要宿主加载新 DLL；
   独立窗口也应使用与之配套的设置程序。

在 64 位 PowerShell 中可查看两个注册视图：

```powershell
reg query "HKCR\CLSID\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\InprocServer32" /ve /reg:64
reg query "HKCR\CLSID\{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}\InprocServer32" /ve /reg:32
```

当前代码隐藏旧“中 / 全 / 键 / 设”悬浮状态栏。隐藏旧窗口的临时脚本只能改变显示，
不能完成模块升级。候选窗右键打开设置，F9 切换软键盘，F10 切换全拼与小鹤双拼。

本机 TSF 验证可使用 `scripts/run_local_tsf_e2e.ps1`。
`-BuildDir` 应指向实际包含测试 EXE 的目录；Visual Studio Generator 通常需要
`build-release\Release`。非交互环境，或要求已注册 Profile 但没有注册时，返回 77 表示跳过。

## 个人数据、权限与快照

升级保留 `%LOCALAPPDATA%\CaishenPinyin\` 下的设置、皮肤、用户词、搭配、短语、
置顶、统计、复制记录数据库及图片。数据库迁移和写入协议见[架构说明](architecture.md)。

安装器收敛个人文件权限、跳过重解析点，并恢复公共词库和资源的沙箱只读访问。
个人数据根取当前进程的 LOCALAPPDATA，不使用 NSIS 的全用户 Shell 目录代替。
仅修复现有权限时使用 `-Action RepairPermissions`，自定义安装根和数据根应显式传入。

正式构建包含 `engine_snapshot_build_tool.exe`。安装成功后，
脚本在新目录存在该工具时尝试隐藏启动快照预生成；工具缺失、启动或生成失败均不影响
安装结果，首次运行仍可回退装载。快照写入执行安装的用户目录，不能承诺其他用户也已预热。

完整墨奇模型不随发行包提供。已有用户自行安装的模型可在词库升级时迁移保留。
因此“包内不含模型”不等于“已有安装目录一定没有模型”。

## 卸载

Setup 通过 Windows“已安装的应用”或安装根的 `Uninstall.exe` 卸载。
默认保留个人数据、系统词库和用户自行安装的模型；
勾选删除数据会再次确认。静默删除数据必须显式传入 `/DELETEUSERDATA`。

Portable 使用原目录的卸载脚本，不能用删除文件代替注销。
卸载时若 DLL 丢失或注册路径不属于当前安装根，事务会停止；
应先修复安装，避免半注销状态。占用中的文件由 NSIS 安排重启后删除。

## 发行验收

当前采用无签名发行，SHA-256 只核对完整性，不替代 Authenticode。
公开发行前应验证新安装、升级、同版本修复、降级阻止、失败回滚、
默认输入法条件恢复、DPI/焦点、卸载数据选择及 Portable 脚本。
发行文件名、校验文件和法律声明按[构建说明](build.md#版本模型与发布验收)核对。
