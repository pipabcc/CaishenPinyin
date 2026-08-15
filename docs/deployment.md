# 独立部署、升级与回滚

管理员安装（先校验 manifest/hash，再注册和切换）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
  -Action Install `
  -DllPath artifacts\release\ShuruIme.dll `
  -SettingsPath artifacts\release `
  -PackagePath artifacts\release\data\lexicon `
  -Version 2.0.1 `
  -HealthCheckExe build-release\release_health_check.exe
```

健康检查及回滚：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action HealthCheck
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Rollback
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Cleanup
```

程序安装到 `%ProgramFiles%\CaishenPinyin\versions\<version>`，每个版本包含 `ShuruIme.dll`、`ShuruSettings.exe`、`ShuruSettings.dll`、`ShuruSettings.deps.json` 和 `ShuruSettings.runtimeconfig.json`。安装脚本在公共开始菜单创建“财神输入法设置”快捷方式，并在升级或回滚时同步到当前版本。

所有原生 C++ 目标均使用静态 MSVC 运行库（Release 为 `/MT`，Debug 为 `/MTd`）。`ShuruIme.dll` 不依赖目标机器的 `MSVCP140.dll`、`VCRUNTIME140.dll` 或 `VCRUNTIME140_1.dll`，因此输入法注入不同宿主进程时无需单独安装 VC++ Redistributable。设置程序仍是 .NET 8 WPF 应用，需要 .NET 8 Desktop Runtime。

当前版和上一版并存。应用与词库版本目录均不可变：同版本文件完全一致时直接复用，内容冲突时拒绝覆盖。重复安装当前版本不会改写上一版本指针。脚本不会终止占用 DLL 的应用，旧目录只在显式执行 `Cleanup` 时清理。日志写入安装根 `logs`。错误码：10/11 参数，20-29 包、哈希或签名，30-33 健康检查或版本冲突，40 注册，50 无可回滚版本。

正式发布入口为 `scripts\build.ps1`：Release configure/build 后强制运行完整 CTest，任一失败立即停止，并输出到显式 `-OutputDir`。发布包 `release-manifest.json` 记录 DLL/词库的 SHA-256、大小和 DLL 文件版本。当前没有代码签名证书；开发构建使用 `IfPresent`，正式发布必须传 `-SigningPolicy Required`，未签名会 fail closed，脚本不会伪造签名。

安装采用 staged → manifest/组件验证 → 版本目录原子切换 → 注册 → current 指针 → 快捷方式 → 真实健康检查。任一阶段失败会恢复旧 DLL 注册、current 指针和快捷方式；以下当前用户数据均不进入发布包，也不会被升级覆盖：

- `%LOCALAPPDATA%\CaishenPinyin\settings.ini`
- `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`
- `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\custom_phrases.txt`
- `%LOCALAPPDATA%\CaishenPinyin\data\typing_stats.txt`
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\config.json`
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\history.db`（SQLite 主库；运行时可能短暂出现 `-wal`、`-shm`）
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\history.json.migrated.bak`（仅旧版 JSON 首次成功迁移后保留）
- `%LOCALAPPDATA%\CaishenPinyin\clipboard\images\`

剪贴板历史首次打开时会在跨进程迁移锁内从旧版 `history.json` 导入 SQLite，成功后把原文件改名为备份；旧 JSON 损坏时原文件保持不变，空数据库仍可继续记录新内容。历史查询使用 FTS5 trigram 索引，图片仍独立保存在 `images` 目录。设置程序的隐藏监听进程由输入法或设置页按需启动，同一登录会话只保留一个实例。

注册/profile 健康检查需要管理员环境；本机交互验证可运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_local_tsf_e2e.ps1 -BuildDir build-release -RequireRegisteredProfile
```

未注册或非交互会明确以退出码 77 跳过。安装后可从开始菜单或候选框右键打开设置；悬浮状态栏保持隐藏。

非管理员核心测试使用显式临时根且跳过 COM 注册：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\deployment_core_test.ps1
```
