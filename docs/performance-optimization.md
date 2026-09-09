# 输入路径性能与测量

性能约束针对加载到宿主进程中的输入服务。基准需要同时检查候选质量、
持久化正确性和延迟，不能只以查询耗时判断用户输入体验。

## 当前实现

| 路径 | 当前约束 | 实现入口 |
|---|---|---|
| 字数统计 | 前台只累计日期与字符数，后台约 150 ms 合并保存；失败不退回输入线程同步写盘 | [typing_stats.cpp](../src/common/typing_stats.cpp) |
| 混拼查询 | 路径回溯、有界束搜索与候选边读取；输出时再组装正文和学习词段 | [pinyin_engine.cpp](../src/engine/pinyin_engine.cpp) |
| 工作预算 | 默认 300,000 单位，上限 1,000,000；索引、纠错和词图展开共同计量 | [QueryOptions](../src/engine/pinyin_engine.h) |
| 候选扩展 | 首屏一页加探测项，翻页/展开时追加，保持已显示候选顺序 | [text_service.cpp](../src/ime/text_service.cpp) |
| 配置与置顶 | 每轮调用复用配置快照，约 100 ms 文件检查，主动变更通知可立即失效 | [runtime_config.cpp](../src/common/runtime_config.cpp)、[pinned_candidate_store.cpp](../src/engine/pinned_candidate_store.cpp) |
| 剪贴板采集 | 独立 STA 线程读取、编码、散列和入库，UI 只接收完成通知 | [ClipboardCaptureWorker.cs](../settings/ClipboardCaptureWorker.cs) |
| 文本粘贴回退 | 后台即时写入 Unicode 文本；打开剪贴板阶段限时重试，取消后不继续提交 | [ClipboardTextWriter.cs](../settings/ClipboardTextWriter.cs) |

普通拼音查询、复制记录查询和 TSF 编辑会话属于不同阶段。
EngineSnapshot 减少系统词库冷加载的重复解析；它的命中率、读取缓存和宿主环境
会影响首次输入延迟，不能保证每台机器都在固定毫秒数内就绪。

## 可复现测量

完成正式构建后，在仓库根目录运行。Ninja 目标位于 `build-release`，
Visual Studio Generator 则位于 `build-release\Release`。

```powershell
$benchmarkDirectory = 'build-release'
if (Test-Path 'build-release\Release\query_benchmark.exe') {
    $benchmarkDirectory = 'build-release\Release'
}
$benchmark = Join-Path $benchmarkDirectory 'query_benchmark.exe'
& $benchmark data\lexicon artifacts\benchmark-user 10
& $benchmark data\lexicon artifacts\benchmark-user 90
```

第一个参数为词库目录，第二个为隔离用户目录，第三个为候选数量。
10 项通常对应默认 9 项首屏加一个探测项，90 项用于比较多页查询。
不要将真实用户词目录用于基准。

记录 Windows、CPU、架构、构建类型、词库清单与模型、冷热缓存状态及重复次数；
同时保存候选文本、拼音、覆盖长度、来源和学习词段的差异。
`QueryDiagnostics` 提供阶段工作量、耗时和预算耗尽状态，不记录用户输入正文。

## 历史基准

以下保留 2026-09-06 开发记录中的对比，环境为 Windows 11、Intel i5-4460、
Release、相同白霜词库与回退模型。查询预热后测量 30 次，
统计测量 200 次；本次文档同步没有重新测量这些数值。

| 输入或操作 | 优化前 90 候选中位耗时 | 优化后首屏中位耗时 | 优化后 90 候选中位耗时 |
|---|---:|---:|---:|
| `nihao` | 0.75 ms | 0.46 ms | 0.48 ms |
| `sh` | 20.69 ms | 7.14 ms | 8.72 ms |
| `suixinshuru` | 38.40 ms | 16.77 ms | 18.21 ms |
| `womenzhidaosuixinshuru` | 149.38 ms | 50.21 ms | 52.16 ms |
| `jintiantianqihenhaowomenyiqiqugongyuan` | 259.83 ms | 53.38 ms | 53.37 ms |
| 48 个连续 `z` | 1171.75 ms | 77.99 ms | 78.94 ms |
| 统计前台记录 | 4.46 ms，同步写盘 | 0.001 ms，仅入队 | 不适用 |

当时对比了 48 组输入的前九项，共 418 个候选位置，并检查文本、拼音、覆盖长度、
来源和分段信息。原始报告在开发机 `artifacts/audit-20260906/`，
属于本地验证产物，不随源码或发行包提供。

这些数值不包括完整 TSF 编辑、宿主排版和候选绘制，也不是所有设备的性能保证。
新的性能改动应使用同一环境重新建立基线，不能直接用上述数值作为 CI 硬阈值。

## 验证边界

- 性能改动同时运行引擎、统计、候选、学习持久化和设置逻辑的相关回归。
- 剪贴板合成采集和隔离窗口站测试验证内容、像素、重试及 UI 响应；
  第三方延迟提供、真实长期占用与大图片仍需要宿主验证。
- 正常 DLL 卸载会在 Loader Lock 外等待保存与回收；进程被强制终止时，
  最后一小批未落盘统计可能丢失。
- 交互测试、非交互测试及脚本跳过条件见[构建说明](build.md)，
  学习一致性约束见[学习协议](learning-correctness.md)。
