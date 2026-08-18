# NSIS 安装包

## 交付结构

安装包采用“NSIS 薄包装层 + `win-x64` 自包含 WPF + PowerShell 部署事务”：

- `Setup.exe` 只负责界面、提权、释放发布包、调用部署事务和注册 Windows 卸载入口；
- `scripts\install_ime.ps1` 负责清单校验、side-by-side 复制、注册、健康检查和回滚；
- `ShuruSettings.exe` 连同 .NET 8 Desktop Runtime 一起部署，目标电脑不需要另装 .NET；
- 原生 `ShuruIme.dll` 使用静态 MSVC 运行库，目标电脑不需要 VC++ Redistributable；
- 安装后由 NSIS 生成 `%ProgramFiles%\CaishenPinyin\Uninstall.exe`。

安装包仅支持 AMD64 Windows 10/11。NSIS 是 32 位进程，因此通过 `Sysnative` 启动
64 位 Windows PowerShell，保证注册 x64 TSF DLL 时使用的是 64 位 `regsvr32.exe`。

## 构建

开发机需要 NSIS 3，默认路径为 `C:\Program Files (x86)\NSIS\makensis.exe`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```

脚本依次执行完整 Release 构建和 CTest、生成 schema 2 发布清单、验证全部文件哈希、
确认 WPF 自包含运行时和 .NET 法律声明齐全、确认不含 `user_dict.txt`、
`rime-moqi-zh.gram` 或 `zh-moqi.gram`，最后生成：

- `artifacts\installer\CaishenPinyin-<version>-win-x64-Setup.exe`
- `artifacts\installer\CaishenPinyin-<version>-win-x64-Setup.exe.sha256`

只修改 NSIS 页面时可使用 `-SkipBuild`；该参数只跳过编译和 CTest，不跳过发布包校验。

## 安装行为

安装首页区分全新安装、升级和修复。版本比较使用已安装 DLL 的数值文件版本：

- 新包版本更高时升级；
- 数值版本相同时使用带唯一后缀的新不可变目录执行修复；
- 已安装版本更高时阻止降级；
- 安装失败时恢复旧 DLL 注册、版本指针、快捷方式和默认输入法状态。

“设为默认输入法”默认勾选。安装器保存安装前的
`HKCU\Control Panel\International\User Profile\InputMethodOverride`，但只有在它实际改变
默认输入法时才取得该状态的管理权。卸载时仅在当前值仍为财神拼音 TIP 时恢复原值；
如果用户安装后手动换过默认输入法，卸载器不会覆盖用户选择。

静默安装默认同样设置财神拼音为默认输入法：

```powershell
CaishenPinyin-<version>-win-x64-Setup.exe /S
```

静默安装时不修改默认输入法：

```powershell
CaishenPinyin-<version>-win-x64-Setup.exe /S /NODEFAULTIME
```

## 卸载与数据

图形卸载默认不删除个人数据，保留：

- `%LOCALAPPDATA%\CaishenPinyin` 下的设置、皮肤、剪贴板、学习数据和统计；
- `%ProgramData%\CaishenPinyin\data\lexicon` 下的系统词库版本；
- 用户自行复制到当前词库版本目录的 `rime-moqi-zh.gram`。

勾选“同时删除设置、皮肤、剪贴板记录和词库数据”后会二次确认。静默卸载默认保留
数据；显式删除数据使用：

```powershell
"%ProgramFiles%\CaishenPinyin\Uninstall.exe" /S /DELETEUSERDATA
```

卸载先恢复受安装器管理的默认输入法，再注销 TSF DLL。若 DLL 丢失或注册路径不属于
当前安装根，卸载会停止并要求先运行修复，避免留下半注销状态。占用中的程序文件由
NSIS 使用 `/REBOOTOK` 安排在重启后删除。

## 无签名限制

当前交付明确采用无签名方案，构建时使用 `SigningPolicy=Off`，并验证最终 Setup 的
Authenticode 状态为 `NotSigned`。因此无法避免以下 Windows 提示：

- UAC 显示“未知发布者”；
- SmartScreen 可能显示“Windows 已保护你的电脑”；
- 下载工具、浏览器或安全软件可能降低信誉或提高拦截概率。

SHA-256 文件只能用于校验下载完整性，不能替代发布者身份签名。未来购买正式
Authenticode 证书后，应同时签名 `ShuruIme.dll`、`ShuruSettings.exe`、`Setup.exe` 和
`Uninstall.exe`，并把发布构建切换到 `SigningPolicy=Required`。

## 发布验收

每次发布至少验证：

1. 完整 CTest 与 `tests\deployment_core_test.ps1` 通过；
2. Setup 的产品名、版本、图标、未签名状态和 SHA-256 正确；
3. 100%、125%、150%、200% DPI 下文字无截断，Tab 顺序和焦点清晰；
4. 全新安装、同版本修复、升级和降级阻止行为正确；
5. 默认输入法勾选/取消、安装失败回滚和卸载条件恢复正确；
6. “已安装的应用”存在卸载入口，默认卸载保留数据，显式删除数据可完整清理；
7. 发布包和安装目录均不含 `rime-moqi-zh.gram`。
