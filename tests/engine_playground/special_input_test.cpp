#include "engine/pinyin_correction.h"
#include "engine/special_input.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "failed at line " << __LINE__ << ": " #condition "\n"; \
            return 1; \
        } \
    } while (false)

bool HasCorrection(
    const std::vector<shuru::PinyinCorrection>& corrections,
    const std::string& expected) {
    return std::any_of(corrections.begin(), corrections.end(), [&](const auto& correction) {
        return correction.pinyin == expected;
    });
}

const shuru::PinyinCorrection* FindCorrection(
    const std::vector<shuru::PinyinCorrection>& corrections,
    const std::string& expected) {
    const auto found = std::find_if(
        corrections.begin(), corrections.end(), [&](const auto& correction) {
            return correction.pinyin == expected;
        });
    return found == corrections.end() ? nullptr : &*found;
}

}  // namespace

int main() {
    using namespace shuru;

    std::vector<MixedInputSegment> segments;
    CHECK(ParseMixedInput("duolaAmeng", &segments));
    CHECK(segments.size() == 3);
    CHECK(segments[0].text == "duola" && !segments[0].literal);
    CHECK(segments[1].text == "A" && segments[1].literal);
    CHECK(segments[2].text == "meng" && !segments[2].literal);
    CHECK(!ParseMixedInput("duolameng", &segments));

    std::wstring value;
    CHECK(TryEvaluateCalculator("v1+2*3", &value) && value == L"7");
    CHECK(TryEvaluateCalculator("v(1+2)*3", &value) && value == L"9");
    CHECK(TryEvaluateCalculator("v-7/2", &value) && value == L"-3.5");
    CHECK(TryEvaluateCalculator("v.5+.25", &value) && value == L"0.75");
    CHECK(!TryEvaluateCalculator("v1/0", &value));
    CHECK(!TryEvaluateCalculator("v1+", &value));
    CHECK(!TryEvaluateCalculator("v1  +2", &value));
    CHECK(!TryEvaluateCalculator("v" + std::string(65, '1'), &value));

    SYSTEMTIME fixed {};
    fixed.wYear = 2026;
    fixed.wMonth = 8;
    fixed.wDay = 12;
    fixed.wHour = 19;
    fixed.wMinute = 30;
    fixed.wSecond = 25;
    fixed.wDayOfWeek = 3;
    const auto time_candidates = BuildTimeCandidates("sj", fixed);
    CHECK(time_candidates.size() == 4);
    CHECK(time_candidates[0].text == L"2026-08-12 19:30:25");
    CHECK(time_candidates[1].text == L"2026年8月12日");
    CHECK(time_candidates[2].text == L"19:30:25");
    CHECK(time_candidates[3].text == L"星期三");
    CHECK(std::all_of(time_candidates.begin(), time_candidates.end(), [](const Candidate& candidate) {
        return !candidate.learnable && candidate.source == CandidateSource::Dynamic;
    }));

    const auto corrections = GeneratePinyinCorrections("zehhsigejuicuo");
    CHECK(corrections.size() <= PinyinCorrectionLimits{}.max_results);
    if (!HasCorrection(corrections, "zheshigejiucuo")) {
        for (const auto& correction : corrections)
            std::cerr << correction.pinyin << " cost=" << correction.cost << '\n';
    }
    CHECK(HasCorrection(corrections, "zheshigejiucuo"));
    const auto* typo = FindCorrection(corrections, "zheshigejiucuo");
    const auto* deletion = FindCorrection(corrections, "zheshigejucuo");
    CHECK(typo != nullptr && deletion != nullptr);
    CHECK(typo->cost == deletion->cost);
    CHECK(typo->ranking_cost < deletion->ranking_cost);
    PinyinCorrectionLimits interactive_limits;
    interactive_limits.max_states_per_position = 32;
    interactive_limits.max_results = 4;
    CHECK(HasCorrection(
        GeneratePinyinCorrections("zehhsigejuicuo", interactive_limits),
        "zheshigejiucuo"));
    CHECK(HasCorrection(
        GeneratePinyinCorrections("chognqi", interactive_limits), "chongqi"));
    CHECK(HasCorrection(GeneratePinyinCorrections("zeh"), "zhe"));
    CHECK(HasCorrection(GeneratePinyinCorrections("hsi"), "shi"));
    CHECK(HasCorrection(GeneratePinyinCorrections("jui"), "jiu"));
    CHECK(HasCorrection(GeneratePinyinCorrections("zhhe"), "zhe"));
    CHECK(HasCorrection(GeneratePinyinCorrections("chognqi"), "chongqi"));
    CHECK(GeneratePinyinCorrections(std::string(33, 'a')).empty());
    CHECK(GeneratePinyinCorrections("has space").empty());

    std::cout << "special_input: OK\n";
    return 0;
}
