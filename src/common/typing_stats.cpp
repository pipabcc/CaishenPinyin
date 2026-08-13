#include "typing_stats.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace shuru {
namespace {

constexpr wchar_t kStatsMutexName[] = L"Local\\FacaiPinyin.TypingStats";

struct StatsState {
    std::string local_date;
    std::uint64_t daily_count = 0;
};

class MutexGuard {
public:
    explicit MutexGuard(DWORD timeout_ms) {
        mutex_ = CreateMutexW(nullptr, FALSE, kStatsMutexName);
        if (mutex_ != nullptr) {
            const DWORD wait = WaitForSingleObject(mutex_, timeout_ms);
            owns_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }

    ~MutexGuard() {
        if (owns_) ReleaseMutex(mutex_);
        if (mutex_ != nullptr) CloseHandle(mutex_);
    }

    bool owns() const noexcept { return owns_; }

private:
    HANDLE mutex_ = nullptr;
    bool owns_ = false;
};

std::string LocalDate(std::time_t now) {
    std::tm local {};
    if (localtime_s(&local, &now) != 0) return {};
    char date[16] {};
    if (strftime(date, sizeof(date), "%Y-%m-%d", &local) == 0) return {};
    return date;
}

std::uint64_t ParseUnsigned64(const std::string& value, std::uint64_t fallback = 0) {
    try {
        std::size_t used = 0;
        const auto parsed = std::stoull(value, &used);
        return used == value.size() ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

StatsState ReadState(const std::wstring& path) {
    StatsState state;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const std::string key = line.substr(0, split);
        const std::string value = line.substr(split + 1);
        if (key == "Date") {
            state.local_date = value;
        } else if (key == "Total") {
            state.daily_count = ParseUnsigned64(value);
        }
    }
    return state;
}

void NormalizeState(StatsState* state, std::time_t now) {
    if (state == nullptr) return;
    const std::string today = LocalDate(now);
    if (state->local_date != today) {
        state->local_date = today;
        state->daily_count = 0;
    }
}

TypingStatsSnapshot ToSnapshot(const StatsState& state) {
    return TypingStatsSnapshot {state.daily_count, true};
}

bool WriteState(const std::wstring& path, const StatsState& state) {
    std::error_code error;
    const std::filesystem::path target(path);
    const auto directory = target.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
        if (error) return false;
    }

    const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    {
        std::ofstream output(std::filesystem::path(temporary),
                             std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "# Facai Pinyin daily typing statistics v2\n";
        output << "Date=" << state.local_date << "\n";
        output << "Total=" << state.daily_count << "\n";
        output.flush();
        if (!output) {
            output.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool IsCountableBmp(wchar_t ch) noexcept {
    WORD type = 0;
    return GetStringTypeW(CT_CTYPE1, &ch, 1, &type) != FALSE &&
           (type & (C1_ALPHA | C1_DIGIT)) != 0;
}

bool IsCjkCodePoint(std::uint32_t code_point) noexcept {
    return code_point == 0x3007 ||
           (code_point >= 0x3400 && code_point <= 0x4DBF) ||
           (code_point >= 0x4E00 && code_point <= 0x9FFF) ||
           (code_point >= 0xF900 && code_point <= 0xFAFF) ||
           (code_point >= 0x20000 && code_point <= 0x2EBEF) ||
           (code_point >= 0x30000 && code_point <= 0x323AF);
}

}  // namespace

TypingStatsStore::TypingStatsStore(std::wstring path)
    : path_(path.empty() ? DefaultPath() : std::move(path)) {}

TypingStatsSnapshot TypingStatsStore::Load(std::time_t now) const {
    if (path_.empty()) return {};
    MutexGuard guard(0);
    if (!guard.owns()) return {};
    StatsState state = ReadState(path_);
    NormalizeState(&state, now);
    return ToSnapshot(state);
}

TypingStatsSnapshot TypingStatsStore::Record(
    const std::wstring& committed_text,
    std::time_t now) const {
    if (path_.empty()) return {};
    MutexGuard guard(1000);
    if (!guard.owns()) return {};

    StatsState state = ReadState(path_);
    NormalizeState(&state, now);
    const std::size_t counted = CountCharacters(committed_text);
    if (counted != 0) {
        const auto available = (std::numeric_limits<std::uint64_t>::max)() -
            state.daily_count;
        state.daily_count += (std::min)(
            static_cast<std::uint64_t>(counted), available);
        WriteState(path_, state);
    }
    return ToSnapshot(state);
}

std::size_t TypingStatsStore::CountCharacters(const std::wstring& text) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint32_t first = static_cast<std::uint16_t>(text[index]);
        if (first >= 0xD800 && first <= 0xDBFF && index + 1 < text.size()) {
            const std::uint32_t second = static_cast<std::uint16_t>(text[index + 1]);
            if (second >= 0xDC00 && second <= 0xDFFF) {
                const std::uint32_t code_point =
                    0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                if (IsCjkCodePoint(code_point)) ++count;
                ++index;
                continue;
            }
        }
        if (IsCjkCodePoint(first) || IsCountableBmp(text[index])) ++count;
    }
    return count;
}

std::wstring TypingStatsStore::DefaultPath() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length == 0) return {};
    std::wstring root(static_cast<std::size_t>(length), L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), length);
    if (written == 0 || written >= length) return {};
    root.resize(written);
    return root + L"\\FacaiPinyin\\data\\typing_stats.txt";
}

}  // namespace shuru
