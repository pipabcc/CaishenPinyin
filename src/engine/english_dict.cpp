#include "english_dict.h"

#include "../common/logger.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace shuru {
namespace {

std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsAsciiLetter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool IsDisplayCharacter(char value) {
    return IsAsciiLetter(value) || value == '\'' || value == '-';
}

bool IsDisplayWord(const std::string& value) {
    if (value.empty() || !IsAsciiLetter(value.front())) return false;
    return std::all_of(value.begin(), value.end(), IsDisplayCharacter);
}

}  // namespace

std::string EnglishDictionary::Normalize(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (ch >= 'a' && ch <= 'z') {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

bool EnglishDictionary::LoadFromFile(const std::wstring& path) {
    words_.clear();
    std::ifstream input{std::filesystem::path(path)};
    if (!input) {
        SHURU_LOG_WARN("EnglishDictionary open failed");
        return false;
    }

    // Keep one entry per lookup key while accepting both the new three-column
    // format and the historical ``word<TAB>frequency`` format.
    std::unordered_map<std::string, Item> unique;
    std::string line;
    size_t loaded = 0;
    while (std::getline(input, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const size_t first_tab = line.find('\t');
        const size_t second_tab = first_tab == std::string::npos
            ? std::string::npos
            : line.find('\t', first_tab + 1);
        std::string key;
        std::string display;
        int frequency = 1000;
        if (first_tab == std::string::npos) {
            key = Normalize(line);
            display = key;
        } else if (second_tab == std::string::npos) {
            key = Normalize(line.substr(0, first_tab));
            display = key;
            try {
                frequency = std::stoi(Trim(line.substr(first_tab + 1)));
            } catch (...) {
                frequency = 1000;
            }
        } else {
            key = Normalize(line.substr(0, first_tab));
            display = Trim(line.substr(first_tab + 1, second_tab - first_tab - 1));
            try {
                frequency = std::stoi(Trim(line.substr(second_tab + 1)));
            } catch (...) {
                frequency = 1000;
            }
        }
        if (key.size() < 2 || display.empty() || !IsDisplayWord(display)) continue;
        frequency = (std::max)(0, frequency);
        auto found = unique.find(key);
        if (found == unique.end() || frequency > found->second.frequency ||
            (frequency == found->second.frequency && display < found->second.display)) {
            unique[key] = Item{key, display, frequency};
        }
        ++loaded;
    }

    words_.reserve(unique.size());
    for (auto& [key, item] : unique) words_.push_back(std::move(item));
    std::sort(words_.begin(), words_.end(), [](const Item& left, const Item& right) {
        return left.word < right.word;
    });
    SHURU_LOG_INFO(
        "EnglishDictionary loaded entries=%zu unique=%zu", loaded, words_.size());
    return !words_.empty();
}

std::vector<Candidate> EnglishDictionary::LookupExact(const std::string& word) const {
    const std::string key = Normalize(word);
    std::vector<Candidate> result;
    if (key.empty() || words_.empty()) return result;
    const auto it = std::lower_bound(
        words_.begin(), words_.end(), key,
        [](const Item& item, const std::string& value) { return item.word < value; });
    if (it == words_.end() || it->word != key) return result;

    Candidate candidate;
    candidate.text = std::wstring(it->display.begin(), it->display.end());
    candidate.pinyin = it->word;
    candidate.frequency = it->frequency;
    candidate.from_user = false;
    candidate.is_english = true;
    result.push_back(std::move(candidate));
    return result;
}

std::vector<Candidate> EnglishDictionary::LookupPrefix(
    const std::string& prefix, size_t limit) const {
    const std::string normalized = Normalize(prefix);
    std::vector<Candidate> result;
    if (normalized.size() < 2 || limit == 0 || words_.empty()) return result;

    const auto begin = std::lower_bound(
        words_.begin(), words_.end(), normalized,
        [](const Item& item, const std::string& value) { return item.word < value; });
    struct Match {
        const Item* item = nullptr;
        size_t completion_length = 0;
    };
    std::vector<Match> matches;
    for (auto it = begin; it != words_.end(); ++it) {
        if (it->word.size() < normalized.size() ||
            it->word.compare(0, normalized.size(), normalized) != 0) {
            break;
        }
        matches.push_back(Match{&*it, it->word.size() - normalized.size()});
    }
    const auto match_less = [](const Match& left, const Match& right) {
        // Exact-length completions are easiest to recognize; frequency breaks
        // ties among words with the same completion length.
        if (left.completion_length != right.completion_length) {
            return left.completion_length < right.completion_length;
        }
        if (left.item->frequency != right.item->frequency) {
            return left.item->frequency > right.item->frequency;
        }
        if (left.item->word != right.item->word) return left.item->word < right.item->word;
        return left.item->display < right.item->display;
    };
    if (matches.size() > limit) {
        std::partial_sort(matches.begin(), matches.begin() + limit, matches.end(), match_less);
        matches.resize(limit);
    } else {
        std::sort(matches.begin(), matches.end(), match_less);
    }
    result.reserve(matches.size());
    for (const auto& match : matches) {
        Candidate candidate;
        candidate.text = std::wstring(
            match.item->display.begin(), match.item->display.end());
        candidate.pinyin = match.item->word;
        candidate.frequency = match.item->frequency;
        candidate.is_english = true;
        result.push_back(std::move(candidate));
    }
    return result;
}

}  // namespace shuru
