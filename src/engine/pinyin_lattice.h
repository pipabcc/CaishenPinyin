#pragma once

#include "candidate.h"
#include "pinyin_syllables.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace shuru::pinyin_data {

struct SyllableEdge {
    std::size_t begin = 0;       // normalized-input offset, apostrophes included
    std::size_t end = 0;
    std::string syllable;
    bool partial = false;
};

struct SyllablePath {
    std::vector<SyllableEdge> edges;
    std::size_t covered = 0;
    bool complete = false;
};

// Enumerates legal syllable paths. Apostrophes are hard boundaries and are never
// silently erased. A final prefix of a legal syllable is retained as a partial edge.
inline std::vector<SyllablePath> BuildSyllableLattice(
    const std::string& input, std::size_t max_paths = 64) {
    std::vector<SyllablePath> paths(1);
    paths.front().complete = true;
    std::size_t segment_begin = 0;
    while (segment_begin <= input.size()) {
        const std::size_t quote = input.find('\'', segment_begin);
        const std::size_t segment_end = quote == std::string::npos ? input.size() : quote;
        const std::string segment = input.substr(segment_begin, segment_end - segment_begin);
        std::vector<std::vector<SyllablePath>> at(segment.size() + 1);
        at[0].push_back(SyllablePath{});
        for (std::size_t begin = 0; begin < segment.size(); ++begin) {
            if (at[begin].empty()) continue;
            for (std::size_t len = 1; len <= 6 && begin + len <= segment.size(); ++len) {
                const std::string piece = segment.substr(begin, len);
                const bool exact = Syllables().count(piece) != 0;
                bool partial = false;
                if (begin + len == segment.size()) {
                    for (const auto& syllable : Syllables()) {
                        if (syllable.size() > piece.size() && syllable.compare(0, piece.size(), piece) == 0) {
                            partial = true;
                            break;
                        }
                    }
                }
                if (!exact && !partial) continue;
                for (const auto& prefix : at[begin]) {
                    if (exact) {
                        SyllablePath next = prefix;
                        next.edges.push_back({segment_begin + begin, segment_begin + begin + len, piece, false});
                        next.covered = segment_begin + begin + len;
                        next.complete = true;
                        at[begin + len].push_back(std::move(next));
                    }
                    // A spelling can be both a complete syllable and a prefix of a longer
                    // syllable (for example final "ha" in "niha"). Preserve both paths.
                    if (partial) {
                        SyllablePath next = prefix;
                        next.edges.push_back({segment_begin + begin, segment_begin + begin + len, piece, true});
                        next.covered = segment_begin + begin + len;
                        next.complete = false;
                        at[begin + len].push_back(std::move(next));
                    }
                    if (at[begin + len].size() >= max_paths) break;
                }
            }
        }
        std::vector<SyllablePath> segment_paths = at[segment.size()];
        if (segment.empty()) segment_paths = {SyllablePath{}};
        std::vector<SyllablePath> joined;
        for (const auto& left : paths) for (const auto& right : segment_paths) {
            SyllablePath value = left;
            value.edges.insert(value.edges.end(), right.edges.begin(), right.edges.end());
            value.covered = right.edges.empty() ? segment_end : right.covered;
            value.complete = left.complete && right.complete;
            joined.push_back(std::move(value));
            if (joined.size() >= max_paths) break;
        }
        paths.swap(joined);
        if (paths.empty() || quote == std::string::npos) break;
        for (auto& path : paths) path.covered = quote + 1;
        segment_begin = quote + 1;
    }
    std::sort(paths.begin(), paths.end(), [](const SyllablePath& a, const SyllablePath& b) {
        if (a.complete != b.complete) return a.complete > b.complete;
        if (a.covered != b.covered) return a.covered > b.covered;
        if (a.edges.size() != b.edges.size()) return a.edges.size() < b.edges.size();
        for (std::size_t i = 0; i < a.edges.size() && i < b.edges.size(); ++i)
            if (a.edges[i].syllable != b.edges[i].syllable) return a.edges[i].syllable < b.edges[i].syllable;
        return false;
    });
    if (paths.size() > max_paths) paths.resize(max_paths);
    return paths;
}

inline std::string RemoveSyllableSeparators(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) if (ch != '\'') out.push_back(ch);
    return out;
}

// 为两字及以上、与实际输入完整对齐的候选生成展示边界。前缀预测允许最后
// 一个音节仍是部分拼写，但只展示用户已经键入的字母，绝不补出未输入后缀。
inline std::string BuildCandidateInputSegmentationFromLattice(
    const std::string& typed,
    const std::vector<SyllablePath>& paths,
    const Candidate& candidate) {
    if (!candidate.input_segmentation.empty() || candidate.text.size() < 2 ||
        typed.empty() || candidate.pinyin.empty() ||
        !std::all_of(candidate.text.begin(), candidate.text.end(), [](wchar_t ch) {
            return ch >= L'\x4e00' && ch <= L'\x9fff';
        })) {
        return {};
    }
    const std::string compact = RemoveSyllableSeparators(typed);
    if (compact.empty() || candidate.pinyin.size() < compact.size() ||
        candidate.pinyin.compare(0, compact.size(), compact) != 0) {
        return {};
    }
    for (const auto& path : paths) {
        if (path.covered != typed.size() ||
            path.edges.size() != candidate.text.size()) {
            continue;
        }
        const bool valid_partial = !path.edges.empty() &&
            path.edges.back().partial &&
            std::none_of(path.edges.begin(), path.edges.end() - 1,
                         [](const SyllableEdge& edge) { return edge.partial; });
        if (!path.complete && !valid_partial) continue;

        std::string segmented;
        for (const auto& edge : path.edges) {
            if (!segmented.empty()) segmented.push_back('\'');
            segmented += edge.syllable;
        }
        return segmented;
    }
    return {};
}

inline std::string BuildCandidateInputSegmentation(
    const std::string& input, const Candidate& candidate) {
    if (candidate.covered_input_len == 0) return {};
    const std::size_t covered = (std::min)(candidate.covered_input_len, input.size());
    const std::string typed = input.substr(0, covered);
    return BuildCandidateInputSegmentationFromLattice(
        typed, BuildSyllableLattice(typed), candidate);
}

inline bool CandidateRespectsHardBoundaries(
    const std::string& query, const Candidate& candidate) {
    if (query.find('\'') == std::string::npos || candidate.is_english) return true;
    const auto paths = BuildSyllableLattice(query);
    for (const auto& path : paths) {
        if (!path.complete || path.covered != query.size()) continue;
        if (RemoveSyllableSeparators(query) != candidate.pinyin) continue;
        // UTF-16 length is a useful deterministic alignment contract for the BMP
        // Chinese lexicon and prevents xi'an from matching the one-syllable 先.
        if (candidate.text.size() == path.edges.size()) return true;
    }
    return false;
}

}  // namespace shuru::pinyin_data
