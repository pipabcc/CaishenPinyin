#pragma once

#include <cstdint>
#include <ctime>
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

    static std::size_t CountCharacters(const std::wstring& text) noexcept;
    static std::wstring DefaultPath();

private:
    std::wstring path_;
};

}  // namespace shuru
