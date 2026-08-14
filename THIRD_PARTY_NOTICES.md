# 第三方数据与许可

## rime-ice 中文词库

- 项目：https://github.com/iDvel/rime-ice
- 历史派生版本记录：`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57`
- 使用文件：`cn_dicts/base.dict.yaml`
- 上游文件 Blob：`f736b646706680bc08068ddb8a1488d448a0e535`
- 许可证：GNU General Public License v3.0 only
- 本项目派生文件：`data/lexicon/base_dict.txt`、`char_dict.txt` 及对应 `.bin` 缓存
- 修改说明：筛选最多八音节、按权重保留约二十万条，并合并
  `data/lexicon/custom_dict.txt` 项目纠错层。

该历史提交当前不能从官方仓库重新解析；现有派生文件以
`data/lexicon/manifest.json` 记录的 SHA-256 为权威，不把旧提交描述为当前可复现来源。

## rime-ice 8105 常用字频

- 项目：https://github.com/iDvel/rime-ice
- 固定提交：`c398c0d4526b012cb3b306f792089abed13e0413`
- 使用文件：`cn_dicts/8105.dict.yaml`
- 上游文件 SHA-256：`67813D950E23C5CE16ACCF38246E9ABCA0AB45222B32073E0B86E942645C7A1B`
- 上游注释的数据来源：25 亿字语料汉字字频表
- 许可证：GNU General Public License v3.0 only
- 本项目派生文件：`data/lexicon/system_lexeme_prior.bin`
- 修改说明：与当前 `base_dict.txt` 的字词出现频率离线融合，生成单字、双字及多字
  词元先验；运行时仅加载派生二进制。

完整许可证文本见 `licenses/GPL-3.0.txt`；发布词库包内复制为 `GPL-3.0.txt`。
上游来源和派生文件哈希见
`data/lexicon/manifest.json`。

## 英文词库

`data/lexicon/en_dict.txt` 的历史来源目前无法从仓库内可靠确认，清单中保持
`NOASSERTION`。在完成来源与许可证审计前，不应把它描述为具有某个确定许可证。
