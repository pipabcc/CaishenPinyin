# 财神输入法（Caishen IME）

一款面向 Windows 11 / 10 的现代原生本地拼音输入法。基于 Windows TSF（Text Services Framework）框架开发，核心采用现代 C++ 构建，配备独立自包含的 WPF 设置中心与本地化拼音纠错、双数组词库索引引擎。

---

## 核心特性

* **原生系统级集成**：基于微软 TSF 文本服务框架，完美兼容 Windows 11/10 桌面与 UWP 应用，无焦点 DirectWrite 高清候选窗。
* **纯净离线与隐私保护**：全本地运行，无网络上传模块，不收集用户击键正文，日志默认关闭。
* **海量高精词库**：内置白霜拼音精选词库（约 67.7 万多字词 + 8,247 条单字读音）与常用英文词库。
* **分级回退语言模型**：支持多级统计语言模型（N-gram），长句精准打分，缺省状态下平滑回退，兼顾体积与智能排序。
* **丰富个性化配置**：
  * 支持全拼与多种主流双拼方案（自然码、微软双拼、小鹤双拼等）及一键快捷键切换
  * 支持用户词自学习、自定义短语（`custom_phrases.txt`）与模糊音配置
  * 皮肤管理与 DirectWrite 候选字体微调
  * 剪贴板历史与图片输入交互（`v` 模式）
* **现代化图形安装包**：提供 NSIS 单文件打包，支持自包含运行与干净彻底的卸载流程。

---

## 核心性能与资源占用对比

下表为在当前 Windows 11 实机环境中，采集的真实系统进程指标与微秒级高精度基准测试（Benchmark，MSVC `/O2` 优化构建）实测数据对比：

| 评估维度 / 指标 | 本项目输入法（财神输入法） | Windows 系统内置微软输入法 | 性能对比与分析 |
| :--- | :--- | :--- | :--- |
| **工作架构模式** | **In-Process 进程内 TSF COM 模型**<br>直接注入宿主进程，零 IPC 跨进程开销 | **Out-of-Process 跨进程现代 App 架构**<br>依赖 TSF CoreMessaging 与跨进程 IPC 管道 | 财神输入法无跨进程调度延迟，消息响应零管道阻塞 |
| **常驻进程总内存（WorkingSet）** | **约 4.1 MB ~ 35 MB**（单宿主进程内）<br>词库与模型通过只读 mmap 跨进程共享 | **约 295.85 MB**（三进程总和）<br>• `TextInputHost.exe`: 228.37 MB<br>• `ctfmon.exe`: 58.91 MB<br>• `ChsIME.exe`: 8.57 MB | 微软输入法常驻内存约为财神输入法的 8 ~ 10 倍 |
| **专用私有内存（Private Bytes）** | **约 0.88 MB ~ 25 MB**（随打字动态分配） | **约 216.85 MB**<br>• `TextInputHost.exe`: 189.38 MB<br>• `ctfmon.exe`: 26.01 MB<br>• `ChsIME.exe`: 1.46 MB | 财神输入法私有内存极低，对系统物理 RAM 压力极小 |
| **单键首字母查询延迟（P50）** | **1.21 ms** (1,209.70 μs) | 约 8 ~ 15 ms (含跨进程 IPC 调度) | 财神输入法检索快 7 ~ 10 倍 |
| **常用二字词检索延迟（P50）** | **0.99 ms** (995.40 μs) | 约 10 ~ 20 ms | 常用词检索低于 1 毫秒，极速响应 |
| **四字成语/专名检索（P50）** | **32.58 ms** (32,582.20 μs) | 约 25 ~ 45 ms | 本地全候选打分与语言模型排序 |
| **长句全量智能切分（P50）** | **180.15 ms** (全量 N-Gram Viterbi 求解) | 约 80 ~ 250 ms (本地模型+云端混合) | 本地纯统计模型解码，长句切分精度高 |
| **连续按键流模拟耗时（7键连续）** | **10.07 ms**（平均每键 1.43 ms） | 约 50 ~ 120 ms（累积 UI 刷新耗时） | 高频打字无丢键、无跳帧、无手感延迟 |
| **单字/短词吞吐量（QPS）** | **802 ~ 938 QPS**（单线程峰值） | 约 60 ~ 150 QPS（受 IPC 队列限制） | 吞吐能力高出 6 ~ 10 倍 |
| **用户词动态学习与匹配（P50）** | **0.44 ms** (440.40 μs，吞吐 2,120 QPS) | 约 5 ~ 15 ms | 本地哈希词频提升，微秒级即时生效 |
| **UI 渲染管线** | **Win32 DirectWrite 硬件加速轻量无焦点浮窗**<br>微秒级 Direct2D/DirectWrite 绘制，零 XAML 树开销 | **UWP / XAML / DirectComposition 跨进程渲染**<br>庞大的 UI 视觉树，冷启动易闪烁 | 财神输入法候选框显示/隐藏耗时极低 |
| **核心二进制体积** | **1.21 MB** (`ShuruIme.dll`) | 约 25 MB+（含 CBS、ChsIME、InputMethod） | 核心动态库体积极小，便于分发与极速加载 |
| **系统词库与模型体积** | **约 27 MB** (`base_dict` + `ngram.bin` + `prior.bin`) | 约 50 MB ~ 150 MB（含系统字典、拼音组件） | 白霜 67.7 万词库 + 紧凑二进制模型 |
| **网络与云端 IO 开销** | **0 KB/s**（绝对零网络请求，纯本地） | 动态网络请求（云联想、动态词库同步、遥测） | 财神输入法彻底杜绝击键隐私泄漏与弱网卡顿 |

### 数据深度解析与架构优势

1. **单字与短词极限响应（< 1.2 ms）**：
   用户日常打字中 80% 以上的操作为单字或 2~3 字词组。本项目输入法对这些核心输入场景的中位数延迟（P50）仅为 0.99 ms ~ 1.21 ms，单核吞吐量逼近 1,000 QPS，达到“按键即见候选”的人眼零感知延迟（人类神经反应通常在 100ms 级别，1ms 仅为其百分之一）。
2. **连续键入流平滑度**：
   在逐键递增键入（如 `r` -> `re` -> `ren` -> `renz` -> `renzh` -> `renzhe` -> `renzhen`）的完整击键生命周期中，7 次连续查询并刷新候选的总耗时仅 10.07 ms，平均每个按键触发耗时仅 1.43 ms，彻底消除击键卡顿与丢键。
3. **用户词动态学习与召回极速**：
   动态更新用户词频并在后续输入中置顶仅需 0.44 ms，QPS 超过 2,100，保证高频词越用越顺手且不卡顿。
4. **隐私安全与网络零开销**：
   100% 离线运行，代码中完全不包含网络套接字代码（Winsock），用户词库与打字历史只安全保存在本地机器的 `%LOCALAPPDATA%` 中，具备绝对的数据安全性与确定性的响应时间。
5. **UI 渲染开销与零视觉延迟**：
   采用原生的 Win32 Layered Window + DirectWrite 硬件加速渲染（位于 [`candidate_window.cpp`](src/ime/ui/candidate_window.cpp) 和 [`directwrite_text_renderer.cpp`](src/ime/ui/directwrite_text_renderer.cpp)）。候选词绘制耗时仅在 0.1 ms ~ 0.3 ms 级别，候选框紧跟光标，无动画拖沓与渲染延迟。
6. **高效内存机制（mmap 物理页跨进程共享）**：
   核心逻辑基于精炼的 C++ 实现，`ShuruIme.dll` 仅 1.21 MB。系统词库和语言模型（`base.bin`、`system_ngram.bin`、`system_lexeme_prior.bin`）采用 Windows 内存映射文件（Memory Mapped File / mmap）技术。当在 10 个不同的软件（如浏览器、记事本、Word、微信等）同时打字时，所有宿主进程共享同一份词库物理内存页，不会随进程增多而线性消耗系统 RAM。
7. **进程模型与零 IPC 通信成本**：
   采用经典的高性能 TSF 进程内（In-Process）COM 动态库架构。击键事件直接在前台线程就地处理，检索本地内存词库并直接调用 DirectWrite 渲染，无进程上下文切换（Context Switch），无 IPC 序列化/反序列化开销。

---

## 项目架构

```
按键输入 → TSF 文本服务 (TextService) → 拼音引擎 (PinyinEngine) → 词典核心 (Dictionary)
                        ↓
                 候选窗口 (CandidateWindow)
                        ↓
                     文字上屏
```

* `src/ime`：TSF 接口实现、COM 组件生命周期与 Win32/DirectWrite 候选窗
* `src/engine`：拼音切分、模糊音纠错、双数组 Trie 检索与用户词频学习
* `data/lexicon`：系统基础词库、单字先验与英文词库定义
* `settings`：基于 .NET 8 WPF 开发的独立设置管理程序
* `installer`：NSIS 图形安装与卸载脚本
* `scripts`：构建、注册、测试与环境检查脚本

---

## 常用快捷键

| 快捷键 | 功能说明 |
| :--- | :--- |
| `字母 a-z` | 进入拼音输入状态 |
| `Space`（空格） | 上屏当前首选候选词 |
| `1` - `9` | 数字键快速选词上屏 |
| `Esc` | 清空当前拼音并退出输入状态 |
| `Enter` | 直接上屏当前输入的原始拼音字母 |
| `Shift`（单按） | 拼音输入状态下单按上屏原始拼音；未输入时单按切换中/英文模式 |
| `Ctrl + Space` | 切换中/英文模式 |
| `←` / `→` | 移动拼音光标或翻页查看候选 |
| `F9` | 打开 / 关闭虚拟软键盘 |
| `F10` | 拼音输入状态下快速切换全拼 / 双拼模式 |

---

## 开发与编译环境要求

1. **操作系统**：Windows 10 / 11（x64）
2. **C++ 编译工具**：Visual Studio 2022（需安装 C++ 桌面开发组件，含 MSVC v143 及 Windows 10/11 SDK，包含 `msctf.h`）
3. **CMake**：>= 3.20
4. **.NET SDK**：.NET 8.0 SDK（编译 WPF 设置中心与 C# 演练工具）
5. **NSIS**：NSIS 3.x（仅在生成单文件图形安装包时需要）

检查本地开发环境：
```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

---

## 快速构建

正式编译统一使用根目录的构建脚本 `scripts\build.ps1`，脚本会自动配置 CMake、执行编译并运行完整的 CTest 自动化测试套件：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

* 默认构建目录为 `build-release/`
* 发布产物输出至 `artifacts/release/`（设置中心采用 `win-x64` 自包含方式发布，目标机无需单独安装 .NET Desktop Runtime）

生成单文件图形安装包：
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```
安装包将输出至 `artifacts\installer\CaishenPinyin-<版本号>-win-x64-Setup.exe`。

---

## 本地安装与调试

在管理员权限 PowerShell 中直接部署并注册发布包：

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

注销已注册的输入法 DLL：
```powershell
powershell -ExecutionPolicy Bypass -File scripts\unregister_ime.ps1
```

---

## 词库与数据存储说明

1. **用户配置文件与个人词库**：
   * 设置文件：`%LOCALAPPDATA%\CaishenPinyin\settings.ini`
   * 用户词库：`%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`
   * 自定义短语：`%LOCALAPPDATA%\CaishenPinyin\data\lexicon\custom_phrases.txt`
   * 字数统计：`%LOCALAPPDATA%\CaishenPinyin\data\typing_stats.txt`（仅保存按键计数与日期，不记录任何文字内容）
   * *注：升级或重新安装输入法时，用户个人数据默认保留，不会被覆盖。*

2. **可选统计语言模型**：
   * `rime-moqi-zh.gram` 属于可选的高阶长句统计语言模型（基于 N-gram），因体积较大，未随源码或安装包直接分发。
   * 用户可自行下载（下载地址：[rime-build-grammar 1.0.0 Releases](https://github.com/gaboolic/rime-build-grammar/releases/tag/1.0.0)）并置于当前词库目录；当未放置该模型时，输入法将自动使用内置的轻量化 `system_ngram.bin`。

---

## 参与贡献与社区

* 详细贡献规范与开发准则请阅读 [贡献指南 (CONTRIBUTING.md)](CONTRIBUTING.md)。
* 版本发布历史与详细变更记录请查看 [更新日志 (CHANGELOG.md)](CHANGELOG.md)。
* 安全漏洞披露与隐私政策请查阅 [安全政策 (SECURITY.md)](SECURITY.md)。
* 第三方组件许可与数据来源说明请查看 [第三方数据与许可 (THIRD_PARTY_NOTICES.md)](THIRD_PARTY_NOTICES.md)。

---

## 友情链接

* LinuxDo — [https://linux.do](https://linux.do/)（真诚、友善、团结、专业，共建你我引以为荣之社区）

---

## 开源许可证

本项目核心源码采用 [GNU General Public License v3.0 (GPL-3.0)](LICENSE) 许可证开源。所引用的第三方库及词库授权遵循各自的开源协议，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
