#include "pinyin_correction.h"

#include "pinyin_syllables.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace shuru {
namespace {

struct EditMeasurement {
    int distance = 2;
    int ranking_cost = 4;
};

EditMeasurement MeasureSingleEdit(
    const std::string& left,
    const std::string& right) {
    if (left == right) return {0, 0};
    if (left.size() == right.size()) {
        std::size_t first = 0;
        while (first < left.size() && left[first] == right[first]) ++first;
        if (first + 1 < left.size() &&
            left[first] == right[first + 1] &&
            left[first + 1] == right[first]) {
            std::size_t tail = first + 2;
            while (tail < left.size() && left[tail] == right[tail]) ++tail;
            if (tail == left.size()) return {1, 1};
        }
        std::size_t differences = 0;
        for (std::size_t index = 0; index < left.size(); ++index)
            if (left[index] != right[index]) ++differences;
        return differences == 1 ? EditMeasurement{1, 3} : EditMeasurement{};
    }
    if (left.size() + 1 == right.size()) {
        std::size_t input = 0;
        std::size_t output = 0;
        bool skipped = false;
        while (input < left.size() && output < right.size()) {
            if (left[input] == right[output]) {
                ++input;
                ++output;
            } else if (!skipped) {
                skipped = true;
                ++output;
            } else {
                return {};
            }
        }
        return {1, 2};
    }
    if (right.size() + 1 == left.size()) {
        const EditMeasurement inverse = MeasureSingleEdit(right, left);
        return inverse.distance == 1 ? EditMeasurement{1, 2} : EditMeasurement{};
    }
    return {};
}

struct CorrectionState {
    std::string pinyin;
    int cost = 0;
    int ranking_cost = 0;
    std::size_t syllables = 0;
};

bool BetterState(const CorrectionState& left, const CorrectionState& right) {
    // 纠错输入经常仍能被切成大量短音节。只按编辑距离排序会让
    // ze-ha-si-ge... 之类碎片路径挤掉 zhe-shi-ge...。一个额外音节的
    // 惩罚与局部编辑排序代价共同作用，可保留更接近自然拼音分词的路径。
    const std::size_t left_quality = left.syllables * 2 +
        static_cast<std::size_t>(left.ranking_cost);
    const std::size_t right_quality = right.syllables * 2 +
        static_cast<std::size_t>(right.ranking_cost);
    if (left_quality != right_quality) return left_quality < right_quality;
    if (left.ranking_cost != right.ranking_cost)
        return left.ranking_cost < right.ranking_cost;
    if (left.cost != right.cost) return left.cost < right.cost;
    if (left.syllables != right.syllables) return left.syllables < right.syllables;
    return left.pinyin < right.pinyin;
}

void PruneStates(std::vector<CorrectionState>* states, std::size_t maximum) {
    if (states == nullptr || states->empty()) return;
    std::unordered_map<std::string, CorrectionState> unique;
    unique.reserve(states->size());
    for (auto& state : *states) {
        auto found = unique.find(state.pinyin);
        if (found == unique.end() || BetterState(state, found->second))
            unique[state.pinyin] = std::move(state);
    }
    states->clear();
    states->reserve(unique.size());
    for (auto& item : unique) states->push_back(std::move(item.second));
    std::sort(states->begin(), states->end(), BetterState);
    if (states->size() > maximum) states->resize(maximum);
}

}  // namespace

std::vector<PinyinCorrection> GeneratePinyinCorrections(
    const std::string& input,
    const PinyinCorrectionLimits& limits) {
    if (input.empty() || input.size() > limits.max_input_length ||
        limits.max_total_cost < 1 || limits.max_states_per_position == 0 ||
        limits.max_results == 0) {
        return {};
    }
    if (!std::all_of(input.begin(), input.end(), [](char ch) {
            return ch >= 'a' && ch <= 'z';
        })) {
        return {};
    }

    std::vector<std::vector<CorrectionState>> states(input.size() + 1);
    states[0].push_back({});
    for (std::size_t begin = 0; begin < input.size(); ++begin) {
        PruneStates(&states[begin], limits.max_states_per_position);
        if (states[begin].empty()) continue;
        const std::size_t max_piece = (std::min)(std::size_t{7}, input.size() - begin);
        for (std::size_t length = 1; length <= max_piece; ++length) {
            const std::string piece = input.substr(begin, length);
            struct Transition {
                std::string syllable;
                EditMeasurement edit;
            };
            std::vector<Transition> transitions;
            for (const auto& syllable : pinyin_data::Syllables()) {
                if (std::abs(static_cast<int>(piece.size()) -
                             static_cast<int>(syllable.size())) > 1) {
                    continue;
                }
                const EditMeasurement edit = MeasureSingleEdit(piece, syllable);
                if (edit.distance <= 1) transitions.push_back({syllable, edit});
            }
            if (transitions.empty()) continue;
            std::sort(transitions.begin(), transitions.end(), [](const auto& left, const auto& right) {
                if (left.edit.ranking_cost != right.edit.ranking_cost)
                    return left.edit.ranking_cost < right.edit.ranking_cost;
                return left.syllable < right.syllable;
            });
            auto& destination = states[begin + length];
            for (const auto& state : states[begin]) {
                for (const auto& transition : transitions) {
                    if (state.cost + transition.edit.distance > limits.max_total_cost) continue;
                    destination.push_back({
                        state.pinyin + transition.syllable,
                        state.cost + transition.edit.distance,
                        state.ranking_cost + transition.edit.ranking_cost,
                        state.syllables + 1,
                    });
                }
            }
            if (destination.size() > limits.max_states_per_position * 4)
                PruneStates(&destination, limits.max_states_per_position);
        }
    }

    PruneStates(&states.back(), limits.max_states_per_position);
    std::vector<PinyinCorrection> results;
    results.reserve((std::min)(limits.max_results, states.back().size()));
    for (const auto& state : states.back()) {
        if (state.cost == 0) continue;
        results.push_back({state.pinyin, state.cost, state.syllables});
        if (results.size() >= limits.max_results) break;
    }
    return results;
}

}  // namespace shuru
