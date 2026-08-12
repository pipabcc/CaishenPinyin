# 第三方数据与许可

## rime-ice 中文词库

- 项目：https://github.com/iDvel/rime-ice
- 固定提交：`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57`
- 使用文件：`cn_dicts/base.dict.yaml`
- 上游文件 Blob：`f736b646706680bc08068ddb8a1488d448a0e535`
- 许可证：GNU General Public License v3.0 only
- 本项目派生文件：`data/lexicon/base_dict.txt`、`char_dict.txt` 及对应 `.bin` 缓存
- 修改说明：筛选最多八音节、按权重保留约二十万条，并合并
  `data/lexicon/custom_dict.txt` 项目纠错层。

完整许可证文本见 `licenses/GPL-3.0.txt`；发布词库包内复制为 `GPL-3.0.txt`。
上游来源和派生文件哈希见
`data/lexicon/manifest.json`。

## 英文词库

`data/lexicon/en_dict.txt` 的历史来源目前无法从仓库内可靠确认，清单中保持
`NOASSERTION`。在完成来源与许可证审计前，不应把它描述为具有某个确定许可证。
