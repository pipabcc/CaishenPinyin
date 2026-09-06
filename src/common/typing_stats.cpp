#include "typing_stats.h"

#include "user_data_paths.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace shuru {
namespace {

constexpr wchar_t kStatsMutexName[] = L"Local\\CaishenPinyin.TypingStats";

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
        output << "# Caishen Pinyin daily typing statistics v2\n";
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
    return RecordCount(CountCharacters(committed_text), now);
}

TypingStatsSnapshot TypingStatsStore::RecordCount(
    std::uint64_t counted, std::time_t now) const {
    if (path_.empty()) return {};
    MutexGuard guard(1000);
    if (!guard.owns()) return {};

    StatsState state = ReadState(path_);
    // 跨午夜的延迟批次不能把其他进程已经保存的新一天计数覆盖回昨天。
    if (state.local_date > LocalDate(now)) return {0, true};
    NormalizeState(&state, now);
    if (counted != 0) {
        const auto available = (std::numeric_limits<std::uint64_t>::max)() -
            state.daily_count;
        state.daily_count += (std::min)(
            static_cast<std::uint64_t>(counted), available);
        if (!WriteState(path_, state)) return {};
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
    return CaishenUserDataPath(L"data\\typing_stats.txt");
}

struct AsyncTypingStatsRecorder::State {
    struct Day {
        std::time_t time = 0;
        std::uint64_t pending = 0;
        std::uint64_t visible = 0;
        bool available = false;
    };
    struct Batch {
        std::string date;
        std::time_t time;
        std::uint64_t count;
    };

    explicit State(std::wstring path) : store(std::move(path)) {
        worker = std::thread([this] { Run(); });
    }

    ~State() {
        {
            std::lock_guard<std::mutex> guard(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    static std::uint64_t Add(std::uint64_t left, std::uint64_t right) {
        return left + (std::min)(right,
            (std::numeric_limits<std::uint64_t>::max)() - left);
    }

    bool HasPending() const {
        return std::any_of(days.begin(), days.end(), [](const auto& entry) {
            return entry.second.pending != 0;
        });
    }

    void Publish(const std::string& date, const TypingStatsSnapshot& saved) {
        if (!saved.available) return;
        auto& day = days[date];
        day.visible = Add(saved.daily_count, day.pending);
        day.available = true;
    }

    void Run() noexcept {
        try {
            for (;;) {
                std::vector<Batch> batches;
                std::uint64_t revision = 0;
                std::time_t observed_time = 0;
                bool should_stop = false;
                {
                    std::unique_lock<std::mutex> guard(mutex);
                    wake.wait(guard, [this] {
                        return stopping || refresh_requested || HasPending();
                    });
                    if (HasPending() && !flush_requested && !stopping) {
                        wake.wait_for(guard, std::chrono::milliseconds(150), [this] {
                            return flush_requested || stopping;
                        });
                    }
                    batches.reserve(days.size());
                    for (auto& entry : days) {
                        auto& day = entry.second;
                        if (day.pending == 0) continue;
                        batches.push_back({entry.first, day.time, day.pending});
                        day.pending = 0;
                    }
                    revision = requested_revision;
                    observed_time = refresh_time;
                    refresh_requested = false;
                    flush_requested = false;
                    should_stop = stopping;
                }

                bool succeeded = true;
                if (batches.empty() && should_stop) return;
                if (batches.empty()) {
                    const auto loaded = store.Load(observed_time);
                    std::lock_guard<std::mutex> guard(mutex);
                    Publish(LocalDate(observed_time), loaded);
                }
                for (const auto& batch : batches) {
                    const auto saved = store.RecordCount(batch.count, batch.time);
                    std::lock_guard<std::mutex> guard(mutex);
                    if (saved.available) {
                        Publish(batch.date, saved);
                    } else {
                        auto& day = days[batch.date];
                        day.pending = Add(day.pending, batch.count);
                        succeeded = false;
                    }
                }
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    if (succeeded) completed_revision = revision;
                    // 只保留近期日期；未落盘的旧批次必须留下以便重试。
                    while (days.size() > 2 && days.begin()->second.pending == 0)
                        days.erase(days.begin());
                }
                settled.notify_all();
                if (should_stop) {
                    if (!succeeded)
                        OutputDebugStringW(L"Caishen typing statistics final save failed\n");
                    return;
                }
                if (!succeeded) {
                    std::unique_lock<std::mutex> guard(mutex);
                    wake.wait_for(guard, std::chrono::milliseconds(250), [this] {
                        return stopping;
                    });
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> guard(mutex);
                worker_failed = true;
            }
            settled.notify_all();
            OutputDebugStringW(L"Caishen typing statistics worker failed\n");
        }
    }

    TypingStatsStore store;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable settled;
    std::map<std::string, Day> days;
    std::uint64_t requested_revision = 0;
    std::uint64_t completed_revision = 0;
    ULONGLONG last_refresh_tick = 0;
    std::time_t refresh_time = std::time(nullptr);
    bool refresh_requested = true;
    bool flush_requested = false;
    bool stopping = false;
    bool worker_failed = false;
    std::thread worker;
};

AsyncTypingStatsRecorder::AsyncTypingStatsRecorder(std::wstring path)
    : state_(std::make_unique<State>(std::move(path))) {}

AsyncTypingStatsRecorder::~AsyncTypingStatsRecorder() = default;

TypingStatsSnapshot AsyncTypingStatsRecorder::Record(
    const std::wstring& text, std::time_t now) {
    const auto count = TypingStatsStore::CountCharacters(text);
    if (count == 0) return Snapshot(now);
    const auto date = LocalDate(now);
    if (date.empty()) return {};
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (state_->worker_failed || state_->stopping) return {};
    auto& day = state_->days[date];
    day.time = now;
    day.pending = State::Add(day.pending, count);
    day.visible = State::Add(day.visible, count);
    day.available = true;
    ++state_->requested_revision;
    state_->wake.notify_one();
    return {day.visible, day.available};
}

TypingStatsSnapshot AsyncTypingStatsRecorder::Snapshot(std::time_t now) {
    const auto date = LocalDate(now);
    std::lock_guard<std::mutex> guard(state_->mutex);
    const auto tick = GetTickCount64();
    if (state_->last_refresh_tick == 0 ||
        tick - state_->last_refresh_tick >= 1000) {
        state_->last_refresh_tick = tick;
        state_->refresh_time = now;
        state_->refresh_requested = true;
        state_->wake.notify_one();
    }
    const auto found = state_->days.find(date);
    return found == state_->days.end() ? TypingStatsSnapshot{}
        : TypingStatsSnapshot{found->second.visible, found->second.available};
}

bool AsyncTypingStatsRecorder::Flush(unsigned long timeout_ms) {
    std::unique_lock<std::mutex> guard(state_->mutex);
    const auto revision = state_->requested_revision;
    state_->flush_requested = true;
    state_->wake.notify_one();
    const bool completed = state_->settled.wait_for(
        guard, std::chrono::milliseconds(timeout_ms), [&] {
            return state_->completed_revision >= revision || state_->worker_failed;
        });
    return completed && !state_->worker_failed &&
        state_->completed_revision >= revision;
}

namespace {
struct RecorderCache {
    SRWLOCK lock = SRWLOCK_INIT;
    std::shared_ptr<AsyncTypingStatsRecorder> recorder;
};
alignas(RecorderCache) unsigned char g_recorder_storage[sizeof(RecorderCache)];

RecorderCache& SharedRecorderCache() {
    // 进程退出不执行包含线程的静态析构；正常 DLL 卸载由显式收尾路径回收。
    static auto* cache = new (g_recorder_storage) RecorderCache;
    return *cache;
}

std::shared_ptr<AsyncTypingStatsRecorder> GetAsyncRecorder() {
    auto& cache = SharedRecorderCache();
    AcquireSRWLockShared(&cache.lock);
    auto recorder = cache.recorder;
    ReleaseSRWLockShared(&cache.lock);
    if (recorder) return recorder;
    AcquireSRWLockExclusive(&cache.lock);
    try {
        if (!cache.recorder) cache.recorder = std::make_shared<AsyncTypingStatsRecorder>();
        recorder = cache.recorder;
    } catch (...) {
        OutputDebugStringW(L"Caishen typing statistics worker unavailable\n");
    }
    ReleaseSRWLockExclusive(&cache.lock);
    return recorder;
}
}  // namespace

TypingStatsSnapshot RecordTypingStatsAsync(const std::wstring& text) {
    static const bool sandbox = IsCurrentProcessAppContainer();
    if (sandbox) return {};
    try {
        auto recorder = GetAsyncRecorder();
        return recorder ? recorder->Record(text) : TypingStatsSnapshot{};
    } catch (...) {
        OutputDebugStringW(L"Caishen typing statistics enqueue failed\n");
        return {};
    }
}

TypingStatsSnapshot LoadTypingStatsAsync() {
    static const bool sandbox = IsCurrentProcessAppContainer();
    if (sandbox) return {};
    try {
        auto recorder = GetAsyncRecorder();
        return recorder ? recorder->Snapshot() : TypingStatsSnapshot{};
    } catch (...) {
        OutputDebugStringW(L"Caishen typing statistics snapshot failed\n");
        return {};
    }
}

bool TryShutdownAsyncTypingStats() {
    auto& cache = SharedRecorderCache();
    AcquireSRWLockShared(&cache.lock);
    auto recorder = cache.recorder;
    ReleaseSRWLockShared(&cache.lock);
    if (!recorder) return true;
    // 等待文件操作时不持有缓存锁，新激活的 TSF 线程仍可立即入队。
    if (!recorder->Flush()) return false;
    AcquireSRWLockExclusive(&cache.lock);
    if (cache.recorder == recorder && recorder.use_count() == 2 && recorder->Flush(0))
        cache.recorder.reset();
    const bool stopped = !cache.recorder;
    ReleaseSRWLockExclusive(&cache.lock);
    return stopped;
}

}  // namespace shuru
