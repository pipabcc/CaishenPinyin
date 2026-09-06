// 开发期基准：query_benchmark <词库目录> <隔离用户目录> [候选数量]
#include "engine/pinyin_engine.h"
#include "common/typing_stats.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}
double Percentile(const std::vector<double>& samples, double percentile) {
    return samples[static_cast<size_t>((samples.size() - 1) * percentile)];
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    SetConsoleOutputCP(CP_UTF8);
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2])) return 2;
    const size_t limit = argc >= 4 ? static_cast<size_t>(_wtoi(argv[3])) : 10;
    if (limit == 0 || limit > 1024) return 2;
    shuru::PinyinEngine engine;
    if (!engine.Initialize(argv[1])) return 1;
    const std::vector<std::string> inputs = {
        "nihao", "mhu", "srf", "zhongguo", "xian", "z", "sh", "yingw",
        "renz", "renzhen", "womenzhidao", "suixinshuru", "womenzhidaosuixinshuru",
        "zhonghuarenmingongheguo", "jintiantianqihenhaowomenyiqiqugongyuan",
        "wmzdyqxx", "nihaozzzzz", "shurufapeizhi", std::string(48, 'a'), std::string(48, 'z')
    };
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "输入\t候选上限\t中位毫秒\tP95毫秒\t最大毫秒\t工作量\t预算用尽\t索引毫秒\t纠错毫秒\t词图毫秒\t混拼毫秒\t模糊毫秒\t收尾毫秒\n";
    for (const auto& input : inputs) {
        engine.Query(input, limit);
        std::vector<double> samples;
        for (int round = 0; round < 30; ++round) {
            const auto started = Clock::now();
            engine.Query(input, limit);
            samples.push_back(Milliseconds(started));
        }
        std::sort(samples.begin(), samples.end());
        shuru::QueryDiagnostics diagnostics;
        shuru::QueryOptions options;
        options.diagnostics = &diagnostics;
        engine.Query(input, limit, options);
        std::cout << input << '\t' << limit << '\t' << Percentile(samples, .5)
                  << '\t' << Percentile(samples, .95) << '\t' << samples.back()
                  << '\t' << diagnostics.work_used << '\t' << diagnostics.budget_exhausted;
        for (const auto elapsed : diagnostics.milliseconds) std::cout << '\t' << elapsed;
        std::cout << '\n';
    }
    shuru::AsyncTypingStatsRecorder statistics(
        (std::filesystem::path(argv[2]) / L"benchmark-stats.txt").wstring());
    std::vector<double> samples;
    for (int index = 0; index < 200; ++index) {
        const auto started = Clock::now();
        statistics.Record(L"你好世界");
        samples.push_back(Milliseconds(started));
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "统计入队\t200\t" << Percentile(samples, .5) << '\t'
              << Percentile(samples, .95) << '\t' << samples.back() << '\n';
    return statistics.Flush(5000) ? 0 : 1;
}
