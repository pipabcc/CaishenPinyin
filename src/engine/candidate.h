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
    bool learnable = true;    // 动态结果和中英混输不写入用户词库
    size_t covered_input_len = 0; // candidate-local raw input coverage
    size_t segment_count = 1;
    double ranking_score = 0.0;
    CandidateSource source = CandidateSource::Raw;
};

struct EngineQueryResult {
    std::vector<Candidate> candidates;
    // Legacy compatibility: maximum candidate coverage; commits use Candidate coverage.
    size_t matched_pinyin_len = 0;
};

} // namespace shuru
