# AGENTS.md — 财神输入法（Caishen IME）

Windows 10/11 x64 原生拼音输入法。CMake 当前使用 C++17；输入服务为 TSF 进程内 COM DLL，
设置中心为 .NET 8 WPF。正式构建包含 x64 DLL 和面向 WOW64 应用的 x86 DLL，
不表示支持 32 位 Windows。输入法运行时全离线，开发和构建脚本可访问上游依赖。

## 目录结构

- `src/ime` — COM/TSF 生命周期、编辑会话、按键状态机；`ui/` 为 Win32、GDI+、DirectWrite 候选窗及软键盘。
- `src/engine` — 字符/音节 Trie、拼音检索、全拼与小鹤双拼、模糊纠错、模型、学习、复制记录和短语。
- `src/common` — 日志、`settings.ini`、私有 DACL、异步字数统计。
- `settings/` — `ShuruSettings.csproj`（`net8.0-windows`），WPF 设置、SQLite 历史、STA 监听和上屏辅助。
- `data/lexicon` — `manifest.json` 管理的系统词库、缓存和统计模型；`data/skins` 为内置皮肤。
- `scripts/` — 环境检查、正式构建、词库生成、打包与部署事务。
- `tests/engine_playground` — C++ CTest；`tests/settings_logic` — C# 逻辑测试；`tests/engine_playground_cs` — C# 演练。
- `installer/` — NSIS 与 Portable 脚本；`docs/` — 当前架构和专题说明；`docs/releases/` — 历史发行说明。

## 构建与验证

正式入口会加载存在的 `tools/env.ps1`。外部环境需要 VS2022 的 x64/x86 C++ 工具链、
Windows SDK、CMake、.NET 8 和 Python；Ninja 可选。

```powershell
# 正式构建：词库校验、双架构编译、相应 CTest、WPF 自包含发布和发行目录。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release

# 本地增量构建仍执行测试。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -Incremental

# 完成正式构建后排查单项。
ctest --test-dir build-release -C Release -R p1_engine -V

# 仅 C# 设置逻辑。
dotnet build settings\ShuruSettings.csproj -c Release
dotnet run --project tests\settings_logic
```

- 固定发布输出为 `artifacts/release/`，包含 `ShuruIme.dll`、`ShuruIme32.dll` 和自包含设置程序。
- Ninja 的 DLL 位于 `build-release/`、`build-release-x86/`；Visual Studio Generator 在相应目录下增加 `Release/` 或 `Debug/`。
- `-NoPackage` 仍编译、测试和发布 WPF，只跳过发行目录组装。
- x64 默认运行全部注册测试；`CAISHEN_SKIP_INTERACTIVE_TESTS=1` 跳过 `interactive-tsf` 标签。
  脚本识别为非交互环境时还跳过 `p1_engine`；x86 执行脚本列出的兼容性子集。
- 打包使用 `scripts/build_installer.ps1`、`scripts/build_portable.ps1`；
  环境检查使用 `scripts/check_env.ps1`。具体流程见[构建说明](docs/build.md)。

## 架构与隐私边界

- TSF 接入、引擎查询、数据访问和 UI 渲染保持职责分离。普通词典使用字符与音节 Trie；
  Darts 双数组格式用于兼容读取 Grammar，不能混为同一种索引实现。
- `SharedEngine` 在每个宿主进程共享引擎。EngineSnapshot 和大型统计模型使用只读映射；
  缓存失效时允许传统装载，不能在高频按键路径整份复制系统词库。
- `DllMain` 不等待线程、不销毁窗口；窗口和 TSF 操作留在所属 UI 线程。
- 密码、私密和 PIN 等敏感输入域完全旁路。日志不写原始输入、候选正文或剪贴板内容；
  统计持久化日期与字符数量。
- 个人数据位于 `%LOCALAPPDATA%\CaishenPinyin\`，使用受保护 DACL。ACL 失败时禁用学习写入，
  输入继续工作；AppContainer 仅使用公共词库和资源，不直接访问个人数据。
- 系统词库位于 `%ProgramData%\CaishenPinyin\data\lexicon\versions\`，由 `current` 指针选定。
  应用版本和词库逻辑版本独立，不能为了升级应用擅自修改词库版本或哈希。
- JSON、TSV、INI 及请求文件使用临时文件和原子替换；跨进程协议使用命名互斥量。
  复制记录数据库使用 SQLite 事务/WAL，旧 JSON 迁移使用迁移锁。
- 复制记录和短语以稳定身份定位；删除不提交候选、不关闭列表。文本上屏优先使用可用的
  原 TSF 会话，回退粘贴必须处理剪贴板占用、目标变化和取消。
- `data/lexicon/manifest.json` 固定来源与 SHA-256，当前也列出 `base_dict.txt.bin`、
  `char_dict.txt.bin`。`*.gram` 大模型不入库不入包；`user_dict.txt` 不得被安装或升级覆盖。

## 编码与提交约定

- C++ 使用当前 C++17 标准、MSVC `/utf-8 /W4` 和静态 CRT；
  字段命名随模块风格，注释解释原因。
- C#/WPF 使用 .NET 8；新增私有字段优先 `_camelCase`，保持现有模块一致，
  XAML 保持 DPI 自适应。
- PowerShell 参数显式声明类型。包含中文且由 Windows PowerShell 5.1 执行的脚本保存为
  UTF-8 BOM，避免中文注释被错误解码。
- 提交信息使用 Conventional Commits 和中文描述，如 `fix: 修复复制记录删除误上屏`。
  合并前完成适当验证和代码审查，明确宿主实测与自动化测试的边界。
- 应用版本统一修改 `src/common/version.h`。该文件被资源脚本包含，必须保持 ASCII；
  中文产品名由 `RuntimeConfig::display_name` 提供。
- 更新功能时同步当前文档和 `CHANGELOG.md` 的未发布部分；历史发行说明保持对应标签的范围。

## 已知注意事项

- Ninja 下的 `CMAKE_CL_SHOWINCLUDES_PREFIX` 用于正确识别中文 MSVC 头文件依赖，不能删除。
- 宿主加载 DLL 后不会自动换新。开发注册使用 `scripts/register_ime.ps1`；
  安装版使用部署事务；两者都需要退出旧宿主才能验证新代码。
- `tsf_e2e_core` 和首键测试使用 `TF_TMAE_NOACTIVATETIP` 隔离已安装输入法。
  `p1_engine` 使用全量词库，600 秒超时上限不代表正常每次都耗时 600 秒。
- 数字小键盘不选词；`v/vv` 的数字键用于筛选，`vvv` 的数字用于表达式。
- 手工运行 `ShuruSettings.exe -self-test` 时，必须给 `CAISHEN_CLIPBOARD_DATA_DIR`
  指定隔离目录。CTest 已配置该目录。

## 改动前的文档入口

- 架构、按键、窗口 → [架构说明](docs/architecture.md)、[组合生命周期](docs/composition-lifecycle.md)。
- 隐私、标点、数字键 → [隐私与输入策略](docs/privacy-input-policy.md)。
- 用户学习与保存 → [学习一致性](docs/learning-correctness.md)。
- 词库生成、缓存、模型 → [词库治理](docs/lexicon-governance.md)。
- 构建、发行 → [构建说明](docs/build.md)。
- 安装、回滚、权限、重载 → [安装与部署](docs/installer.md)。
