#pragma once

#include "candidate.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace shuru {

struct MixedInputSegment {
    std::string text;
    bool literal = false;
};

// 大写字母是原样英文片段，小写字母和撇号是拼音片段。
// 返回 false 表示输入中没有大写字母，应走普通拼音查询。
bool ParseMixedInput(
    const std::string& raw_input,
    std::vector<MixedInputSegment>* segments);

bool IsCalculatorInput(const std::string& raw_input) noexcept;
bool TryEvaluateCalculator(
    const std::string& raw_input,
    std::wstring* formatted_result) noexcept;

std::vector<Candidate> BuildCalculatorCandidates(const std::string& raw_input);
std::vector<Candidate> BuildTimeCandidates(
    const std::string& raw_input,
    const SYSTEMTIME& local_time);
std::vector<Candidate> BuildCurrentTimeCandidates(const std::string& raw_input);

}  // namespace shuru
