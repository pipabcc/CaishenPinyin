# 贡献指南

感谢你对财神输入法（Caishen IME）项目的关注与支持！无论是提交缺陷报告、改进文档、提出新想法还是贡献代码，我们都非常欢迎。

为了让协作更加高效顺畅，请在参与贡献前阅读以下指引。

---

## 目录

- [一、 行为准则](#一-行为准则)
- [二、 如何参与贡献](#二-如何参与贡献)
  - [1. 报告缺陷（Bug Report）](#1-报告缺陷bug-report)
  - [2. 提出功能建议（Feature Request）](#2-提出功能建议feature-request)
  - [3. 改进文档](#3-改进文档)
  - [4. 贡献代码](#4-贡献代码)
- [三、 本地开发与环境搭建](#三-本地开发与环境搭建)
  - [环境要求](#环境要求)
  - [编译与运行测试](#编译与运行测试)
  - [无 C++ 工具链的轻量验证](#无-c-工具链的轻量验证)
- [四、 代码规范与设计哲学](#四-代码规范与设计哲学)
  - [设计与编码原则](#设计与编码原则)
  - [C++ 代码风格](#c-代码风格)
  - [C# / WPF 代码风格](#c--wpf-代码风格)
  - [PowerShell 脚本风格](#powershell-脚本风格)
- [五、 Git 提交规范](#五-git-提交规范)
- [六、 合并请求（Pull Request）流程](#六-合并请求pull-request流程)

---

## 一、 行为准则

我们致力于为所有参与者打造一个友好、包容、互相尊重的开源社区。在沟通与协作中，请保持友善与客观，尊重不同意见与技术探讨。

---

## 二、 如何参与贡献

### 1. 报告缺陷（Bug Report）
如果你在安装、日常打字、候选框渲染、宿主兼容性（如 Office、浏览器、游戏等）或设置界面中发现了问题：
1. 请先在 GitHub Issues 中搜索，确认该问题是否已被提出或已有解决方案。
2. 若未找到，请创建新的 Issue，并选用 **问题反馈（Bug Report）** 模板，尽可能详细地提供：
   - 操作系统版本（如 Windows 11 23H2 / 24H2）
   - 输入法版本号（如 v2.0.1）
   - 复现步骤、期望效果与实际效果
   - 相关的截图或录屏

### 2. 提出功能建议（Feature Request）
如果你有新的功能想法（如词库支持、皮肤交互、分词算法、快捷键模式等）：
- 欢迎在 Issues 中选用 **功能建议（Feature Request）** 模板发起讨论，说明需求的使用场景与解决的痛点。

### 3. 改进文档
文档中若有表述不清、过时或错误之处，可以直接修改并提交 Pull Request。

### 4. 贡献代码
如果你打算实现一个较大的新功能或进行深度的架构重构，建议先提交 Issue 进行方案讨论，达成共识后再着手编码，以避免不必要的重复劳动。

---

## 三、 本地开发与环境搭建

### 环境要求

1. **操作系统**：Windows 10 / Windows 11（x64）
2. **C++ 工具链**：Visual Studio 2022 Build Tools（包含 MSVC v143 及 Windows 10/11 SDK，包含 `msctf.h`）
3. **CMake**：>= 3.20
4. **.NET SDK**：.NET 8.0 SDK（用于编译 WPF 设置中心与 C# 演练工具）
5. **打包工具（可选）**：NSIS 3.x（仅在生成完整图形安装包时需要）

检查本地开发环境是否就绪：
```powershell
powershell -ExecutionPolicy Bypass -File scripts\check_env.ps1
```

### 编译与运行测试

项目的正式编译入口统一为 `scripts\build.ps1`，它会自动配置 CMake、编译 Release 目标并运行完整的 CTest 自动化测试套件：

```powershell
# 编译 Release 版本并执行自动化测试
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
```

构建产物将输出在 `artifacts\release` 目录。

生成单文件图形安装包：
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_installer.ps1
```

### 无 C++ 工具链的轻量验证

若仅参与 C# 设置中心开发或拼音引擎排序演练，无需安装完整 MSVC 工具链：
```powershell
# 运行 C# 引擎交互演练工具
dotnet run --project tests\engine_playground_cs

# 编译 WPF 设置中心
dotnet build settings\ShuruSettings.csproj -c Release
```

---

## 四、 代码规范与设计哲学

### 设计与编码原则

* **保持简单（KISS）**：输入法运行于各个宿主进程的高频输入路径上，逻辑越清晰精简越可靠，避免晦涩难懂的过度设计。
* **单一职责（Single Responsibility）**：每个函数只做一件事，核心引擎与界面渲染严格解耦。
* **防御性编程**：严格校验外部输入与数据边界，防范空指针、越界与非法格式；杜绝吞掉异常，确保关键错误能快速暴露。
* **高内聚低耦合**：TSF 接口层、拼音引擎层、词库层和 UI 渲染层边界清晰。
* **童子军军规**：离开营地时，让代码比你发现时更整洁。

### C++ 代码风格

* 遵循现代 C++ 规范（C++20 标准）。
* 类名与结构体采用 PascalCase（如 `PinyinEngine`、`CandidateWindow`）。
* 成员变量建议采用下划线后缀或小驼峰（如 `candidate_list_` 或 `m_candidates`，与现有模块保持风格一致）。
* 局部变量和函数参数采用 snake_case 或 camelCase。
* 注释解释“为什么这么做（Why）”，避免无意义的自明性注释。

### C# / WPF 代码风格

* 遵循微软官方 C# 编码规范，使用 .NET 8 现代语法。
* 属性与方法名采用 PascalCase，私有成员变量采用 `_camelCase` 前缀。
* XAML 布局保持结构清晰，避免硬编码不可调整的绝对尺寸，确保高 DPI 下的自适应。

### PowerShell 脚本风格

* 统一使用 UTF-8 编码保存脚本文件。
* 脚本参数显式声明类型与默认值，支持 `-Verbose` 与良好的错误提示。

---

## 五、 Git 提交规范

我们遵循约定式提交（Conventional Commits）规范，提交信息统一使用**中文**说明修改内容与原因：

```text
<类型>(<可选范围>): <简短描述>

[可选的详细说明]

[可选的关联 Issue，例如：Fixes #123]
```

### 常用类型说明：

| 类型 | 说明 | 示例 |
| :--- | :--- | :--- |
| `feat` | 新增功能 | `feat: 增加全拼双拼一键快捷键切换` |
| `fix` | 修复缺陷 | `fix: 消除候选框快速打字时的逐键闪跳` |
| `docs` | 文档变更 | `docs: 完善本地开发环境配置说明` |
| `style` | 代码格式调整（不影响代码逻辑） | `style: 规范候选窗绘制相关的缩进与空格` |
| `refactor` | 重构（既不是新增功能也不是修复缺陷） | `refactor: 提取双数组 Trie 树遍历公共逻辑` |
| `perf` | 性能优化 | `perf: 优化高频输入下的内存分配与词频查询` |
| `test` | 增加或修正测试用例 | `test: 增加模糊音匹配边界条件单元测试` |
| `chore` | 构建流程、依赖管理或辅助工具变动 | `chore: 更新 NSIS 打包脚本中的文件清单` |

---

## 六、 合并请求（Pull Request）流程

1. **Fork 本仓库** 到你个人的 GitHub 账号。
2. 从 `main` 分支切出你的特性分支：
   ```bash
   git checkout -b feat/your-feature-name
   ```
3. 在本地完成代码修改，并确保所有本地测试均已通过：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release
   ```
4. 提交你的修改并推送到你 Fork 的远程分支：
   ```bash
   git push origin feat/your-feature-name
   ```
5. 在 GitHub 上向本项目发起 Pull Request，并填写 PR 模板中的各项内容。
6. 关注 CI 构建状态及 Code Review 反馈，配合完成必要的修改与讨论。
7. 审核通过并合入后，你的贡献将正式成为项目的一部分！

再次感谢你的贡献！
