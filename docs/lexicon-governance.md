# 词库包治理

系统词库包位于 `data/lexicon`，由 `manifest.json` 固定包 ID、schema、版本、来源、许可证声明、有效条目数和 SHA-256。空行及以 `#`/`;` 开头的注释不计入条目数。`user_dict.txt` 是用户数据，不属于系统包，也不得在升级时复制或覆盖。

中文基础词库的历史派生记录为 rime-ice
`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57` 的 `cn_dicts/base.dict.yaml`，许可证标识为
`GPL-3.0-only`。该提交当前不能从官方仓库重新解析，因此现有派生文件以清单中的
SHA-256 为权威，不能再宣称仅凭该旧提交即可复现。`custom_dict.txt` 是项目维护的小型
纠错与专名层。下次升级基础词库必须改用可解析提交并记录上游文件哈希。

系统字符语言模型 `system_ngram.bin` 使用同一固定提交中的
`cn_dicts/ext.dict.yaml` 与 `cn_dicts/tencent.dict.yaml` 离线生成，许可证同为
`GPL-3.0-only`。生成器 `scripts/build_system_ngram.py` 只提取中文词条内部的二元、
三元字符频次，运行时不读取上游 YAML，也不联网。二进制格式固定为
`CSNGRM1\0`、格式版本 1、有序二元记录和有序三元记录；清单校验会检查 magic、
版本、记录上限、严格递增键、非零频次、中文字符范围及精确文件长度。

短字短词先验 `system_lexeme_prior.bin` 与字符 N-gram 并行加载，保存
`拼音 + 单字/词语 -> 常用度`，不替换 `system_ngram.bin`。单字来源固定为 rime-ice
提交 `c398c0d4526b012cb3b306f792089abed13e0413` 的 `8105.dict.yaml`，上游文件
SHA-256 为 `67813D950E23C5CE16ACCF38246E9ABCA0AB45222B32073E0B86E942645C7A1B`；其注释说明
字频来自 25 亿字语料。单字先验按
`sqrt(8105 字频 * base_dict 中该字的加权出现频率)` 计算，双字及多字词沿用基础词典
词频。生成器 `scripts/build_lexeme_prior.py` 输出 `CSLXPR1\0` 格式版本 1 的严格有序
变长记录。运行时只加载生成后的二进制并做内存二分查询，不运行 Python、不读取上游
YAML、不联网，也不加载 AI 模型。

可编辑文本是权威源；运行时优先加载按源文件 SHA-256 校验的 `.bin` 索引缓存。

生成并校验：

```powershell
python scripts/build_lexeme_prior.py `
  --char-source <固定提交的8105.dict.yaml> `
  --expect-char-sha256 67813D950E23C5CE16ACCF38246E9ABCA0AB45222B32073E0B86E942645C7A1B `
  --base data/lexicon/base_dict.txt `
  --out data/lexicon/system_lexeme_prior.bin
python scripts/lexicon_manifest.py generate --version 1.4.0 --max-frequency 50000000
python scripts/lexicon_manifest.py validate
```

校验器严格检查中文三列、纯小写拼音、整数频率、英文两列格式、完全重复键词对、
两种二进制模型的 magic、版本、记录顺序、非零分值和精确文件边界，以及可配置频率
上下限。当前英文词库来源仍无法从仓库内可靠确认，因此继续标为 `NOASSERTION`；
发布前必须由维护者补齐可审计的上游版本与许可证，不得据此推断或伪造许可证。

部署使用 `scripts/install_ime.ps1`。系统包安装到稳定数据根的 `versions/<version>`，
通过小型 `current` 指针原子切换；默认机器根为
`%ProgramData%\CaishenPinyin\data\lexicon`。引擎优先读取该包，缺失或损坏指针时兼容
DLL 旁 `data\lexicon`。用户词始终位于
`%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`。
