#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shuru {

// 只读系统语言模型。优先加载 Rime Grammar 双数组模型；旧的 CSNGRM1
// 二元/三元字符模型仍可读取，供旧数据包回退。
class SystemLanguageModel {
public:
    bool LoadFromFile(const std::wstring& path);
    // 返回追加 next 后新增字符 n-gram 的得分。同一完整文本无论如何分词，
    // 每个二元/三元组都只计一次，避免多切词路径重复获益。
    double AppendScore(const std::wstring& prefix,
                       const std::wstring& next,
                       bool is_rear = false) const;

    bool empty() const {
        return !grammar_ && bigrams_.empty() && trigrams_.empty();
    }
    bool is_grammar() const { return grammar_ != nullptr; }
    size_t grammar_unit_count() const;
    size_t mapped_bytes() const;
    size_t bigram_size() const { return bigrams_.size(); }
    size_t trigram_size() const { return trigrams_.size(); }

private:
    struct GrammarMapping;

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
    std::shared_ptr<const GrammarMapping> grammar_;

    static std::shared_ptr<const GrammarMapping> LoadGrammarMapping(
        const std::wstring& path);
    static bool LoadLegacyModel(
        const std::wstring& path,
        std::vector<BigramRecord>* bigrams,
        std::vector<TrigramRecord>* trigrams);

    std::uint32_t BigramCount(wchar_t first, wchar_t second) const;
    std::uint32_t TrigramCount(
        wchar_t first, wchar_t second, wchar_t third) const;
    double QueryGrammar(const std::wstring& context,
                        const std::wstring& word,
                        bool is_rear) const;
};

}  // namespace shuru
