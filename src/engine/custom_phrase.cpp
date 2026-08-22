#include "custom_phrase.h"

#include "../common/user_data_paths.h"

#include "../common/com_utils.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace shuru {
namespace {

constexpr std::size_t kMaximumCodeLength = 32;
constexpr std::size_t kMaximumPhraseLength = 128;

std::string TrimAscii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool NormalizeCode(const std::string& input, std::string* output) {
    if (output == nullptr) return false;
    output->clear();
    const std::string trimmed = TrimAscii(input);
    if (trimmed.empty() || trimmed.size() > kMaximumCodeLength) return false;
    output->reserve(trimmed.size());
    for (const unsigned char ch : trimmed) {
        if (ch >= 'A' && ch <= 'Z') {
            output->push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (ch >= 'a' && ch <= 'z') {
            output->push_back(static_cast<char>(ch));
        } else {
            output->clear();
            return false;
        }
    }
    return true;
}

bool IsValidPhrase(const std::wstring& phrase) {
    if (phrase.empty() || phrase.size() > kMaximumPhraseLength) return false;
    return std::none_of(phrase.begin(), phrase.end(), [](wchar_t ch) {
        return ch == L'\t' || ch == L'\r' || ch == L'\n' || ch < L' ';
    });
}

}  // namespace

bool CustomPhraseDictionary::LoadFromFile(const std::wstring& path) {
    entries_.clear();
    size_ = 0;
    if (path.empty()) return true;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;

    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return false;

    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        const std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') continue;

        std::stringstream fields(line);
        std::string raw_code;
        std::string phrase_utf8;
        std::string position_text;
        std::string extra;
        if (!std::getline(fields, raw_code, '\t') ||
            !std::getline(fields, phrase_utf8, '\t') ||
            !std::getline(fields, position_text, '\t') ||
            std::getline(fields, extra, '\t')) {
            continue;
        }

        std::string code;
        if (!NormalizeCode(raw_code, &code)) continue;
        const std::wstring phrase = Utf8ToWide(phrase_utf8);
        if (!IsValidPhrase(phrase)) continue;

        const std::string normalized_position = TrimAscii(position_text);
        std::size_t position = 0;
        try {
            std::size_t used = 0;
            position = static_cast<std::size_t>(std::stoul(normalized_position, &used));
            if (used != normalized_position.size() || position < 1 || position > 9) continue;
        } catch (...) {
            continue;
        }

        const std::string identity = code + '\0' + phrase_utf8;
        if (!seen.insert(identity).second) continue;
        entries_[code].push_back(CustomPhraseEntry {code, phrase, position});
        ++size_;
    }

    for (auto& bucket : entries_) {
        std::stable_sort(bucket.second.begin(), bucket.second.end(), [](const auto& left, const auto& right) {
            return left.position < right.position;
        });
    }
    return input.eof() || static_cast<bool>(input);
}

std::vector<CustomPhraseEntry> CustomPhraseDictionary::LookupExact(const std::string& code) const {
    std::string normalized;
    if (!NormalizeCode(code, &normalized)) return {};
    const auto found = entries_.find(normalized);
    return found == entries_.end() ? std::vector<CustomPhraseEntry>{} : found->second;
}

std::wstring GetCustomPhrasePath(const std::wstring& fallback_directory) {
    const std::wstring path = CaishenUserDataPath(
        L"data\\lexicon\\custom_phrases.txt");
    if (!path.empty()) return path;
    return fallback_directory.empty()
        ? std::wstring(L"custom_phrases.txt")
        : fallback_directory + L"\\custom_phrases.txt";
}

void InsertCustomPhraseCandidates(
    const std::vector<CustomPhraseEntry>& phrases,
    const std::string& normalized_code,
    std::size_t covered_input_len,
    std::size_t limit,
    std::vector<Candidate>* candidates) {
    if (candidates == nullptr || phrases.empty() || limit == 0) return;

    std::size_t next_available_position = 0;
    for (const auto& phrase : phrases) {
        const std::size_t desired = (std::max)(phrase.position - 1, next_available_position);
        if (desired >= limit) continue;

        candidates->erase(
            std::remove_if(candidates->begin(), candidates->end(), [&](const Candidate& candidate) {
                return candidate.text == phrase.phrase;
            }),
            candidates->end());

        Candidate candidate;
        candidate.text = phrase.phrase;
        candidate.pinyin = normalized_code;
        candidate.covered_input_len = covered_input_len;
        candidate.learnable = false;
        candidate.from_user = false;
        candidate.ranking_score = 1000000.0;
        candidate.source = CandidateSource::CustomPhrase;

        const std::size_t insertion = (std::min)(desired, candidates->size());
        candidates->insert(
            candidates->begin() + static_cast<std::vector<Candidate>::difference_type>(insertion),
            std::move(candidate));
        next_available_position = insertion + 1;
    }
    if (candidates->size() > limit) candidates->resize(limit);
}

}  // namespace shuru
