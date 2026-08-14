#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shuru {

// 只读系统字符语言模型。数据由固定版本的系统短语词典离线生成，运行时
// 只在词边界查询二元/三元字符频次，不参与候选召回。
class SystemLanguageModel {
public:
    bool LoadFromFile(const std::wstring& path);
    // 返回追加 next 后新增字符 n-gram 的得分。同一完整文本无论如何分词，
    // 每个二元/三元组都只计一次，避免多切词路径重复获益。
    double AppendScore(const std::wstring& prefix,
                       const std::wstring& next) const;

    bool empty() const { return bigrams_.empty() && trigrams_.empty(); }
    size_t bigram_size() const { return bigrams_.size(); }
    size_t trigram_size() const { return trigrams_.size(); }

private:
    struct BigramRecord {
        std::uint32_t key = 0;
        std::uint32_t count = 0;
    };
    struct TrigramRecord {
        std::uint64_t key = 0;
        std::uint32_t count = 0;
    };

    std::vector<BigramRecord> bigrams_;
    std::vector<TrigramRecord> trigrams_;

    std::uint32_t BigramCount(wchar_t first, wchar_t second) const;
    std::uint32_t TrigramCount(
        wchar_t first, wchar_t second, wchar_t third) const;
};

}  // namespace shuru
