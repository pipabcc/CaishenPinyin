#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/com_utils.h"

namespace shuru {

// 用户二元搭配模型：记录「上一个上屏词 -> 下一个上屏词」的选择次数。
// 供整句转换路径加权（发财 -> 暴富）与上屏后联想使用。模型只增不删，
// 由每前词后继上限与总量上限约束内存；快照以 shared_ptr 发布，写时复制。
class UserBigramModel {
public:
    struct Successor {
        std::wstring text;
        int count = 0;
        std::int64_t last_used_unix = 0;
    };

    static constexpr size_t kMaxSuccessorsPerWord = 16;
    static constexpr size_t kMaxEntries = 4096;

    void Observe(const std::wstring& previous, const std::wstring& next,
                 std::int64_t now_unix) {
        if (previous.empty() || next.empty()) return;
        if (previous.size() > 16 || next.size() > 16) return;
        auto& successors = map_[previous];
        for (auto& item : successors) {
            if (item.text == next) {
                ++item.count;
                item.last_used_unix = now_unix;
                dirty_ = true;
                return;
            }
        }
        if (map_.size() > kMaxEntries) return;  // 总量已满：只强化已有搭配
        if (successors.size() >= kMaxSuccessorsPerWord) {
            // 淘汰计数最低且最久未用的一项
            auto weakest = successors.begin();
            for (auto it = successors.begin(); it != successors.end(); ++it) {
                if (it->count < weakest->count ||
                    (it->count == weakest->count &&
                     it->last_used_unix < weakest->last_used_unix)) {
                    weakest = it;
                }
            }
            successors.erase(weakest);
        }
        successors.push_back({next, 1, now_unix});
        dirty_ = true;
    }

    int Count(const std::wstring& previous, const std::wstring& next) const {
        if (previous.empty() || next.empty() || map_.empty()) return 0;
        const auto found = map_.find(previous);
        if (found == map_.end()) return 0;
        for (const auto& item : found->second) {
            if (item.text == next) return item.count;
        }
        return 0;
    }

    std::vector<Successor> Successors(const std::wstring& previous, size_t limit) const {
        std::vector<Successor> out;
        const auto found = map_.find(previous);
        if (found == map_.end()) return out;
        out = found->second;
        std::sort(out.begin(), out.end(), [](const Successor& a, const Successor& b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.last_used_unix != b.last_used_unix) return a.last_used_unix > b.last_used_unix;
            return a.text < b.text;
        });
        if (out.size() > limit) out.resize(limit);
        return out;
    }

    bool empty() const { return map_.empty(); }
    bool dirty() const { return dirty_; }
    void clear_dirty() const { dirty_ = false; }

    bool LoadFromFile(const std::wstring& path) {
        std::ifstream in{std::filesystem::path(path)};
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t first = line.find('\t');
            if (first == std::string::npos) continue;
            const size_t second = line.find('\t', first + 1);
            if (second == std::string::npos) continue;
            const size_t third = line.find('\t', second + 1);
            const std::wstring previous = Utf8ToWide(line.substr(0, first));
            const std::wstring next = Utf8ToWide(line.substr(first + 1, second - first - 1));
            int count = 0;
            std::int64_t last_used = 0;
            try {
                count = std::stoi(line.substr(second + 1,
                    third == std::string::npos ? std::string::npos : third - second - 1));
                if (third != std::string::npos) last_used = std::stoll(line.substr(third + 1));
            } catch (...) {
                continue;
            }
            if (previous.empty() || next.empty() || count <= 0) continue;
            auto& successors = map_[previous];
            if (successors.size() < kMaxSuccessorsPerWord) {
                successors.push_back({next, count, last_used});
            }
            if (map_.size() > kMaxEntries) break;
        }
        dirty_ = false;
        return true;
    }

    bool SaveToFile(const std::wstring& path) const {
        try {
            const std::filesystem::path target(path);
            if (target.has_parent_path()) {
                std::filesystem::create_directories(target.parent_path());
            }
            const std::filesystem::path temp = target.wstring() +
                L".tmp." + std::to_wstring(GetCurrentProcessId());
            std::ofstream out{temp, std::ios::binary | std::ios::trunc};
            if (!out) return false;
            out << "# user bigram v1: previous<TAB>next<TAB>count<TAB>last_used_unix\n";
            for (const auto& [previous, successors] : map_) {
                const std::string previous_utf8 = WideToUtf8(previous);
                for (const auto& item : successors) {
                    out << previous_utf8 << '\t' << WideToUtf8(item.text) << '\t'
                        << item.count << '\t' << item.last_used_unix << '\n';
                }
            }
            out.flush();
            if (!out) {
                out.close();
                DeleteFileW(temp.c_str());
                return false;
            }
            out.close();
            if (!MoveFileExW(temp.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temp.c_str());
                return false;
            }
            dirty_ = false;
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    std::unordered_map<std::wstring, std::vector<Successor>> map_;
    mutable bool dirty_ = false;
};

}  // namespace shuru
