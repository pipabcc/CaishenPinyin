#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

namespace shuru {

struct TypingStatsSnapshot {
    std::uint64_t daily_count = 0;
    bool available = false;
};

class TypingStatsStore {
public:
    explicit TypingStatsStore(std::wstring path = {});

    TypingStatsSnapshot Load(std::time_t now = std::time(nullptr)) const;
    TypingStatsSnapshot Record(
        const std::wstring& committed_text,
        std::time_t now = std::time(nullptr)) const;
    TypingStatsSnapshot RecordCount(
        std::uint64_t count,
        std::time_t now = std::time(nullptr)) const;

    static std::size_t CountCharacters(const std::wstring& text) noexcept;
    static std::wstring DefaultPath();

private:
    std::wstring path_;
};

// 后台仅接收日期和数量，输入正文不会进入队列。
class AsyncTypingStatsRecorder {
public:
    explicit AsyncTypingStatsRecorder(std::wstring path = {});
    ~AsyncTypingStatsRecorder();
    AsyncTypingStatsRecorder(const AsyncTypingStatsRecorder&) = delete;
    AsyncTypingStatsRecorder& operator=(const AsyncTypingStatsRecorder&) = delete;

    TypingStatsSnapshot Record(
        const std::wstring& text, std::time_t now = std::time(nullptr));
    TypingStatsSnapshot Snapshot(std::time_t now = std::time(nullptr));
    bool Flush(unsigned long timeout_ms = 2000);

private:
    struct State;
    std::unique_ptr<State> state_;
};

TypingStatsSnapshot RecordTypingStatsAsync(const std::wstring& text);
TypingStatsSnapshot LoadTypingStatsAsync();
// 只能在 Loader Lock 外调用；失败时保留队列，让 DLL 暂缓卸载并重试。
bool TryShutdownAsyncTypingStats();

}  // namespace shuru
