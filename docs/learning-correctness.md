# 用户学习与持久化一致性

本文描述当前用户词、搭配学习和跨进程保存协议。主要实现为
[PinyinEngine](../src/engine/pinyin_engine.cpp)、
[Dictionary](../src/engine/dictionary.cpp)、
[原生用户词存储](../src/engine/user_dictionary_store.h)、
[设置侧用户词存储](../settings/UserDictionaryStore.cs)和
[搭配模型](../src/engine/user_bigram.h)。

## 数据与排序

用户词位于 `%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`，
与系统词库分开保存。普通词条为五列 TSV：

```text
全拼<TAB>词条<TAB>兼容词频<TAB>selection_count<TAB>last_used_unix
```

旧三列格式仍可读取。运行时按“规范全拼 + 词条”定位元数据，
不能使用候选重排前的下标写回次数或时间。混拼学习写入规范全拼词段，
自定义短语、复制记录、原始输入回退等不可学习候选不写入用户词。

`Dictionary::ComputeLearningScore` 综合选择次数的对数证据与时间衰减，
时间尺度为 30 天，学习分数上限为 90。最终候选排序还使用匹配成本、
词频、来源和模型得分，不能简化为固定的“匹配成本减学习分”公式。

基础词典与英文索引为不可变共享数据，用户词保存在独立覆盖层。
学习或重载只更新用户层，不重建整份系统词库。

## 代次与跨进程保存

当前文件头包含两个标记：

- `# generation=` 管理用户词表代次。
- `# bigram_generation=` 管理搭配数据代次。

设置程序与原生引擎共用命名互斥量 `Local\CaishenPinyin.UserDictionary`。
保存读取磁盘上的当前代次，仅合并本进程未确认的学习或撤销增量，
通过临时文件和原子替换发布；成功后才确认已持久化的增量。

| 操作 | 对持久化状态的影响 |
|---|---|
| 学习 | 累计当前代次的新增次数与词频 |
| 撤销 | 记录针对最近学习的减量，已落盘数据也能撤销 |
| 清空 | 切换词表与搭配代次并删除搭配文件；旧进程积压增量不能恢复旧数据 |
| 合并导入 | 更新词表代次并合并内容，保留已有搭配代次 |
| 保存失败 | 保留未确认增量，后台重试 |
| 外部更新 | 比较文件身份并重新发布用户覆盖层，基础词库继续复用 |

没有代次标记的旧文件按兼容规则读取；文件身份也用于检测旧格式文件被修改。
导入、清空、后台保存和焦点恢复都必须遵守同一协议，不能用一次全量覆盖绕开代次检查。
同一版本的 DLL 和设置程序应配套更新，升级后退出仍使用旧 DLL 的宿主。

## 搭配模型

用户搭配记录保存在 `user_bigram.txt`，按前词与后词合并增量。

- 最多 4096 个前词，每个前词最多 16 个后继。
- 运行时更新、文件加载和待保存状态都受容量边界约束。
- 并发进程各自的新增计数相加，不以最后一个进程的完整快照覆盖其他进程。
- 保存成功后确认增量；保存失败和再次保存不能重复计数。
- 查询读取不可变快照；独占时可以原地更新，避免不必要的完整复制。

搭配用于后续查询排序。引擎保留 `PredictNext` 接口，
但当前输入服务不显示上屏后的独立联想候选窗。

## 首次加载与学习权限

词库尚未就绪时保留组合状态，最多同步等待 300 ms 后转入 UI 线程轮询。
回调校验输入世代与上下文，选词前可补查刚就绪的候选；
焦点改变或取消后不使用旧回调。该流程属于候选就绪管理，不改变学习文件代次。

个人学习目录使用当前用户受保护 DACL。权限初始化失败时关闭学习写入；
AppContainer 宿主只使用公共词库和资源，不直接读写个人词、
搭配、短语、置顶及统计。权限细节见[隐私策略](privacy-input-policy.md)。

## 回归与复测

| 入口 | 验证内容 |
|---|---|
| `p1_engine` | 学习衰减、撤销、旧格式读取、缓存损坏与候选质量 |
| `learning_persistence` | 代次、清空/导入、并发进程合并、保存失败、搭配容量及元数据重排 |
| `dictionary_stress` | 并发查询、学习期间查询继续推进及线程调度压力 |
| `shared_engine_lifecycle` | 共享引擎就绪、引用和卸载收尾 |
| `engine_snapshot` | 传统装载与映射快照的查询结果一致 |
| `appcontainer_acl` | 公共资源与个人文件的真实沙箱访问边界 |
| `settings_logic` | C# 用户词协议、清空和原生互操作所需的文件行为 |

```powershell
ctest --test-dir build-release -C Release -R "p1_engine|learning_persistence|dictionary_stress|shared_engine_lifecycle|engine_snapshot|appcontainer_acl" --output-on-failure
dotnet run --project tests\settings_logic
```

`tests/engine_playground/learning_persistence_test.cpp` 使用两个真实进程检查合并结果；
`dictionary_stress_test.cpp` 用协调后的并发查询压力验证学习路径。
这类测试的墙钟峰值受 CPU 调度影响，
不能单凭一次峰值推断发生了全词库复制。

测试数量、超时和具体调用以 [CMakeLists.txt](../CMakeLists.txt) 为准。
首次加载和学习协议回归不能替代 Word、浏览器或 SearchHost 的实际输入验收。
