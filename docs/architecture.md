# 发财拼音技术设计（Phase 0）

## 1. 目标

交付可在 Windows 11 安装启用的本地拼音输入法骨架，支持全拼输入、候选选择、基础学习接口，默认离线。

## 2. 模块划分

### 2.1 TSF 接入层（`src/ime`）

| 类 | 职责 |
|---|---|
| `TextService` | `ITfTextInputProcessorEx` 主服务 |
| `ClassFactory` | COM 类工厂 |
| `Insert/Set/Start/EndCompositionEditSession` | 文档编辑会话 |
| `CandidateWindow` | 无焦点候选窗 |
| `Register*` | CLSID / Language Profile 注册 |

关键 GUID：

- CLSID: `{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}`
- Profile: `{3A8B5C2E-9D1F-4E6A-B7C8-5D2E9F1A3B6C}`
- LangID: `0x0804`（zh-CN）

### 2.2 引擎层（`src/engine`）

查询优先级：

1. 精确拼音命中
2. 前缀扩展
3. 回溯截断（`nihao` → `niha` → ... → `ni`）
4. 回退为原始拼音，保证可上屏

学习：`IncreaseUserWord` 提升词频，并由后台线程合并、持久化用户词典。

### 2.3 数据层

- `base_dict.txt`：系统词库
- `user_dict.txt`：用户词库
- 运行时从 `ShuruIme.dll` 旁 `data/lexicon` 加载

### 2.4 设置层（`settings`）

WPF 程序，读取注册表 CLSID 检查安装状态，打开词库目录。高级开关先占位。

## 3. 按键状态机（简化）

```text
[空闲]
  字母 → [组合中] 更新拼音/候选/组合串
[组合中]
  字母 → 追加
  Backspace → 删除；空则回空闲
  Space/数字 → 上屏 → 空闲
  Enter/单击 Shift → 上屏原始拼音 → 空闲
  Esc → 清空 → 空闲
[任意]
  无组合时单击 Shift → 切换 english_mode
  Ctrl+Space → 切换 english_mode
```

Shift 在按下时进入待定状态，只有未与其他按键组合的抬起事件才执行动作；密码框等敏感输入域完全旁路。Enter 与组合中的 Shift 共用原始拼音提交路径，避免两套上屏逻辑产生行为差异。

候选窗采用 Win32 无焦点弹窗和双缓冲绘制。内容未变化时不重复调整窗口或擦除背景；组合串与右侧页码使用独立矩形，长组合串只在自身区域内省略。窗口区域与背景均使用 8 像素圆角，垂直内边距为 5 像素。

## 4. 构建产物

- `ShuruIme.dll`：TSF 文本服务
- `engine_playground.exe`：C++ 引擎演练
- `ShuruSettings.exe`：设置
- `EnginePlayground`（C#）：无 MSVC 时的词库验证

## 5. 风险与后续

- 光标跟随目前用 `GetCaretPos` 近似，复杂控件需 `ITfContextView::GetTextExt`
- 未做代码签名，部分环境可能拦截
- 词库规模与语言模型直接决定“好用”程度
- 需真机矩阵：Notepad、Word、Chrome、WinUI、终端、全屏游戏

## 6. 验收标准（Phase 0）

1. DLL 可 `regsvr32` 注册
2. 语言列表可见“发财拼音”
3. 记事本可输入 `nihao` 选“你好”
4. 候选窗可见
5. 设置程序可显示注册状态

## 7. Phase 1 引擎增强

### 词库

- 系统词：`base_dict.txt`（由 `scripts/build_lexicon.py` 从固定版本 rime-ice 转换，默认约 20 万高频词）
- 用户词：`%LOCALAPPDATA%\FacaiPinyin\data\lexicon\user_dict.txt`；上屏学习后
  合并短时间内的连续更新并异步落盘，兼容迁移旧安装目录词典
- 注册脚本复制词库时**不覆盖**已存在的 `user_dict.txt`

### 查询顺序

1. 若输入像简拼（无 aeiou/v）：简拼精确/短前缀 → 全拼精确
2. 否则：全拼精确 → 全拼前缀 → 简拼补充
3. 仍无结果：回溯截断；再无则回退原始拉丁

### 简拼

- `pinyin_syllables.h`：约 410 个合法音节
- 加载时建立 `jianpin -> [full_pinyin...]` 索引
- 例：`srf`→输入法，`zm`→怎么，`nh`→你好等

### 当前注册产物

- 优先 `dist\ShuruImeN.dll` 最高版本（本轮为 ShuruIme7.dll）


## 8. Phase 1 增强

### Display Attribute
- `ITfDisplayAttributeProvider` 挂在 TextService
- 注册 `GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER`
- 组合 range 写 `GUID_PROP_ATTRIBUTE`（蓝色下划线，`TF_ATTR_INPUT`）

### 状态栏 / 软键盘
- 右下角状态条：中/英切换、软键盘开关
- 软键盘：26 键 + 空格/退格/Esc；`SendInput` 注入；快捷键 F9

### 光标跟随
1. 只读 edit session + `GetTextExt(ec)`
2. `GetTextExt(TF_INVALID_COOKIE)`
3. `GUITHREADINFO`
4. `GetCaretPos` + DPI 行高
5. 焦点窗客户区兜底
6. 多监视器工作区夹取，下方不够则翻到上方

### P1 带权模糊与学习
- 模糊候选使用确定性代价：精确 0、声母 100、韵母 180、漏元音 320；排序先比较代价，不依赖生成顺序。
- `FuzzyConfig` 可分别关闭三类规则，并严格限制 `max_cost`、`max_variants`、`max_work`。
- 用户学习保存在独立覆盖层，提供撤销最近一次、导入、导出、清空 API；旧三列 TSV 继续可读，写盘保持命名互斥量、临时文件和原子替换。
- 密码/私密输入域和关闭学习时不进入学习写入路径。

### 索引
- 全拼前缀：字符 trie
- 简拼：hash + 有序键 `lower_bound` 前缀区间
- 基础词库为不可变共享快照，用户词频为独立小快照；查询同时捕获两层，
  学习只复制用户层，不复制基础词库及英文索引


## 9. 进程内共享资源

- SharedEngine：词库/引擎只加载一次
- SharedStatusUi：每个 TSF UI 线程只创建一套状态栏和软键盘；同一线程内由
  Bind/Unbind 绑定当前前台 TextService，不同 UI 线程互不抢占 F9 软键盘回调
