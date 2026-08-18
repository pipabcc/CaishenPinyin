# 独立部署、升级与回滚

管理员安装（先校验 manifest/hash，再注册和切换）：

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

健康检查及回滚：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action HealthCheck
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Rollback
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Cleanup
```

程序默认安装到 `%ProgramFiles%\CaishenPinyin\versions\<version>`；NSIS 全新安装也可选择其他本机专用目录。每个版本包含 `ShuruIme.dll`、`ShuruSettings.exe`、完整的 `win-x64` 自包含 .NET 8 Desktop Runtime、法律声明和发布包内的 `data\skins` 内置皮肤资源。安装脚本把文件哈希写入组件清单并在健康检查时验证，同时在公共开始菜单创建“财神输入法设置”快捷方式，并在升级或回滚时同步到当前版本。

所有原生 C++ 目标均使用静态 MSVC 运行库（Release 为 `/MT`，Debug 为 `/MTd`）。`ShuruIme.dll` 不依赖目标机器的 `MSVCP140.dll`、`VCRUNTIME140.dll` 或 `VCRUNTIME140_1.dll`；设置程序把 .NET 8 Desktop Runtime 一同发布，因此目标电脑无需另装 VC++ Redistributable 或 .NET Desktop Runtime。

当前版和上一版并存。应用版本目录不可变，同版本文件完全一致时直接复用，内容冲突时拒绝覆盖；NSIS 的同版本修复会生成带唯一后缀的新版本目录，从而旁路损坏或被占用的旧文件。词库物理目录使用 `<逻辑版本>-<manifest 哈希前缀>`，升级到新词库时会迁移用户自行安装的 `rime-moqi-zh.gram`，但它从不进入安装包。脚本不会终止占用 DLL 的应用，旧目录只在显式执行 `Cleanup` 时清理。日志写入安装根 `logs`。错误码：10/11 参数，20-29 包、哈希或签名，30-33 健康检查或版本冲突，40-44 注册、默认输入法或安装状态，50 无可回滚版本。

正式发布包入口为 `scripts\build.ps1`：Release configure/build 后强制运行完整 CTest，任一失败立即停止，并输出到显式 `-OutputDir`。发布包 `release-manifest.json` 记录全部文件的组件类别、SHA-256、大小和 DLL 文件版本。当前采用无签名分发，`scripts\build_installer.ps1` 固定使用 `SigningPolicy=Off` 并验证最终 Setup 确实未签名；Windows 显示“未知发布者”或 SmartScreen 提示是该方案无法消除的系统行为。未来取得 Authenticode 证书后应改为 `Required`。

安装采用 staged → manifest/组件验证 → 版本目录原子切换 → 注册 → 可选默认输入法 → current 指针 → 快捷方式 → 真实健康检查。安装器只在自己实际改变默认输入法时记录原值；失败会恢复安装前值，卸载时也仅在当前默认值仍为财神输入法时恢复，避免覆盖用户之后的手动修改。任一阶段失败会恢复旧 DLL 注册、current 指针和快捷方式；以下当前用户数据均不进入发布包，也不会被升级覆盖：

- `%LOCALAPPDATA%\CaishenPinyin\settings.ini`
- `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`
- `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\custom_phrases.txt`
- `%LOCALAPPDATA%\CaishenPinyin\data\typing_stats.txt`
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\config.json`
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\history.db`（SQLite 主库；运行时可能短暂出现 `-wal`、`-shm`）
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\history.json.migrated.bak`（仅旧版 JSON 首次成功迁移后保留）
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\images\`

剪贴板历史首次打开时会在跨进程迁移锁内从旧版 `history.json` 导入 SQLite，成功后把原文件改名为备份；旧 JSON 损坏时原文件保持不变，空数据库仍可继续记录新内容。历史查询使用 FTS5 trigram 索引，图片仍独立保存在 `images` 目录。设置程序的隐藏监听进程由输入法或设置页按需启动，同一登录会话只保留一个实例。

正式安装和卸载使用 `Setup.exe` / `%ProgramFiles%\CaishenPinyin\Uninstall.exe`。卸载默认保留
`%LOCALAPPDATA%\CaishenPinyin` 与 `%ProgramData%\CaishenPinyin\data\lexicon`；只有用户
明确勾选删除个人数据时才移除，包括可选 Grammar。详细命令行参数和发布验收见
`docs/installer.md`。

注册/profile 健康检查需要管理员环境；本机交互验证可运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_local_tsf_e2e.ps1 -BuildDir build-release -RequireRegisteredProfile
```

未注册或非交互会明确以退出码 77 跳过。安装后可从开始菜单或候选框右键打开设置；悬浮状态栏保持隐藏。

非管理员核心测试使用显式临时根且跳过 COM 注册：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\deployment_core_test.ps1
```
