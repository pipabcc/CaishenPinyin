# 词库包治理

系统词库包位于 `data/lexicon`，由 `manifest.json` 固定包 ID、schema、版本、来源、许可证声明、有效条目数和 SHA-256。空行及以 `#`/`;` 开头的注释不计入条目数。`user_dict.txt` 是用户数据，不属于系统包，也不得在升级时复制或覆盖。

中文基础词库固定使用 rime-ice `569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57`
的 `cn_dicts/base.dict.yaml`，许可证标识为 `GPL-3.0-only`。`custom_dict.txt` 是项目维护的
小型纠错与专名层，构建时覆盖同键词条。升级上游必须先更新固定提交、许可证记录，
再运行生成、校验和候选回归，不能直接抓取浮动分支发布。

系统字符语言模型 `system_ngram.bin` 使用同一固定提交中的
`cn_dicts/ext.dict.yaml` 与 `cn_dicts/tencent.dict.yaml` 离线生成，许可证同为
`GPL-3.0-only`。生成器 `scripts/build_system_ngram.py` 只提取中文词条内部的二元、
三元字符频次，运行时不读取上游 YAML，也不联网。二进制格式固定为
`CSNGRM1\0`、格式版本 1、有序二元记录和有序三元记录；清单校验会检查 magic、
版本、记录上限、严格递增键、非零频次、中文字符范围及精确文件长度。

可编辑文本是权威源；运行时优先加载按源文件 SHA-256 校验的 `.bin` 索引缓存。

生成并校验：

```powershell
python scripts/lexicon_manifest.py generate --version 1.3.0 --max-frequency 50000000
python scripts/lexicon_manifest.py validate
```

校验器严格检查中文三列、纯小写拼音、整数频率、英文两列格式、完全重复键词对、二进制语言模型结构以及可配置频率上下限。当前英文词库来源仍无法从仓库内可靠确认，因此继续标为 `NOASSERTION`；发布前必须由维护者补齐可审计的上游版本与许可证，不得据此推断或伪造许可证。

部署使用 `scripts/install_ime.ps1`。系统包安装到稳定数据根的 `versions/<version>`，通过小型 `current` 指针原子切换；默认机器根为 `%ProgramData%\FacaiPinyin\data\lexicon`。引擎优先读取该包，缺失或损坏指针时兼容 DLL 旁 `data\lexicon`。用户词始终位于 `%LOCALAPPDATA%\FacaiPinyin\data\lexicon\user_dict.txt`。
