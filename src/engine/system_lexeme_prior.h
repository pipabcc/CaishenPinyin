#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shuru {

// 只读系统词元先验。它描述“拼音 + 单字/词语”的无上下文常用度，
// 与负责字符上下文关系的 SystemLanguageModel 分工互补。
class SystemLexemePriorModel {
public:
    bool LoadFromFile(const std::wstring& path);
    std::uint32_t Lookup(
        const std::string& pinyin, const std::wstring& word) const;

    bool empty() const { return records_.empty(); }
    size_t size() const { return records_.size(); }

private:
    struct Record {
        std::string pinyin;
        std::wstring word;
        std::uint32_t score = 0;
    };

    std::vector<Record> records_;
};

}  // namespace shuru
