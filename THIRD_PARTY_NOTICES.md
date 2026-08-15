# 第三方数据与许可

## Rime Frost 白霜拼音词库

- 项目：https://github.com/gaboolic/rime-frost
- 固定提交：`2aedeea96c1468c1caa17cea01864419a11a4b26`
- 许可证：GNU General Public License v3.0 only
- 使用源：`cn_dicts/8105.dict.yaml`、`base.dict.yaml`、`ext.dict.yaml`、
  `others.dict.yaml`、`corrections.dict.yaml`，以及选定的细胞词库
- 派生产物：`base_dict.txt`、`char_dict.txt`、对应 `.bin` 缓存和
  `system_lexeme_prior.bin`

转换脚本只接受 BMP 基本汉字和合法全拼，合并项目维护的
`data/lexicon/custom_dict.txt`，并输出确定性排序。逐源文件 SHA-256、接受数、
拒绝数和重复数见 `docs/frost-import-report.json`。完整 GPL 文本见
`licenses/GPL-3.0.txt`，运行词库中另附 `GPL-3.0.txt`。

## Rime Ice 字符 N-gram 回退模型

- 项目：https://github.com/iDvel/rime-ice
- 固定提交：`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57`
- 使用源：`cn_dicts/ext.dict.yaml`、`cn_dicts/tencent.dict.yaml`
- 许可证：GNU General Public License v3.0 only
- 派生产物：`system_ngram.bin`，`7,416,888` 字节
- SHA-256：`5FBC1AE57443CF46F8F40FE969E4E3E24927E888A053FB47437982C4B946F870`

`scripts/build_system_ngram.py` 仅提取词条内部的二元、三元汉字频次，输出严格有序的
`CSNGRM1\0` 二进制记录。运行时只读取该派生文件，不加载上游 YAML，也不联网。
完整 GPL 文本见 `licenses/GPL-3.0.txt` 和运行词库中的 `GPL-3.0.txt`。

## 完整墨奇语言模型

- 项目：https://github.com/gaboolic/rime-build-grammar
- 发布：`1.0.0`
- 文件：`rime-moqi-zh.gram`，`192,703,532` 字节
- SHA-256：`35993085E9CE5D9722050BD548B807572EDCDD784ABF8079152091F8CD9BC731`
- 许可证：`NOASSERTION`

截至本次固定版本，上游模型仓库和 GitHub Release 均未提供许可证文件。
本项目不能据此把该模型宣称为 GPL、BSD 或其他开源许可。个人本地构建可使用
用户自行下载的副本；公开再分发包含该模型的安装包前，应先向模型权利人取得
明确授权。

## librime-octagram 兼容读取

- 项目：https://github.com/lotem/librime-octagram
- 参考提交：`bfb168ca33d8b372596fdf2007933f3da1cf360e`
- 许可证：BSD 3-Clause

本项目没有链接 librime；`SystemLanguageModel` 实现了兼容的 Grammar 元数据、
Unicode 编码、Darts 双数组查询和 Octagram 打分行为。许可证全文见
`licenses/BSD-3-Clause-librime-octagram.txt`。

## Darts-clone 双数组格式

- 项目：https://github.com/s-yata/darts-clone
- 参考提交：`87b71afd6cf784953e3c08f24c64203397f3b724`
- 许可证：BSD 2-Clause

许可证全文见 `licenses/BSD-2-Clause-darts-clone.txt`。

## 英文词库

`data/lexicon/en_dict.txt` 的历史来源目前无法从仓库内可靠确认，清单中保持
`NOASSERTION`。完成来源与许可证审计前，不应对外宣称确定许可证。
