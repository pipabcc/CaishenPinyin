# 词库包治理

系统词库包位于 `data/lexicon`，由 `manifest.json` 固定包 ID、schema、版本、来源、许可证声明、有效条目数和 SHA-256。空行及以 `#`/`;` 开头的注释不计入条目数。`user_dict.txt` 是用户数据，不属于系统包，也不得在升级时复制或覆盖。

中文基础词库固定为 Rime Frost 提交
`2aedeea96c1468c1caa17cea01864419a11a4b26`，许可证为 `GPL-3.0-only`。
`scripts/build_frost_lexicon.py` 读取 8105、base、ext、others、corrections 及选定细胞
词库，只接受 BMP 基本汉字、合法拼音和可解析频率，再合并项目维护的
`custom_dict.txt`。逐文件哈希和导入统计见 `docs/frost-import-report.json`。

完整语言模型为 `rime-moqi-zh.gram`，固定到
`gaboolic/rime-build-grammar` Release `1.0.0`，SHA-256 为
`35993085E9CE5D9722050BD548B807572EDCDD784ABF8079152091F8CD9BC731`。运行时使用
Windows 只读文件映射和兼容 librime-octagram 的 Darts 双数组查询，不把 193 MB 文件
复制到堆内存。模型是本地统计 N-gram，不是神经网络或生成式 AI；输入时会查询，
不会联网。上游仓库没有许可证文件，因此清单必须保持 `NOASSERTION`，公开再分发前
需要模型权利人的明确授权。

字符回退模型 `system_ngram.bin` 由 Rime Ice 固定提交
`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57` 的 `cn_dicts/ext.dict.yaml` 与
`cn_dicts/tencent.dict.yaml` 离线生成，许可证为 `GPL-3.0-only`。生成器只统计词条
内部的二元、三元汉字频次，输出 `CSNGRM1\0` 格式版本 1；运行时不读取上游 YAML。
该文件是 2.0.1 发布包的必需文件。

短字短词先验 `system_lexeme_prior.bin` 保存
`拼音 + 单字/双字 -> 常用度`。单字采用 25% 独立字频与 75% 词内上下文频次的几何
融合，双字沿用白霜词频；三字及以上交给词典和 Grammar 排序。生成器输出
`CSLXPR1\0` 格式版本 1 的严格有序变长记录。运行时仅加载派生二进制并做内存二分，
不运行 Python、不读取上游 YAML。

可编辑文本是权威源；运行时优先加载按源文件 SHA-256 校验的 `.bin` 索引缓存。

生成并校验：

```powershell
python scripts/build_frost_lexicon.py `
  --frost-root ciku/rime-frost-master白霜拼音 `
  --output-dir artifacts/frost-build `
  --custom data/lexicon/custom_dict.txt `
  --report docs/frost-import-report.json
python scripts/build_lexeme_prior.py `
  --char-source artifacts/frost-build/char_dict.txt `
  --base artifacts/frost-build/base_dict.txt `
  --out artifacts/frost-build/system_lexeme_prior.bin
python scripts/lexicon_manifest.py generate --version 2.0.1 --schema 2
python scripts/lexicon_manifest.py validate
```

校验器严格检查中文三列、纯小写拼音、整数频率、英文两列格式、完全重复键词对、
短词先验与字符 N-gram 的 magic/版本/顺序，以及 Grammar 元数据、双数组偏移、
根单元、单元上限和精确文件长度。清单还固定每个文件的大小和 SHA-256。英文词库来源同样保持
`NOASSERTION`，不得据此推断或伪造许可证。

部署使用 `scripts/install_ime.ps1`。系统包安装到稳定数据根的 `versions/<version>`，
通过小型 `current` 指针原子切换；默认机器根为
`%ProgramData%\CaishenPinyin\data\lexicon`。引擎优先读取该包，缺失或损坏指针时兼容
DLL 旁 `data\lexicon`。运行时依次尝试完整墨奇、`system_ngram.bin` 和旧版小
`zh-moqi.gram`；完整墨奇在清单中标为运行时可选，删除后健康检查仍通过，
`system_ngram.bin` 则必须存在且校验通过。小墨奇不列入 2.0.1 清单，仅兼容旧安装。
用户词始终位于
`%LOCALAPPDATA%\CaishenPinyin\data\lexicon\user_dict.txt`。
