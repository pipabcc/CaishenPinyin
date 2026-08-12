# 独立部署、升级与回滚

管理员安装（先校验 manifest/hash，再注册和切换）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Install -DllPath E:\build\ShuruIme.dll -Version 0.2.0
```

健康检查及回滚：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action HealthCheck
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Rollback
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 -Action Cleanup
```

DLL 安装到 `%ProgramFiles%\FacaiPinyin\versions\<version>`，当前版和上一版并存。同版本词库已安装且清单完全一致时直接复用，不删除正在使用的词库目录；同版本清单冲突则拒绝安装。脚本不会终止占用 DLL 的应用，旧目录只在显式执行 `Cleanup` 时清理。日志写入安装根 `logs`。错误码：10/11 参数，20-29 包、哈希或签名，30-33 健康检查或版本冲突，40 注册，50 无可回滚版本。

正式发布入口为 `scripts\build.ps1`：Release configure/build 后强制运行完整 CTest，任一失败立即停止，并输出到显式 `-OutputDir`。发布包 `release-manifest.json` 记录 DLL/词库的 SHA-256、大小和 DLL 文件版本。当前没有代码签名证书；开发构建使用 `IfPresent`，正式发布必须传 `-SigningPolicy Required`，未签名会 fail closed，脚本不会伪造签名。

安装采用 staged → manifest/DLL 验证 → 版本目录原子切换 → 注册 → current 指针 → 真实健康检查。任一阶段失败会恢复旧 DLL 注册和 current 指针；`user_dict.txt` 不进入发布包也不会被升级覆盖。注册/profile 健康检查需要管理员环境；本机交互验证可运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run_local_tsf_e2e.ps1 -BuildDir build-release -RequireRegisteredProfile
```

未注册或非交互会明确以退出码 77 跳过；本批次未执行最终注册/部署。

非管理员核心测试使用显式临时根且跳过 COM 注册：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\deployment_core_test.ps1
```
