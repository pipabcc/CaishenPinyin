# 发财拼音（Facai Pinyin）

Windows 11 本地拼音输入法，包含 TSF 文本服务、拼音引擎、候选窗口、系统词库和独立设置程序。

## 当前状态

| 模块 | 状态 |
|---|---|
| TSF 输入法 DLL 源码 | 已就绪（需 MSVC 编译） |
| 拼音引擎 / 词库 | 已就绪（约 20 万基础中文词 + 完整单字库） |
| 候选窗 | 已就绪（Win32 无焦点窗） |
| 设置程序 (WPF) | 常规、全拼/双拼、自定义短语、词库与隐私设置已连接运行时 |
| C# 引擎演练 | 可运行 |
| 本机 C++ 工具链 | **已安装到 E:\\shurufa\\tools，可编译** |
| 发布版本 | **0.4.1**（现代化设置、自定义短语、开始菜单与状态栏设置入口） |

## 架构

```
按键 → TSF TextService → PinyinEngine → Dictionary
                 ↓
           CandidateWindow
                 ↓
              上屏汉字
```

- `src/ime`：TSF / COM / 候选窗
- `src/engine`：拼音查询与用户词学习
- `data/lexicon`：词库
- `settings`：设置 UI
- `scripts`：构建 / 注册 / 环境检查

## 快捷键（MVP）

- 字母：进入拼音组合
- `Space`：上屏首选
- `1-9`：选词
- `Esc`：清空
- `Enter`：上屏原始拼音
- 组合输入时单独按 `Shift`：上屏原始拼音；无组合时单独按 `Shift`：切换中英文
- `Ctrl+Space`：中英切换
- `←/→`：切换候选
- `F9`：开关软键盘
- 组合输入时按 `F10`：切换全拼/双拼

## 开发环境要求

1. Visual Studio 2022 Build Tools（MSVC v143）
2. Windows 10/11 SDK（含 `msctf.h`）
3. CMake >= 3.20
4. .NET 8 SDK（设置程序 / C# 演练）

检查环境：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

## 构建

唯一正式构建入口是 `scripts\build.ps1`。它会配置并编译、运行完整 CTest，并在未指定 `-NoPackage` 时生成发布包；不要直接调用 CMake 或历史批次脚本。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

默认构建目录为 `build-release`，发布包目录为 `artifacts\release`。可用 `-BuildDir`、`-OutputDir`、`-SigningPolicy Off|IfPresent|Required` 和 `-NoPackage` 显式调整；正式发布应使用 `-SigningPolicy Required`。

## 安装与注册

先完成正式构建，再在管理员 PowerShell 中安装发布包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_ime.ps1 `
  -Action Install `
  -DllPath artifacts\release\ShuruIme.dll `
  -SettingsPath artifacts\release `
  -PackagePath artifacts\release\data\lexicon `
  -Version 0.4.1 `
  -HealthCheckExe build-release\release_health_check.exe
```

安装后可从开始菜单打开“发财拼音设置”，也可左键状态栏的“设”，或右键状态栏选择“输入法设置”。

卸载：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\unregister_ime.ps1
```

## 无 C++ 工具链时的验证

```powershell
dotnet run --project tests\engine_playground_cs
dotnet build settings\ShuruSettings.csproj -c Release
```

在演练程序输入 `nihao`、`shurufa`、`women` 可验证词库排序。

## 词库格式

`data/lexicon/base_dict.txt`：

```text
pinyin<TAB>词<TAB>词频
nihao	你好	9000
```

## 版本化词库与部署

词库 manifest 生成/校验、独立版本目录、side-by-side DLL 安装、健康检查和回滚见：

- `docs/lexicon-governance.md`
- `docs/deployment.md`
- `docs/privacy-input-policy.md`

`user_dict.txt` 和 `custom_phrases.txt` 始终保存在 `%LOCALAPPDATA%`，升级流程不会覆盖。

## 设置与生效

设置保存在 `%LOCALAPPDATA%\FacaiPinyin\settings.ini`，采用同目录临时文件 + 原子替换，无需管理员权限。设置程序可控制全拼/双拼、学习、内容日志（默认关闭）、模糊拼音、全/半角标点、候选数量和字体，并可管理系统词库、自动学习词及自定义短语。自定义短语保存在 `%LOCALAPPDATA%\FacaiPinyin\data\lexicon\custom_phrases.txt`，格式为 `输入码<TAB>短语<TAB>候选位置`。保存后切换到其他输入法再切回即可生效。

## 下一步（Phase 1）

1. 安装 MSVC 后完成真机注册与记事本出字
2. 扩大词库与简拼
3. 用户词持久化
4. 组合串显示属性 / 更精确光标跟随
5. 应用兼容性矩阵
