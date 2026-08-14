#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace shuru {

struct PinyinCorrectionLimits {
    std::size_t max_input_length = 32;
    int max_total_cost = 4;
    std::size_t max_states_per_position = 64;
    std::size_t max_results = 24;
};

struct PinyinCorrection {
    std::string pinyin;
    // 原始输入按修正后音节对齐的展示边界，例如 chogn'qi。
    // 查询和学习仍使用 pinyin，界面展示使用该字段。
    std::string input_segmentation;
    int cost = 0;
    std::size_t syllable_count = 0;
};

std::vector<PinyinCorrection> GeneratePinyinCorrections(
    const std::string& input,
    const PinyinCorrectionLimits& limits = {});

}  // namespace shuru
