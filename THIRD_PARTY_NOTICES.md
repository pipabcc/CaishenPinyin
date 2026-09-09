# 第三方数据与许可

本文件记录当前源码和正式发行包使用的数据、格式兼容实现及运行时归属。
具体词库文件、来源和 SHA-256 以 [manifest.json](data/lexicon/manifest.json) 为准，
变更流程见[词库治理](docs/lexicon-governance.md)。应用版本与词库逻辑版本独立维护。

## Rime Frost 白霜拼音词库

- 项目：[Rime Frost](https://github.com/gaboolic/rime-frost)
- 固定提交：`2aedeea96c1468c1caa17cea01864419a11a4b26`
- 许可证：GNU General Public License v3.0 only
- 使用源：`cn_dicts/8105.dict.yaml`、`base.dict.yaml`、`ext.dict.yaml`、
  `others.dict.yaml`、`corrections.dict.yaml`，以及选定的细胞词库
- 派生产物：`base_dict.txt`、`char_dict.txt`、对应 `.bin` 缓存和
  `system_lexeme_prior.bin`

转换脚本只接受 BMP 基本汉字和合法全拼，合并项目维护的
`data/lexicon/custom_dict.txt`，并输出确定性排序。逐源文件 SHA-256、接受数、
拒绝数和重复数见 [导入报告](docs/frost-import-report.json)。完整 GPL 文本见
[GPL-3.0.txt](licenses/GPL-3.0.txt)，运行词库中另附 `GPL-3.0.txt`。

## Rime Ice 字符 N-gram 回退模型

- 项目：[Rime Ice](https://github.com/iDvel/rime-ice)
- 固定提交：`569ff3bc65dd4aec0a26b33c49c8bbdfa8b5fd57`
- 使用源：`cn_dicts/ext.dict.yaml`、`cn_dicts/tencent.dict.yaml`
- 许可证：GNU General Public License v3.0 only
- 派生产物：`system_ngram.bin`，`7,416,888` 字节
- SHA-256：`5FBC1AE57443CF46F8F40FE969E4E3E24927E888A053FB47437982C4B946F870`

`scripts/build_system_ngram.py` 仅提取词条内部的二元、三元汉字频次，输出严格有序的
`CSNGRM1\0` 二进制记录。运行时只读取该派生文件，不加载上游 YAML，也不联网。
完整 GPL 文本见 [GPL-3.0.txt](licenses/GPL-3.0.txt) 和运行词库中的 `GPL-3.0.txt`。

## 完整墨奇语言模型

- 项目：[rime-build-grammar](https://github.com/gaboolic/rime-build-grammar)
- 发布：`1.0.0`
- 文件：`rime-moqi-zh.gram`，`192,703,532` 字节
- SHA-256：`35993085E9CE5D9722050BD548B807572EDCDD784ABF8079152091F8CD9BC731`
- 许可证：`NOASSERTION`

截至本次固定版本，上游模型仓库和 GitHub Release 均未提供许可证文件。
本项目不能据此把该模型宣称为 GPL、BSD 或其他开源许可。个人本地构建可使用
用户自行下载的副本；公开再分发前应先向模型权利人取得明确授权。
当前源码仓库、Setup 和 Portable 均不提供该模型。

## librime-octagram 兼容读取

- 项目：[librime-octagram](https://github.com/lotem/librime-octagram)
- 参考提交：`bfb168ca33d8b372596fdf2007933f3da1cf360e`
- 许可证：BSD 3-Clause

本项目没有链接 librime；`SystemLanguageModel` 实现了兼容的 Grammar 元数据、
Unicode 编码、Darts 双数组查询和 Octagram 打分行为。许可证全文见
[BSD-3-Clause-librime-octagram.txt](licenses/BSD-3-Clause-librime-octagram.txt)。

## Darts-clone 双数组格式

- 项目：[Darts-clone](https://github.com/s-yata/darts-clone)
- 参考提交：`87b71afd6cf784953e3c08f24c64203397f3b724`
- 许可证：BSD 2-Clause

许可证全文见 [BSD-2-Clause-darts-clone.txt](licenses/BSD-2-Clause-darts-clone.txt)。
这里的格式兼容用于 Grammar 读取，不代表普通拼音词典使用同一双数组实现。

## 英文词库

`data/lexicon/en_dict.txt` 是由 `scripts/build_en_dict.py` 生成的确定性派生文件，
合并项目旧词条与以下固定上游：

- 项目：[rime-easy-en](https://github.com/BlindingDark/rime-easy-en)
- 固定提交：`54a4a07289412efc54134092c0d945f895a71ed3`
- 原始文件：`easy_en.dict.yaml`
- 原始文件 SHA-256：`4F039026B2746FA9B0D4D7A248CDF866B64609DCA2317708F04E9E68AC7D868A`
- 仓库许可证：GNU Lesser General Public License v3.0（全文见
  [LGPL-3.0-rime-easy-en.txt](licenses/LGPL-3.0-rime-easy-en.txt)）
- 词典致谢来源：`skywind3000/ECDICT`，其仓库声明 MIT License（全文见
  [MIT-ECDICT.txt](licenses/MIT-ECDICT.txt)）

本项目不把上游代码许可证扩展解释为词典数据的额外权利；发布包保留上述归属，
并在清单中固定来源提交和派生文件 SHA-256。

## .NET 8 桌面运行时

正式发布的 WPF 设置程序采用 `win-x64` 自包含方式，
携带 [.NET Runtime](https://github.com/dotnet/runtime) 和
[WPF](https://github.com/dotnet/wpf) 所需组件。

`scripts/build.ps1` 收集 .NET 安装目录的许可证与第三方声明到
`licenses/dotnet-LICENSE.txt` 和 `licenses/dotnet-ThirdPartyNotices.txt`。
这些文件在构建产物中生成，发行验证要求随包保留；
更新运行时后应使用对应版本的声明，不能只附本项目的 GPL 许可证。

Windows TSF、DirectWrite、GDI+ 和 winsqlite3 由目标 Windows 系统提供。
用户自行导入的词库或皮肤仍需遵守原权利人的许可；本项目的许可证不改变这些资源的授权范围。
