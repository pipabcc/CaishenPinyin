#pragma once

#include "pinyin_syllables.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace shuru {

struct FuzzyConfig {
    bool initials = true;
    bool finals = true;
    bool missing_vowel = true;
    int initial_cost = 100;
    int final_cost = 180;
    int missing_vowel_cost = 320;
    int max_cost = 500;
    size_t max_variants = 24;
    size_t max_work = 512;
};

struct FuzzyVariant {
    std::string pinyin;
    int cost = 0;
};

inline std::vector<std::string> SegmentPinyin(const std::string& input) {
    std::vector<std::string> parts;
    const auto& table = pinyin_data::Syllables();
    size_t i = 0;
    while (i < input.size()) {
        bool found = false;
        const size_t max_len = (std::min)(static_cast<size_t>(6), input.size() - i);
        for (size_t len = max_len; len >= 1; --len) {
            const std::string sub = input.substr(i, len);
            if (table.count(sub)) {
                parts.push_back(sub);
                i += len;
                found = true;
                break;
            }
        }
        if (!found) {
            parts.push_back(input.substr(i));
            break;
        }
    }
    return parts;
}

inline bool IsCompletePinyin(const std::string& value) {
    size_t covered = 0;
    for (const auto& piece : SegmentPinyin(value)) {
        if (!pinyin_data::Syllables().count(piece)) return false;
        covered += piece.size();
    }
    return covered == value.size();
}

inline void AddBest(std::map<std::string, int>* values, const std::string& value, int cost) {
    if (value.empty()) return;
    const auto it = values->find(value);
    if (it == values->end() || cost < it->second) (*values)[value] = cost;
}

inline std::vector<FuzzyVariant> ExpandFuzzyPinyinWeighted(
    const std::string& pinyin, const FuzzyConfig& config = {}) {
    if (pinyin.empty()) return {};
    size_t work = 0;
    std::vector<std::map<std::string, int>> options;
    for (const auto& syl : SegmentPinyin(pinyin)) {
        std::map<std::string, int> one;
        AddBest(&one, syl, 0);
        if (config.initials && ++work <= config.max_work) {
            auto replace_initial = [&](const char* from, const char* to) {
                const size_t n = std::char_traits<char>::length(from);
                if (syl.compare(0, n, from) == 0)
                    AddBest(&one, std::string(to) + syl.substr(n), config.initial_cost);
            };
            if (syl.rfind("zh", 0) == 0) replace_initial("zh", "z"); else replace_initial("z", "zh");
            if (syl.rfind("ch", 0) == 0) replace_initial("ch", "c"); else replace_initial("c", "ch");
            if (syl.rfind("sh", 0) == 0) replace_initial("sh", "s"); else replace_initial("s", "sh");
            replace_initial("n", "l"); replace_initial("l", "n");
        }
        if (config.finals) {
            const auto base = one;
            for (const auto& item : base) {
                auto swap_suffix = [&](const char* from, const char* to) {
                    if (++work > config.max_work) return;
                    const size_t n = std::char_traits<char>::length(from);
                    if (item.first.size() >= n && item.first.compare(item.first.size() - n, n, from) == 0)
                        AddBest(&one, item.first.substr(0, item.first.size() - n) + to,
                                item.second + config.final_cost);
                };
                swap_suffix("ang", "an"); swap_suffix("an", "ang");
                swap_suffix("eng", "en"); swap_suffix("en", "eng");
                swap_suffix("ing", "in"); swap_suffix("in", "ing");
                swap_suffix("iang", "ian"); swap_suffix("ian", "iang");
                swap_suffix("uang", "uan"); swap_suffix("uan", "uang");
            }
        }
        for (auto it = one.begin(); it != one.end();) {
            if (it->first != syl && !pinyin_data::Syllables().count(it->first)) it = one.erase(it);
            else ++it;
        }
        options.push_back(std::move(one));
    }

    std::map<std::string, int> combined{{"", 0}};
    for (const auto& one : options) {
        std::map<std::string, int> next;
        for (const auto& prefix : combined) for (const auto& piece : one) {
            if (++work > config.max_work) break;
            const int cost = prefix.second + piece.second;
            if (cost <= config.max_cost) AddBest(&next, prefix.first + piece.first, cost);
        }
        combined.swap(next);
    }
    if (config.missing_vowel) {
        static constexpr char kVowels[] = "aeiouv";
        for (size_t pos = 0; pos <= pinyin.size() && work < config.max_work; ++pos) {
            for (const char* vowel = kVowels; *vowel && work < config.max_work; ++vowel, ++work) {
                std::string recovered = pinyin;
                recovered.insert(recovered.begin() + static_cast<std::string::difference_type>(pos), *vowel);
                if (IsCompletePinyin(recovered)) AddBest(&combined, recovered, config.missing_vowel_cost);
            }
        }
    }
    AddBest(&combined, pinyin, 0);
    std::vector<FuzzyVariant> result;
    for (const auto& item : combined) result.push_back({item.first, item.second});
    std::sort(result.begin(), result.end(), [](const FuzzyVariant& a, const FuzzyVariant& b) {
        return a.cost != b.cost ? a.cost < b.cost : a.pinyin < b.pinyin;
    });
    if (result.size() > config.max_variants) result.resize(config.max_variants);
    return result;
}

inline std::vector<std::string> ExpandFuzzyPinyin(const std::string& pinyin, size_t max_variants = 24) {
    FuzzyConfig config;
    config.max_variants = max_variants;
    std::vector<std::string> result;
    for (const auto& variant : ExpandFuzzyPinyinWeighted(pinyin, config)) result.push_back(variant.pinyin);
    return result;
}

}  // namespace shuru
