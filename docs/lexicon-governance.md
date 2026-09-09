# 词库、模型与缓存治理

[manifest.json](../data/lexicon/manifest.json) 固定包 ID、schema、逻辑版本、
文件大小、条目数、来源、许可证和 SHA-256。当前词库逻辑版本为 `2.0.1`，
应用版本为 `2.0.2`；两者独立维护。

## 系统包与来源

| 文件 | 当前用途 |
|---|---|
| `base_dict.txt` | 白霜派生基础词典，当前 677,441 条 |
| `char_dict.txt` | 单字读音，当前 8,247 条 |
| `en_dict.txt` | 英文词库，当前 250,504 条 |
| `base_dict.txt.bin`、`char_dict.txt.bin` | 对应文本的派生缓存，当前已列入 manifest |
| `system_ngram.bin` | 必需的字符二元/三元回退模型 |
| `system_lexeme_prior.bin` | 单字、双字常用度先验 |
| `GPL-3.0.txt` | 随词库提供的许可证 |

中文源固定到 Rime Frost 提交 `2aedeea96c1468c1caa17cea01864419a11a4b26`。
生成器按 `DEFAULT_SOURCES` 读取选定词典，接受 BMP 基本汉字、合法全拼和可解析频率，
合并项目维护的 `custom_dict.txt`，输出确定性排序。
逐源哈希及接受、拒绝、重复统计见[导入报告](frost-import-report.json)。

`system_ngram.bin` 来自固定 Rime Ice 字典的词内二元、三元频次；
`system_lexeme_prior.bin` 对单字融合独立字频与词内上下文频次，对双字使用白霜词频。
英文词典合并固定的 rime-easy-en 上游与项目旧词条，并保留 ECDICT 归属。
具体提交、哈希、许可证全文入口见[第三方声明](../THIRD_PARTY_NOTICES.md)。

运行时只读取派生产物，不执行 Python、不读取上游 YAML，也不联网。

## 可选完整模型

`rime-moqi-zh.gram` 固定参考上游 Release `1.0.0`，大小为 192,703,532 字节，
SHA-256 为 `35993085E9CE5D9722050BD548B807572EDCDD784ABF8079152091F8CD9BC731`。
它是本地统计 N-gram，使用只读文件映射和兼容的 Darts 双数组读取，
不是神经网络模型。

上游没有明确许可证声明，归属记录保持 `NOASSERTION`。该文件不提交 Git、不进入
正式发行包。用户应在具有相应权利时自行取得副本，放入已安装词库的当前版本目录。

引擎依次尝试完整墨奇、`system_ngram.bin`、兼容旧安装的 `zh-moqi.gram`。
当前清单不包含这两种 `.gram`；缺少它们不影响包校验，必需回退模型仍须存在。
当前清单生成器的 `FILES` 列表不包含 `.gram`，不会为它生成文件条目。
源码保留旧版 `runtimeOptional` 元数据兼容；发行组装会跳过这类可选条目，
包验证仍禁止携带 `.gram`。

## 两层派生缓存

### 文本缓存

`src/engine/lexicon_cache.cpp` 与 `scripts/build_lexicon_cache.py` 使用
`FCPYLEX1` 格式版本 1。头部包含源文本 SHA-256、负载 SHA-256 和条目数；
运行时还检查字段长度及边界。校验失败时回退文本，并尝试原子重建缓存。

可编辑文本仍是权威源，但当前的两份中文缓存已经纳入系统 manifest。
更新文本后必须一起更新对应缓存和清单，不能保留“缓存不属于 manifest”的旧约定。
清单工具对缓存检查大小和文件哈希，格式、源哈希与负载校验由运行时缓存读取器完成。

### EngineSnapshot v2

[engine_snapshot.cpp](../src/engine/engine_snapshot.cpp) 将最终系统词条、
键索引、字符/音节 Trie、音节表、指纹和英文词典保存为偏移量布局。
引擎可直接采用只读映射，语言模型另行加载。

快照是运行时派生缓存，不属于系统词库 manifest，位于
`%LOCALAPPDATA%\CaishenPinyin\snapshot\`。头部记录源文件 size、mtime 和生成时 SHA-256；
正常加载比较源文件身份并验证索引、区段和 Trie 结构，不在每次启动重新散列所有源文件。
失效后使用传统装载并尝试生成新快照。个人学习数据不写入系统快照。

正式构建另提供 `engine_snapshot_build_tool.exe`，
安装阶段可尝试预生成；是否成功不影响正常装载回退。详见[安装说明](installer.md#个人数据权限与快照)。

## 生成和验证

以下示例在临时产物目录重建当前中文数据，沿用清单中的其他必需文件。
先将 `$frostRoot` 指向已检出固定提交的上游仓库，核对提交与导入报告后再接受结果。

```powershell
$frostRoot = 'C:\src\rime-frost'
$stage = 'artifacts\lexicon-stage'
$currentManifest = Get-Content data\lexicon\manifest.json -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($entry in $currentManifest.files) {
    if (-not $entry.runtimeOptional) {
        Copy-Item -LiteralPath (Join-Path 'data\lexicon' $entry.path) -Destination (Join-Path $stage $entry.path)
    }
}
python scripts/build_frost_lexicon.py `
    --frost-root $frostRoot --output-dir $stage `
    --custom data/lexicon/custom_dict.txt --report artifacts/frost-import-report.json
python scripts/build_lexicon_cache.py --source "$stage/base_dict.txt" --out "$stage/base_dict.txt.bin"
python scripts/build_lexicon_cache.py --source "$stage/char_dict.txt" --out "$stage/char_dict.txt.bin"
python scripts/build_lexeme_prior.py `
    --char-source "$stage/char_dict.txt" --base "$stage/base_dict.txt" --out "$stage/system_lexeme_prior.bin"
python scripts/lexicon_manifest.py generate `
    --dir $stage --manifest "$stage/manifest.json" --version $currentManifest.version --schema 2
python scripts/lexicon_manifest.py validate --dir $stage --manifest "$stage/manifest.json"
```

该示例用于复现现有逻辑版本；真正改变来源、内容或转换规则时，
维护者应选择新的词库版本并同步正式报告、缓存、清单和第三方声明。
英文与字符模型的单独生成参数见对应脚本的 `--help`。

正式仓库校验：

```powershell
python scripts/lexicon_manifest.py validate --dir data/lexicon --manifest data/lexicon/manifest.json
```

校验包括文本格式、重复键词对、频率范围、文件哈希，以及相应模型的 magic、
版本、排序和结构。空行及 `#`、`;` 注释不计入有效文本条目数。

## 安装与个人数据

系统包位于 `%ProgramData%\CaishenPinyin\data\lexicon\versions/<逻辑版本>-<清单哈希前缀>`，
通过小型 `current` 指针切换。运行时优先读取这个版本目录，
缺失时兼容 DLL 附近的 `data\lexicon`。

`user_dict.txt`、`user_bigram.txt` 和 `custom_phrases.txt` 属于当前用户数据，
不随系统包覆盖。升级可保留用户自行安装的完整模型，
但不能因此将该模型加入发布包。安装事务与回滚见[安装说明](installer.md)。
