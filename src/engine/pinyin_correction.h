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
    int cost = 0;
    std::size_t syllable_count = 0;
};

std::vector<PinyinCorrection> GeneratePinyinCorrections(
    const std::string& input,
    const PinyinCorrectionLimits& limits = {});

}  // namespace shuru
