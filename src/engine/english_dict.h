#pragma once

#include "candidate.h"

#include <string>
#include <vector>

namespace shuru {

// 英文单词词典：大小写不敏感查询，候选保留词库提供的规范大小写。
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
        std::string display;
        int frequency = 0;
    };
    std::vector<Item> words_;  // 按规范化查询键升序，便于 lower_bound

    static std::string Normalize(const std::string& s);
};

}  // namespace shuru
