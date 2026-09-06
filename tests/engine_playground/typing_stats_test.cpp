#include "common/typing_stats.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <chrono>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "check failed line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (0)

namespace {

std::time_t LocalNoon(int year, int month, int day) {
    std::tm value {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = 12;
    value.tm_isdst = -1;
    return std::mktime(&value);
}

}  // namespace

int wmain() {
    namespace fs = std::filesystem;
    wchar_t temporary_root[MAX_PATH] {};
    CHECK(GetTempPathW(ARRAYSIZE(temporary_root), temporary_root) > 0);
    const fs::path directory = fs::path(temporary_root) /
        (L"facai-typing-stats-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path path = directory / L"typing_stats.txt";
    fs::remove_all(directory);

    const std::wstring countable = L"中文Ab9 ，。 \U00020000\U0001F600";
    CHECK(shuru::TypingStatsStore::CountCharacters(countable) == 6);

    shuru::TypingStatsStore store(path.wstring());
    const std::time_t first = LocalNoon(2026, 8, 13);
    CHECK(first != static_cast<std::time_t>(-1));
    fs::create_directories(directory);
    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "# Facai Pinyin typing statistics v1\n"
               << "Date=2026-08-13\nTotal=2\nBuckets=1:2\n";
    }
    auto snapshot = store.Record(L"中文 A1，!", first);
    CHECK(snapshot.available && snapshot.daily_count == 6);
    snapshot = store.Record(L"b2。", first + 30);
    CHECK(snapshot.daily_count == 8);
    snapshot = store.Load(first + 61);
    CHECK(snapshot.daily_count == 8);

    const std::time_t next_day = LocalNoon(2026, 8, 14);
    snapshot = store.Record(L"新", next_day);
    CHECK(snapshot.daily_count == 1);
    snapshot = store.Record(L" ，。!?", next_day + 1);
    CHECK(snapshot.daily_count == 1);

    std::ifstream input(path, std::ios::binary);
    const std::string persisted {
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(persisted.find("Date=") != std::string::npos);
    CHECK(persisted.find("Total=1") != std::string::npos);
    CHECK(persisted.find("Buckets=") == std::string::npos);
    CHECK(persisted.find("中文") == std::string::npos);

    input.close();

    const fs::path async_path = directory / L"async.txt";
    {
        shuru::AsyncTypingStatsRecorder first_writer(async_path.wstring());
        shuru::AsyncTypingStatsRecorder second_writer(async_path.wstring());
        const auto started = std::chrono::steady_clock::now();
        for (int index = 0; index < 100; ++index) {
            CHECK(first_writer.Record(L"中文", first).available);
            CHECK(second_writer.Record(L"四个汉字", first).available);
        }
        const auto queued_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        CHECK(queued_ms < 1000);
        CHECK(first_writer.Flush(5000));
        CHECK(second_writer.Flush(5000));
        CHECK(shuru::TypingStatsStore(async_path.wstring()).Load(first).daily_count == 600);
        CHECK(first_writer.Record(L"新", next_day).available);
        CHECK(second_writer.Record(L"今天", next_day).available);
    }
    CHECK(shuru::TypingStatsStore(async_path.wstring()).Load(next_day).daily_count == 3);
    // 延迟的昨日批次不得回退已经由另一宿主写入的新日期。
    CHECK(shuru::TypingStatsStore(async_path.wstring()).RecordCount(9, first).available);
    CHECK(shuru::TypingStatsStore(async_path.wstring()).Load(next_day).daily_count == 3);

    const fs::path blocker = directory / L"blocked-parent";
    { std::ofstream file(blocker); file << "阻止创建子目录"; }
    const fs::path retry_path = blocker / L"stats.txt";
    {
        shuru::AsyncTypingStatsRecorder retry(retry_path.wstring());
        CHECK(retry.Record(L"重试保存", first).daily_count == 4);
        CHECK(!retry.Flush(50));
        fs::remove(blocker);
        CHECK(retry.Flush(5000));
    }
    CHECK(shuru::TypingStatsStore(retry_path.wstring()).Load(first).daily_count == 4);
    fs::remove_all(directory);
    std::cout << "typing_stats: OK\n";
    return 0;
}
