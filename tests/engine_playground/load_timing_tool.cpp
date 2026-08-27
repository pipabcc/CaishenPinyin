// 加载基准（阶段1）：对指定词库目录分别执行一次传统装载与一次快照装载，
// 输出各自耗时。仅用于开发期基准测量，不属于 CTest 常规集。
//
//   load_timing_tool <lexicon_dir>
#include "engine/pinyin_engine.h"
#include "engine/dictionary.h"
#include "engine/english_dict.h"
#include "engine/engine_snapshot.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: load_timing_tool <lexicon_dir>\n");
        return 2;
    }
    const std::wstring dir(argv[1], argv[1] + std::strlen(argv[1]));
    if (dir.empty()) return 2;

    using clock = std::chrono::steady_clock;

    // 独立测量"映射+校验+注入"耗时，与 Initialize 其余步骤区分。
    {
        shuru::Dictionary d;
        shuru::EnglishDictionary e;
        const auto started = clock::now();
        const bool ok = shuru::TryAdoptEngineSnapshot(
            dir + L"\\base_dict.txt", dir + L"\\char_dict.txt",
            dir + L"\\en_dict.txt", &d, &e);
        const double ms =
            std::chrono::duration<double, std::milli>(clock::now() - started)
                .count();
        std::printf("adopt-only ok=%d %.1f ms\n", ok ? 1 : 0, ms);
    }
    auto run_once = [dir](const char* tag) {
        shuru::PinyinEngine engine;
        const auto started = clock::now();
        const bool ok = engine.Initialize(dir);
        const double ms =
            std::chrono::duration<double, std::milli>(clock::now() - started)
                .count();
        std::printf("%s ok=%d %.1f ms\n", tag, ok ? 1 : 0, ms);
        return ok;
    };

    // 第一遍：无快照或快照失效 -> 传统装载，并在发布后生成快照。
    if (!run_once("load1(legacy-or-snapshot)")) return 1;
    // 第二遍：上一遍已生成/刷新快照 -> 映射即就绪。
    if (!run_once("load2(snapshot)")) return 1;
    return 0;
}
