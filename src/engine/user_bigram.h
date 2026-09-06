#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/com_utils.h"
#include "../common/private_acl.h"
#include "../common/user_data_file.h"

namespace shuru {

class UserBigramModel {
public:
    struct Successor {
        std::wstring text;
        int count = 0;
        std::int64_t last_used_unix = 0;
    };
    struct Change {
        int count = 0;
        std::int64_t time = 0;
    };
    using Key = std::pair<std::wstring, std::wstring>;
    using Changes = std::map<Key, Change>;
    static constexpr size_t kMaxSuccessorsPerWord = 16;
    static constexpr size_t kMaxEntries = 4096;

    bool Observe(const std::wstring& previous, const std::wstring& next, std::int64_t time) {
        if (!Apply(previous, next, 1, time)) return false;
        auto& change = pending_[{previous, next}];
        change.count = Add(change.count, 1);
        change.time = (std::max)(change.time, time);
        return true;
    }
    bool CanObserve(const std::wstring& previous, const std::wstring& next) const {
        return ValidWord(previous) && ValidWord(next) &&
            (map_.size() < kMaxEntries || map_.find(previous) != map_.end());
    }

    int Count(const std::wstring& previous, const std::wstring& next) const {
        const auto found = map_.find(previous);
        if (found == map_.end()) return 0;
        for (const auto& item : found->second) if (item.text == next) return item.count;
        return 0;
    }

    std::vector<Successor> Successors(const std::wstring& previous, size_t limit) const {
        const auto found = map_.find(previous);
        if (found == map_.end()) return {};
        auto out = found->second;
        std::sort(out.begin(), out.end(), [](const Successor& a, const Successor& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.last_used_unix != b.last_used_unix) return a.last_used_unix > b.last_used_unix;
            return a.text < b.text;
        });
        if (out.size() > limit) out.resize(limit);
        return out;
    }

    bool empty() const { return map_.empty(); }
    size_t size() const { return map_.size(); }
    size_t pending_size() const { return pending_.size(); }
    bool dirty() const { return !pending_.empty() || needs_migration_; }
    void clear_dirty() { pending_.clear(); needs_migration_ = false; }
    const std::string& generation() const { return generation_; }
    bool stale_generation() const { return stale_generation_; }
    const UserDataFileStamp& file_stamp() const { return file_stamp_; }

    void BindGeneration(const std::string& generation, bool allow_legacy) {
        if (generation_ != generation && !(generation_.empty() && allow_legacy)) {
            map_.clear();
            pending_.clear();
        }
        needs_migration_ = generation_.empty() && allow_legacy && !map_.empty();
        allow_legacy_ = allow_legacy;
        generation_ = generation;
    }

    bool LoadFromFile(const std::wstring& path) {
        std::string text;
        UserDataFileStamp stamp;
        if (!ReadUserDataFile(path, &text, &stamp) || !stamp.present) return false;
        UserBigramModel loaded;
        loaded.file_stamp_ = stamp;
        if (!ParseUserDataGeneration(text, &loaded.generation_)) return false;
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string previous, next, count_text, time_text;
            if (!std::getline(fields, previous, '\t') || !std::getline(fields, next, '\t') ||
                !std::getline(fields, count_text, '\t')) continue;
            std::getline(fields, time_text, '\t');
            try {
                size_t used = 0;
                const int count = std::stoi(count_text, &used);
                if (used != count_text.size() || count <= 0) continue;
                const std::int64_t time = time_text.empty() ? 0 : std::stoll(time_text);
                if (time < 0) continue;
                loaded.Apply(Utf8ToWide(previous), Utf8ToWide(next), count, time, true);
            } catch (const std::exception&) {
                continue;
            }
        }
        *this = std::move(loaded);
        return true;
    }

    bool SaveToFile(const std::wstring& path) {
        UserLearningFileLock lock;
        if (!lock.owns()) return false;
        stale_generation_ = false;
        if (!generation_.empty()) {
            std::string user_text, current_generation;
            UserDataFileStamp stamp;
            const auto user_path = (std::filesystem::path(path).parent_path() / L"user_dict.txt").wstring();
            if (!ReadUserDataFile(user_path, &user_text, &stamp) ||
                !ParseUserDataGeneration(user_text, &current_generation, kBigramGenerationPrefix)) return false;
            if (current_generation.empty() && !ParseUserDataGeneration(user_text, &current_generation)) return false;
            if (current_generation.empty()) current_generation = LegacyUserDataGeneration(stamp);
            if (current_generation != generation_) {
                stale_generation_ = true;
                return false;
            }
        }
        UserBigramModel merged;
        const auto stamp = ReadUserDataFileStamp(path);
        if (!stamp.valid) return false;
        if (stamp.present && !merged.LoadFromFile(path)) return false;
        if (!generation_.empty() && merged.generation_ != generation_ &&
            !(merged.generation_.empty() && allow_legacy_)) merged.map_.clear();
        if (!stamp.present && pending_.empty() && generation_.empty()) merged.map_ = map_;
        merged.generation_ = generation_;
        for (const auto& item : pending_)
            merged.Apply(item.first.first, item.first.second, item.second.count, item.second.time);
        if (!EnsureCurrentUserOnlyPath(path, false) || !merged.WriteSnapshot(path)) return false;
        map_ = std::move(merged.map_);
        file_stamp_ = ReadUserDataFileStamp(path);
        pending_.clear();
        needs_migration_ = false;
        allow_legacy_ = false;
        return true;
    }

    // 引擎串行保护待写元数据；查询线程只读 map_，不会看到计数被原地改写。
    void Acknowledge(const UserBigramModel& captured) {
        if (&captured == this) {
            clear_dirty();
            return;
        }
        for (const auto& item : captured.pending_) {
            auto found = pending_.find(item.first);
            if (found == pending_.end()) continue;
            found->second.count -= (std::min)(found->second.count, item.second.count);
            if (found->second.count == 0) pending_.erase(found);
        }
        if (captured.needs_migration_) needs_migration_ = false;
    }

    void ApplyPendingFrom(const UserBigramModel& source) {
        if (&source == this) return;
        pending_.clear();
        for (const auto& item : source.pending_)
            if (Apply(item.first.first, item.first.second, item.second.count, item.second.time))
                pending_.insert(item);
    }

private:
    static int Add(int left, int right) {
        return left + (std::min)(right, (std::numeric_limits<int>::max)() - left);
    }

    static bool ValidWord(const std::wstring& word) {
        return !word.empty() && word.size() <= 16 &&
            std::none_of(word.begin(), word.end(), [](wchar_t ch) { return ch < L' ' || ch == 127; });
    }

    bool Apply(const std::wstring& previous, const std::wstring& next,
               int count, std::int64_t time, bool absolute = false) {
        if (!ValidWord(previous) || !ValidWord(next) || count <= 0) return false;
        auto found = map_.find(previous);
        if (found == map_.end()) {
            if (map_.size() >= kMaxEntries) return false;
            found = map_.emplace(previous, std::vector<Successor>{}).first;
        }
        auto& successors = found->second;
        for (auto& item : successors) {
            if (item.text != next) continue;
            item.count = absolute ? (std::max)(item.count, count) : Add(item.count, count);
            item.last_used_unix = (std::max)(item.last_used_unix, time);
            return true;
        }
        if (successors.size() >= kMaxSuccessorsPerWord) {
            const auto weakest = std::min_element(successors.begin(), successors.end(),
                [](const Successor& a, const Successor& b) {
                    if (a.count != b.count) return a.count < b.count;
                    if (a.last_used_unix != b.last_used_unix) return a.last_used_unix < b.last_used_unix;
                    return a.text < b.text;
                });
            pending_.erase({previous, weakest->text});
            successors.erase(weakest);
        }
        successors.push_back({next, count, time});
        return true;
    }

    bool WriteSnapshot(const std::wstring& path) const {
        const auto temporary = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetCurrentThreadId());
        std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "# user bigram v2\n";
        if (!generation_.empty()) output << kUserGenerationPrefix << generation_ << '\n';
        for (const auto& entry : map_)
            for (const auto& item : entry.second)
                output << WideToUtf8(entry.first) << '\t' << WideToUtf8(item.text) << '\t'
                       << item.count << '\t' << item.last_used_unix << '\n';
        output.flush();
        const bool written = static_cast<bool>(output);
        output.close();
        if (!written || !MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }

    std::unordered_map<std::wstring, std::vector<Successor>> map_;
    Changes pending_;
    UserDataFileStamp file_stamp_;
    std::string generation_;
    bool needs_migration_ = false;
    bool allow_legacy_ = false;
    bool stale_generation_ = false;
};

}  // namespace shuru
