# 重载输入法 / 去掉「中全键」栏

## 为何还在
IME DLL **加载进每个应用进程后不会自动换新**。已经打开的记事本或浏览器会继续使用进程中加载的旧 DLL。

## 立刻隐藏（不重启）
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File E:\shurufa\scripts\hide_status_bar_now.ps1
```

## 真正重载（推荐）
1. **关掉**所有用过发财拼音的窗口（记事本、Edge、Chrome、资源管理器里的搜索框等）
2. 或直接：**注销 / 重启 Windows**（最干净）
3. 再打开记事本，确认注册路径指向 `%ProgramFiles%\FacaiPinyin\versions\0.2.6\ShuruIme.dll`
4. 不应再出现「中 | 全 | 键」悬浮条
5. 有组合串时单击 **Shift** 或按 **Enter** 上屏原始字母；无组合串时单击 **Shift** 切换中/英；**F10** 切换全拼/双拼（无界面）

## 当前版本
- DLL：0.2.6（版本化旁路安装，旧版本目录保留）
- 词库：1.1.0（系统词库独立版本化，升级不覆盖用户词库）
