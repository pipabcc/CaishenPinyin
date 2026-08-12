#pragma once

#include "candidate.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace shuru {

struct CustomPhraseEntry {
    std::string code;
    std::wstring phrase;
    std::size_t position = 1;
};

class CustomPhraseDictionary {
public:
    // 文件不存在表示空词典；格式错误的单行会被忽略。
    bool LoadFromFile(const std::wstring& path);
    std::vector<CustomPhraseEntry> LookupExact(const std::string& code) const;
    std::size_t Size() const noexcept { return size_; }

private:
    std::unordered_map<std::string, std::vector<CustomPhraseEntry>> entries_;
    std::size_t size_ = 0;
};

std::wstring GetCustomPhrasePath(const std::wstring& fallback_directory = {});

// 自定义短语按 1-based 位置插入；位置冲突时按文件顺序向后顺延。
void InsertCustomPhraseCandidates(
    const std::vector<CustomPhraseEntry>& phrases,
    const std::string& normalized_code,
    std::size_t covered_input_len,
    std::size_t limit,
    std::vector<Candidate>* candidates);

}  // namespace shuru
