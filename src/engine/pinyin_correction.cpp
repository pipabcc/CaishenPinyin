#include "pinyin_correction.h"

#include "pinyin_lattice.h"
#include "pinyin_syllables.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace shuru {
namespace {

struct EditMeasurement {
    int distance = 3;
    int ranking_cost = (std::numeric_limits<int>::max)() / 4;
};

// QWERTY 相邻键：手滑打错几乎都落在物理相邻键上，替换代价按键距分级，
// 相邻键纠错排序优先于任意替换。
bool AreKeysAdjacent(char left, char right) {
    static const std::unordered_map<char, const char*> kNeighbors = {
        {'q', "wa"},     {'w', "qeas"},   {'e', "wrsd"},  {'r', "etdf"},
        {'t', "ryfg"},   {'y', "tugh"},   {'u', "yihj"},  {'i', "uojk"},
        {'o', "ipkl"},   {'p', "ol"},     {'a', "qwsz"},  {'s', "awedzx"},
        {'d', "serfxc"}, {'f', "drtgcv"}, {'g', "ftyhvb"},{'h', "gyujbn"},
        {'j', "huiknm"}, {'k', "jiolm"},  {'l', "kop"},   {'z', "asx"},
        {'x', "zsdc"},   {'c', "xdfv"},   {'v', "cfgb"},  {'b', "vghn"},
        {'n', "bhjm"},   {'m', "njk"},
    };
    const auto found = kNeighbors.find(left);
    if (found == kNeighbors.end()) return false;
    for (const char* neighbor = found->second; *neighbor; ++neighbor) {
        if (*neighbor == right) return true;
    }
    return false;
}

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
        std::size_t mismatch = 0;
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (left[index] != right[index]) {
                ++differences;
                mismatch = index;
            }
        }
        if (differences != 1) return {};
        return AreKeysAdjacent(left[mismatch], right[mismatch])
            ? EditMeasurement{1, 2}
            : EditMeasurement{1, 4};
    }
    if (left.size() + 1 == right.size()) {
        std::size_t input_index = 0;
        std::size_t output_index = 0;
        bool skipped = false;
        while (input_index < left.size() && output_index < right.size()) {
            if (left[input_index] == right[output_index]) {
                ++input_index;
                ++output_index;
            } else if (!skipped) {
                skipped = true;
                ++output_index;
            } else {
                return {};
            }
        }
        return {1, 2};
    }
    if (right.size() + 1 == left.size()) {
        const EditMeasurement inverse = MeasureSingleEdit(right, left);
        return inverse.distance == 1 ? EditMeasurement{1, 2}
                                     : EditMeasurement{};
    }
    return {};
}

bool BetterMeasurement(
    const EditMeasurement& left, const EditMeasurement& right) {
    if (left.distance != right.distance) return left.distance < right.distance;
    return left.ranking_cost < right.ranking_cost;
}

EditMeasurement AddMeasurement(
    const EditMeasurement& value, int distance, int ranking_cost) {
    if (value.distance > 2 || value.distance + distance > 2) return {};
    return {value.distance + distance, value.ranking_cost + ranking_cost};
}

// 每个音节允许最多两次局部编辑。短片段长度不超过 7，固定数组的
// Damerau-Levenshtein 动态规划既覆盖增删改，也覆盖相邻字母换位。
EditMeasurement MeasureSyllableEdit(
    const std::string& left,
    const std::string& right) {
    constexpr std::size_t kMaximumPieceLength = 7;
    if (left.size() > kMaximumPieceLength || right.size() > kMaximumPieceLength)
        return {};

    std::array<std::array<EditMeasurement, kMaximumPieceLength + 1>,
               kMaximumPieceLength + 1> distance {};
    distance[0][0] = {0, 0};
    auto relax = [&](std::size_t row, std::size_t column,
                     const EditMeasurement& candidate) {
        if (BetterMeasurement(candidate, distance[row][column]))
            distance[row][column] = candidate;
    };

    for (std::size_t row = 0; row <= left.size(); ++row) {
        for (std::size_t column = 0; column <= right.size(); ++column) {
            const EditMeasurement current = distance[row][column];
            if (current.distance > 2) continue;
            if (row < left.size()) {
                relax(row + 1, column, AddMeasurement(current, 1, 2));
            }
            if (column < right.size()) {
                relax(row, column + 1, AddMeasurement(current, 1, 2));
            }
            if (row < left.size() && column < right.size()) {
                if (left[row] == right[column]) {
                    relax(row + 1, column + 1, current);
                } else {
                    relax(row + 1, column + 1,
                          AddMeasurement(current, 1,
                              AreKeysAdjacent(left[row], right[column]) ? 2 : 4));
                }
            }
            if (row + 1 < left.size() && column + 1 < right.size() &&
                left[row] == right[column + 1] &&
                left[row + 1] == right[column]) {
                relax(row + 2, column + 2,
                      AddMeasurement(current, 1, 1));
            }
        }
    }
    return distance[left.size()][right.size()];
}

struct CorrectionState {
    std::string pinyin;
    std::string input_segmentation;
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
    if (left.input_segmentation != right.input_segmentation)
        return left.input_segmentation < right.input_segmentation;
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

void AddWholeInputTranspositions(
    const std::string& input, std::vector<CorrectionState>* states) {
    if (states == nullptr || input.size() < 2) return;
    for (std::size_t index = 0; index + 1 < input.size(); ++index) {
        if (input[index] == input[index + 1]) continue;
        std::string corrected = input;
        std::swap(corrected[index], corrected[index + 1]);
        const auto paths = pinyin_data::BuildSyllableLattice(corrected, 16);
        const auto found = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
            return path.complete && path.covered == corrected.size() &&
                !path.edges.empty();
        });
        if (found == paths.end()) continue;

        std::string display;
        for (const auto& edge : found->edges) {
            if (!display.empty()) display.push_back('\'');
            display.append(input, edge.begin, edge.end - edge.begin);
        }
        states->push_back({std::move(corrected), std::move(display), 1, 1,
                           found->edges.size()});
    }
}

void AddShortDoubleEditCorrections(
    const std::string& input, std::vector<CorrectionState>* states) {
    if (states == nullptr || input.empty() || input.size() > 5) return;

    auto find_complete_path = [](const std::string& value,
                                 pinyin_data::SyllablePath* output) {
        if (value.empty()) {
            *output = {};
            return true;
        }
        const auto paths = pinyin_data::BuildSyllableLattice(value, 8);
        const auto found = std::find_if(
            paths.begin(), paths.end(), [&](const auto& path) {
                return path.complete && path.covered == value.size() &&
                    !path.edges.empty();
            });
        if (found == paths.end()) return false;
        *output = *found;
        return true;
    };
    auto append_segment = [](std::string_view value, std::string* display) {
        if (!display->empty()) display->push_back('\'');
        display->append(value.data(), value.size());
    };

    for (std::size_t begin = 0; begin < input.size(); ++begin) {
        pinyin_data::SyllablePath prefix_path;
        if (!find_complete_path(input.substr(0, begin), &prefix_path)) continue;
        for (std::size_t end = begin + 1; end <= input.size(); ++end) {
            pinyin_data::SyllablePath suffix_path;
            if (!find_complete_path(input.substr(end), &suffix_path)) continue;
            const std::string piece = input.substr(begin, end - begin);
            for (const auto& syllable : pinyin_data::Syllables()) {
                if (std::abs(static_cast<int>(piece.size()) -
                             static_cast<int>(syllable.size())) > 2) {
                    continue;
                }
                const EditMeasurement edit = MeasureSyllableEdit(piece, syllable);
                if (edit.distance != 2) continue;

                std::string display;
                for (const auto& edge : prefix_path.edges) {
                    append_segment(
                        std::string_view(input).substr(
                            edge.begin, edge.end - edge.begin),
                        &display);
                }
                append_segment(piece, &display);
                for (const auto& edge : suffix_path.edges) {
                    append_segment(
                        std::string_view(input).substr(
                            end + edge.begin, edge.end - edge.begin),
                        &display);
                }
                states->push_back({
                    input.substr(0, begin) + syllable + input.substr(end),
                    std::move(display),
                    2,
                    edit.ranking_cost,
                    prefix_path.edges.size() + 1 + suffix_path.edges.size(),
                });
            }
        }
    }
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
                if (edit.distance <= 1)
                    transitions.push_back({syllable, edit});
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
                    std::string input_segmentation = state.input_segmentation;
                    if (!input_segmentation.empty()) input_segmentation.push_back('\'');
                    input_segmentation += piece;
                    destination.push_back({
                        state.pinyin + transition.syllable,
                        std::move(input_segmentation),
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

    std::vector<CorrectionState> completed = std::move(states.back());
    AddWholeInputTranspositions(input, &completed);
    // 短输入额外允许一个音节内的两次编辑，但只枚举“精确前缀 + 一个
    // 双编辑片段 + 精确后缀”，避免通用状态机的组合爆炸。
    AddShortDoubleEditCorrections(input, &completed);
    PruneStates(&completed, (std::max)(
        limits.max_states_per_position, limits.max_results));
    std::vector<PinyinCorrection> results;
    results.reserve((std::min)(limits.max_results, completed.size()));
    for (const auto& state : completed) {
        if (state.cost == 0) continue;
        results.push_back({state.pinyin, state.input_segmentation,
                           state.cost, state.ranking_cost, state.syllables});
        if (results.size() >= limits.max_results) break;
    }
    return results;
}

}  // namespace shuru
