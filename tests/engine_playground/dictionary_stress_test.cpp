#include "engine/pinyin_engine.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool CreateStressLexicon(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }
    std::ofstream output(directory / L"base_dict.txt", std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    constexpr char kEntry[] = "ni\t\xE4\xBD\xA0\t100\n";
    for (int i = 0; i < 400000; ++i) {
        output.write(kEntry, sizeof(kEntry) - 1);
    }
    constexpr char kFuzzyEntry[] = "mohu\t\xE6\xA8\xA1\xE7\xB3\x8A\t100\n";
    output.write(kFuzzyEntry, sizeof(kFuzzyEntry) - 1);
    return static_cast<bool>(output);
}

bool VerifyConcurrentQueryLatency(shuru::PinyinEngine& engine) {
    constexpr int kThreadCount = 8;
    constexpr int kQueriesPerThread = 250;
    std::atomic<bool> start {false};
    std::atomic<int> failures {0};
    std::atomic<long long> maximum_latency_us {0};
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
        workers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                SwitchToThread();
            }
            for (int i = 0; i < kQueriesPerThread; ++i) {
                const auto started = std::chrono::steady_clock::now();
                const auto result = engine.Query("ni", 9);
                const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
                long long observed = maximum_latency_us.load(std::memory_order_relaxed);
                while (latency > observed &&
                       !maximum_latency_us.compare_exchange_weak(
                           observed, latency, std::memory_order_relaxed)) {
                }
                if (result.candidates.empty() || result.candidates.front().text != L"你") {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }

    const long long maximum_ms = (maximum_latency_us.load() + 999) / 1000;
    std::cout << "concurrent query maximum latency: " << maximum_ms << " ms\n";
    // 捕获整批串行化或明显停顿，同时给繁忙桌面/CI 的线程调度留出余量。
    return failures.load() == 0 && maximum_ms < 250;
}

bool VerifyFailedReloadKeepsPublishedSnapshot(
    shuru::PinyinEngine& engine,
    const std::filesystem::path& missing_lexicon) {
    if (engine.Initialize(missing_lexicon.wstring())) {
        std::cerr << "missing lexicon unexpectedly initialized\n";
        return false;
    }
    const auto result = engine.Query("ni", 1);
    return engine.IsReady() && !result.candidates.empty() &&
           result.candidates.front().text == L"你";
}

bool VerifyLearningDuringConcurrentQueries(shuru::PinyinEngine& engine) {
    std::atomic<bool> stop {false};
    std::atomic<int> failures {0};
    std::atomic<long long> maximum_learn_latency_us {0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                const auto result = engine.Query("ni", 9);
                if (result.candidates.empty()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 一音节只能学习一个汉字；多字样本会被标准拼音结构校验正确过滤。
    constexpr wchar_t kLearnedWord[] = L"拟";
    for (int i = 0; i < 20; ++i) {
        const auto started = std::chrono::steady_clock::now();
        engine.Learn("ni", kLearnedWord);
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        long long observed = maximum_learn_latency_us.load(std::memory_order_relaxed);
        while (latency > observed &&
               !maximum_learn_latency_us.compare_exchange_weak(
                   observed, latency, std::memory_order_relaxed)) {
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    const auto learned = engine.Query("ni", 9);
    bool found = false;
    for (const auto& candidate : learned.candidates) {
        found = found || candidate.text == kLearnedWord;
    }
    const long long maximum_ms = (maximum_learn_latency_us.load() + 999) / 1000;
    std::cout << "learning maximum latency with active readers: " << maximum_ms << " ms\n";
    // 压力词库有 40 万行；若误复制基础快照，学习延迟会明显越过此上限。
    return failures.load() == 0 && found && maximum_ms < 100;
}

bool VerifyQueryOptionSnapshots(shuru::PinyinEngine& engine) {
    shuru::QueryOptions fuzzy;
    fuzzy.schema = shuru::InputSchema::Quanpin;
    fuzzy.fuzzy_enabled = true;
    shuru::QueryOptions strict = fuzzy;
    strict.fuzzy_enabled = false;
    std::atomic<int> failures {0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t]() {
            const auto& options = (t & 1) ? fuzzy : strict;
            for (int i = 0; i < 200; ++i) {
                const auto result = engine.Query("mhu", 9, options);
                bool found = false;
                for (const auto& candidate : result.candidates) found = found || candidate.text == L"模糊";
                if (found != options.fuzzy_enabled) failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    std::cout << "query option snapshot failures: " << failures.load() << "\n";
    return failures.load() == 0;
}

bool VerifyBaseWordLearningIncreasesWeight(shuru::PinyinEngine& engine) {
    const auto before = engine.Query("ni", 9);
    int before_frequency = -1;
    for (const auto& candidate : before.candidates) {
        if (candidate.text == L"你") {
            before_frequency = candidate.frequency;
            break;
        }
    }
    if (before_frequency < 0) {
        return false;
    }

    engine.Learn("ni", L"你");
    const auto after = engine.Query("ni", 9);
    for (const auto& candidate : after.candidates) {
        if (candidate.text == L"你") {
            return candidate.from_user && candidate.frequency >= before_frequency + 20;
        }
    }
    return false;
}

}  // namespace

int wmain() {
    wchar_t temp_path[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp_path) == 0) {
        return 1;
    }
    const std::filesystem::path root =
        std::filesystem::path(temp_path) /
        (L"FacaiDictionaryStress-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path lexicon = root / L"lexicon";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (!CreateStressLexicon(lexicon)) {
        return 2;
    }

    // 每个进程使用独立用户目录，避免历史用户词频和旧 ACL 污染压力基线。
    const std::filesystem::path local_app_data = root / L"local-app-data";
    std::filesystem::create_directories(local_app_data, error);
    if (error || !SetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.c_str())) return 10;

    {
        shuru::PinyinEngine engine;
        if (!engine.Initialize(lexicon.wstring())) {
            return 3;
        }
        const auto candidates = engine.Query("ni", 1).candidates;
        if (candidates.empty() || candidates.front().text != L"你") {
            return 4;
        }
        if (!VerifyConcurrentQueryLatency(engine)) {
            return 5;
        }
        if (!VerifyFailedReloadKeepsPublishedSnapshot(engine, root / L"missing")) {
            return 6;
        }
        if (!VerifyLearningDuringConcurrentQueries(engine)) {
            return 7;
        }
        if (!VerifyBaseWordLearningIncreasesWeight(engine)) {
            return 8;
        }
        if (!VerifyQueryOptionSnapshots(engine)) {
            return 9;
        }
    }

    std::filesystem::remove_all(root, error);
    std::wcout << L"dictionary_stress: OK\n";
    return 0;
}
