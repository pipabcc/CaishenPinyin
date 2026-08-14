#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shuru {

enum class CandidateSource : std::uint8_t {
    Exact = 0,
    Prefix = 1,
    WordGraph = 2,
    Jianpin = 3,
    Mixed = 4,
    Fuzzy = 5,
    English = 6,
    Raw = 7,
    Correction = 8,
    Dynamic = 9,
    LiteralMixed = 10,
    CustomPhrase = 11,
    MixedSentence = 12,
};

struct Candidate {
    std::wstring text;
    std::string pinyin;
    int frequency = 0;
    int match_cost = 0;       // 0=精确；越大表示越弱的模糊恢复
    int selection_count = 0;  // 用户明确选择次数
    std::int64_t last_used_unix = 0;
    int learning_score = 0;   // 经时间衰减后的有界、可解释加分
    bool from_user = false;
    bool is_english = false;  // 英文单词候选
    bool learnable = true;    // 动态结果不写入用户词库
    size_t covered_input_len = 0; // candidate-local raw input coverage
    size_t segment_count = 1;
    double ranking_score = 0.0;
    double language_score = 0.0;
    CandidateSource source = CandidateSource::Raw;
    // 与当前候选对应的原始输入音节边界，例如 x'x'li'you。
    // 仅用于组合串展示；学习继续使用规范全拼 pinyin/learn_segments。
    std::string input_segmentation;
    // 中英混输候选整体没有合法拼音串，改为按拼音子段学习：
    // 每项为 (子段全拼, 子段选中文本)。为空时按 (pinyin, text) 整体学习。
    std::vector<std::pair<std::string, std::wstring>> learn_segments;
};

struct EngineQueryResult {
    std::vector<Candidate> candidates;
    // Legacy compatibility: maximum candidate coverage; commits use Candidate coverage.
    size_t matched_pinyin_len = 0;
};

} // namespace shuru
