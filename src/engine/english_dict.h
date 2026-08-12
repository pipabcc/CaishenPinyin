#pragma once

#include "candidate.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace shuru {

// 英文单词词典：前缀检索（hello / hel...）
class EnglishDictionary {
public:
    bool LoadFromFile(const std::wstring& path);
    bool empty() const { return words_.empty(); }
    size_t Size() const { return words_.size(); }

    // 精确匹配
    std::vector<Candidate> LookupExact(const std::string& word) const;
    // 前缀匹配，按词频排序
    std::vector<Candidate> LookupPrefix(const std::string& prefix, size_t limit = 32) const;

private:
    struct Item {
        std::string word;
        int frequency = 0;
    };
    std::vector<Item> words_;  // 按 word 升序，便于 lower_bound
    std::unordered_map<std::string, int> freq_;

    static std::string Normalize(const std::string& s);
};

}  // namespace shuru
