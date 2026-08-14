#include "system_language_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace shuru {
namespace {

constexpr char kMagic[8] = {'C', 'S', 'N', 'G', 'R', 'M', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMaxBigrams = 1'000'000;
constexpr std::uint32_t kMaxTrigrams = 2'000'000;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t bigram_count;
    std::uint32_t trigram_count;
};

struct FileBigramRecord {
    std::uint32_t key;
    std::uint32_t count;
};

struct FileTrigramRecord {
    std::uint64_t key;
    std::uint32_t count;
};
#pragma pack(pop)

bool IsBmpChinese(wchar_t ch) {
    return ch >= L'\x4e00' && ch <= L'\x9fff';
}

std::uint32_t BigramKey(wchar_t first, wchar_t second) {
    return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(first)) << 16) |
        static_cast<std::uint16_t>(second);
}

std::uint64_t TrigramKey(wchar_t first, wchar_t second, wchar_t third) {
    return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(first)) << 32) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(second)) << 16) |
        static_cast<std::uint16_t>(third);
}

template <typename T>
bool ReadExact(std::ifstream* input, T* value) {
    if (input == nullptr || value == nullptr) return false;
    return static_cast<bool>(input->read(
        reinterpret_cast<char*>(value), static_cast<std::streamsize>(sizeof(T))));
}

}  // namespace

bool SystemLanguageModel::LoadFromFile(const std::wstring& path) {
    std::vector<BigramRecord> bigrams;
    std::vector<TrigramRecord> trigrams;
    try {
        const std::filesystem::path file(path);
        const std::uintmax_t size = std::filesystem::file_size(file);
        std::ifstream input(file, std::ios::binary);
        FileHeader header {};
        if (!input || !ReadExact(&input, &header) ||
            std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
            header.version != kFormatVersion ||
            header.bigram_count > kMaxBigrams ||
            header.trigram_count > kMaxTrigrams) {
            return false;
        }

        const std::uintmax_t expected_size = sizeof(FileHeader) +
            static_cast<std::uintmax_t>(header.bigram_count) * sizeof(FileBigramRecord) +
            static_cast<std::uintmax_t>(header.trigram_count) * sizeof(FileTrigramRecord);
        if (size != expected_size) return false;

        bigrams.reserve(header.bigram_count);
        std::uint32_t previous_bigram = 0;
        for (std::uint32_t index = 0; index < header.bigram_count; ++index) {
            FileBigramRecord record {};
            if (!ReadExact(&input, &record) || record.count == 0 ||
                (index != 0 && record.key <= previous_bigram)) {
                return false;
            }
            previous_bigram = record.key;
            bigrams.push_back({record.key, record.count});
        }

        trigrams.reserve(header.trigram_count);
        std::uint64_t previous_trigram = 0;
        for (std::uint32_t index = 0; index < header.trigram_count; ++index) {
            FileTrigramRecord record {};
            if (!ReadExact(&input, &record) || record.count == 0 ||
                (index != 0 && record.key <= previous_trigram)) {
                return false;
            }
            previous_trigram = record.key;
            trigrams.push_back({record.key, record.count});
        }
        if (input.peek() != std::ifstream::traits_type::eof()) return false;
    } catch (...) {
        return false;
    }

    bigrams_ = std::move(bigrams);
    trigrams_ = std::move(trigrams);
    return true;
}

std::uint32_t SystemLanguageModel::BigramCount(
    wchar_t first, wchar_t second) const {
    if (!IsBmpChinese(first) || !IsBmpChinese(second)) return 0;
    const std::uint32_t key = BigramKey(first, second);
    const auto found = std::lower_bound(
        bigrams_.begin(), bigrams_.end(), key,
        [](const BigramRecord& record, std::uint32_t value) {
            return record.key < value;
        });
    return found != bigrams_.end() && found->key == key ? found->count : 0;
}

std::uint32_t SystemLanguageModel::TrigramCount(
    wchar_t first, wchar_t second, wchar_t third) const {
    if (!IsBmpChinese(first) || !IsBmpChinese(second) || !IsBmpChinese(third)) {
        return 0;
    }
    const std::uint64_t key = TrigramKey(first, second, third);
    const auto found = std::lower_bound(
        trigrams_.begin(), trigrams_.end(), key,
        [](const TrigramRecord& record, std::uint64_t value) {
            return record.key < value;
        });
    return found != trigrams_.end() && found->key == key ? found->count : 0;
}

double SystemLanguageModel::AppendScore(
    const std::wstring& prefix, const std::wstring& next) const {
    if (next.empty() || empty()) return 0.0;

    double score = 0.0;
    auto char_at = [&](size_t index) {
        return index < prefix.size()
            ? prefix[index]
            : next[index - prefix.size()];
    };
    const size_t total_size = prefix.size() + next.size();
    for (size_t index = prefix.size(); index < total_size; ++index) {
        if (index >= 1) {
            const std::uint32_t bigram = BigramCount(
                char_at(index - 1), char_at(index));
            if (bigram != 0) {
                score += 0.35 * std::log1p(static_cast<double>(bigram));
            }
        }
        if (index >= 2) {
            const std::uint32_t trigram = TrigramCount(
                char_at(index - 2), char_at(index - 1), char_at(index));
            if (trigram != 0) {
                score += 0.75 * std::log1p(static_cast<double>(trigram));
            }
        }
    }
    return score;
}

}  // namespace shuru
