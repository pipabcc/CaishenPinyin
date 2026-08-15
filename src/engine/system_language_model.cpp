#include "system_language_model.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

namespace shuru {
namespace {

constexpr char kMagic[8] = {'C', 'S', 'N', 'G', 'R', 'M', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMaxBigrams = 1'000'000;
constexpr std::uint32_t kMaxTrigrams = 2'000'000;
constexpr char kGrammarPrefix[] = "Rime::Grammar/";
constexpr int kMaxEncodedUnicode = 8;
constexpr int kCollocationMaxLength = 5;
constexpr int kCollocationMinLength = 2;
constexpr double kCollocationPenalty = -14.0;
constexpr double kNonCollocationPenalty = -4.0;
constexpr double kWeakCollocationPenalty = -24.0;
constexpr double kRearPenalty = -18.0;
constexpr double kGrammarValueScale = 10000.0;

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

struct GrammarMetadata {
    char format[32];
    std::uint32_t checksum;
    std::uint32_t unit_count;
    std::int32_t double_array_offset;
};
#pragma pack(pop)

static_assert(sizeof(GrammarMetadata) == 44, "unexpected grammar metadata layout");

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

std::uint32_t UnitLabel(std::uint32_t unit) {
    return unit & ((std::uint32_t{1} << 31) | 0xFFu);
}

bool UnitHasLeaf(std::uint32_t unit) {
    return ((unit >> 8) & 1u) != 0;
}

std::uint32_t UnitOffset(std::uint32_t unit) {
    return (unit >> 10) << ((unit & (std::uint32_t{1} << 9)) >> 6);
}

bool DecodeCodePoints(const std::wstring& text, std::vector<std::uint32_t>* out) {
    if (out == nullptr) return false;
    out->clear();
    out->reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const std::uint32_t first = static_cast<std::uint16_t>(text[index]);
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (index + 1 >= text.size()) return false;
            const std::uint32_t second =
                static_cast<std::uint16_t>(text[index + 1]);
            if (second < 0xDC00 || second > 0xDFFF) return false;
            out->push_back(0x10000 + ((first - 0xD800) << 10) +
                           (second - 0xDC00));
            ++index;
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return false;
        } else {
            out->push_back(first);
        }
    }
    return true;
}

void EncodeCodePoint(std::uint32_t value, std::string* output) {
    if (value < 0x80) {
        output->push_back(static_cast<char>(value == 0 ? 0xE0 : value));
        return;
    }
    if (value >= 0x4000 && value < 0xA000) {
        if ((value & 0xFF) == 0) {
            output->push_back(static_cast<char>(0xE1));
            output->push_back(static_cast<char>((value >> 8) + 0x40));
        } else {
            output->push_back(static_cast<char>((value >> 8) + 0x40));
            output->push_back(static_cast<char>(value & 0xFF));
        }
        return;
    }

    int bits = 32;
    while (bits > 0 && (value & 0xFE000000u) == 0) {
        bits -= 7;
        value <<= 7;
    }
    int bytes = (bits + 6) / 7;
    output->push_back(static_cast<char>(0xE0 | bytes));
    while (bytes-- > 0) {
        output->push_back(static_cast<char>(((value >> 25) & 0x7F) | 0x80));
        value <<= 7;
    }
}

std::string EncodeCodePoints(const std::vector<std::uint32_t>& code_points,
                             size_t begin,
                             size_t end) {
    std::string encoded;
    encoded.reserve((end - begin) * 2);
    for (size_t index = begin; index < end; ++index) {
        EncodeCodePoint(code_points[index], &encoded);
    }
    return encoded;
}

size_t EncodedCharacterLength(unsigned char first) {
    if ((first & 0x80) == 0) return 1;
    if ((first & 0xF0) == 0xE0) return (first & 0x0F) + 1;
    return 2;
}

size_t CountEncodedCharacters(std::string_view encoded, size_t byte_count) {
    const size_t end = (std::min)(encoded.size(), byte_count);
    size_t count = 0;
    for (size_t offset = 0; offset < end;) {
        const size_t length = EncodedCharacterLength(
            static_cast<unsigned char>(encoded[offset]));
        if (length == 0 || offset + length > end) break;
        offset += length;
        ++count;
    }
    return count;
}

}  // namespace

struct SystemLanguageModel::GrammarMapping {
    struct Match {
        int value = -1;
        size_t length = 0;
    };

    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const unsigned char* view = nullptr;
    size_t size = 0;
    const std::uint32_t* units = nullptr;
    size_t unit_count = 0;

    ~GrammarMapping() {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    bool Unit(size_t index, std::uint32_t* value) const {
        if (value == nullptr || units == nullptr || index >= unit_count) {
            return false;
        }
        *value = units[index];
        return true;
    }

    bool Traverse(std::string_view key, size_t* node) const {
        if (node == nullptr) return false;
        size_t current = *node;
        std::uint32_t unit = 0;
        if (!Unit(current, &unit)) return false;
        for (unsigned char label : key) {
            current ^= static_cast<size_t>(UnitOffset(unit)) ^ label;
            if (!Unit(current, &unit) || UnitLabel(unit) != label) return false;
        }
        *node = current;
        return true;
    }

    size_t Lookup(std::string_view context,
                  std::string_view word,
                  Match* results,
                  size_t capacity) const {
        size_t node = 0;
        if (!Traverse(context, &node)) return 0;

        std::uint32_t unit = 0;
        if (!Unit(node, &unit)) return 0;
        node ^= UnitOffset(unit);
        size_t count = 0;
        for (size_t index = 0; index < word.size(); ++index) {
            node ^= static_cast<unsigned char>(word[index]);
            if (!Unit(node, &unit) ||
                UnitLabel(unit) != static_cast<unsigned char>(word[index])) {
                return count;
            }
            node ^= UnitOffset(unit);
            if (UnitHasLeaf(unit)) {
                std::uint32_t leaf = 0;
                if (!Unit(node, &leaf)) return count;
                if (count < capacity) {
                    results[count].value = static_cast<int>(leaf & 0x7FFFFFFFu);
                    results[count].length = index + 1;
                }
                ++count;
            }
        }
        return count;
    }
};

std::shared_ptr<const SystemLanguageModel::GrammarMapping>
SystemLanguageModel::LoadGrammarMapping(
    const std::wstring& path) {
    auto grammar = std::make_shared<SystemLanguageModel::GrammarMapping>();
    grammar->file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (grammar->file == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER file_size {};
    if (!GetFileSizeEx(grammar->file, &file_size) ||
        file_size.QuadPart < static_cast<LONGLONG>(sizeof(GrammarMetadata)) ||
        static_cast<unsigned long long>(file_size.QuadPart) >
            static_cast<unsigned long long>((std::numeric_limits<size_t>::max)())) {
        return nullptr;
    }
    grammar->size = static_cast<size_t>(file_size.QuadPart);
    grammar->mapping = CreateFileMappingW(
        grammar->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (grammar->mapping == nullptr) return nullptr;
    grammar->view = static_cast<const unsigned char*>(
        MapViewOfFile(grammar->mapping, FILE_MAP_READ, 0, 0, 0));
    if (grammar->view == nullptr) return nullptr;

    const auto* metadata =
        reinterpret_cast<const GrammarMetadata*>(grammar->view);
    if (std::memcmp(metadata->format, kGrammarPrefix,
                    sizeof(kGrammarPrefix) - 1) != 0 ||
        std::memchr(metadata->format, '\0', sizeof(metadata->format)) == nullptr ||
        metadata->unit_count < 256 || metadata->double_array_offset <= 0) {
        return nullptr;
    }
    const size_t offset_field = offsetof(GrammarMetadata, double_array_offset);
    const size_t array_offset = offset_field +
        static_cast<size_t>(metadata->double_array_offset);
    const size_t array_bytes =
        static_cast<size_t>(metadata->unit_count) * sizeof(std::uint32_t);
    if (array_offset < sizeof(GrammarMetadata) ||
        array_offset % alignof(std::uint32_t) != 0 ||
        array_bytes / sizeof(std::uint32_t) != metadata->unit_count ||
        array_offset > grammar->size ||
        array_bytes != grammar->size - array_offset) {
        return nullptr;
    }
    grammar->units = reinterpret_cast<const std::uint32_t*>(
        grammar->view + array_offset);
    grammar->unit_count = metadata->unit_count;

    std::uint32_t root = 0;
    if (!grammar->Unit(0, &root) || UnitLabel(root) != 0 ||
        UnitHasLeaf(root) || UnitOffset(root) == 0 || UnitOffset(root) >= 512) {
        return nullptr;
    }
    return grammar;
}

bool SystemLanguageModel::LoadLegacyModel(
    const std::wstring& path,
    std::vector<BigramRecord>* bigrams,
    std::vector<TrigramRecord>* trigrams) {
    if (bigrams == nullptr || trigrams == nullptr) return false;
    std::vector<BigramRecord> loaded_bigrams;
    std::vector<TrigramRecord> loaded_trigrams;
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

        loaded_bigrams.reserve(header.bigram_count);
        std::uint32_t previous_bigram = 0;
        for (std::uint32_t index = 0; index < header.bigram_count; ++index) {
            FileBigramRecord record {};
            if (!ReadExact(&input, &record) || record.count == 0 ||
                (index != 0 && record.key <= previous_bigram)) {
                return false;
            }
            previous_bigram = record.key;
            loaded_bigrams.push_back({record.key, record.count});
        }

        loaded_trigrams.reserve(header.trigram_count);
        std::uint64_t previous_trigram = 0;
        for (std::uint32_t index = 0; index < header.trigram_count; ++index) {
            FileTrigramRecord record {};
            if (!ReadExact(&input, &record) || record.count == 0 ||
                (index != 0 && record.key <= previous_trigram)) {
                return false;
            }
            previous_trigram = record.key;
            loaded_trigrams.push_back({record.key, record.count});
        }
        if (input.peek() != std::ifstream::traits_type::eof()) return false;
    } catch (...) {
        return false;
    }
    *bigrams = std::move(loaded_bigrams);
    *trigrams = std::move(loaded_trigrams);
    return true;
}

bool SystemLanguageModel::LoadFromFile(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    char signature[32] {};
    if (!input.read(signature, sizeof(signature))) return false;
    if (std::memcmp(signature, kGrammarPrefix, sizeof(kGrammarPrefix) - 1) == 0) {
        auto grammar = LoadGrammarMapping(path);
        if (!grammar) return false;
        grammar_ = std::move(grammar);
        bigrams_.clear();
        trigrams_.clear();
        return true;
    }

    std::vector<BigramRecord> bigrams;
    std::vector<TrigramRecord> trigrams;
    if (!LoadLegacyModel(path, &bigrams, &trigrams)) return false;
    bigrams_ = std::move(bigrams);
    trigrams_ = std::move(trigrams);
    grammar_.reset();
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
    const std::wstring& prefix, const std::wstring& next, bool is_rear) const {
    if (next.empty() || empty()) return 0.0;
    if (grammar_) return QueryGrammar(prefix, next, is_rear);

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

size_t SystemLanguageModel::grammar_unit_count() const {
    return grammar_ ? grammar_->unit_count : 0;
}

size_t SystemLanguageModel::mapped_bytes() const {
    return grammar_ ? grammar_->size : 0;
}

double SystemLanguageModel::QueryGrammar(
    const std::wstring& context,
    const std::wstring& word,
    bool is_rear) const {
    if (!grammar_ || context.empty()) return kNonCollocationPenalty;

    std::vector<std::uint32_t> context_points;
    std::vector<std::uint32_t> word_points;
    if (!DecodeCodePoints(context, &context_points) ||
        !DecodeCodePoints(word, &word_points) || word_points.empty()) {
        return kNonCollocationPenalty;
    }

    const size_t max_side = static_cast<size_t>((std::min)(
        kMaxEncodedUnicode, kCollocationMaxLength - 1));
    const size_t context_begin = context_points.size() > max_side
        ? context_points.size() - max_side
        : 0;
    const size_t word_end = (std::min)(word_points.size(), max_side);
    const std::string context_query = EncodeCodePoints(
        context_points, context_begin, context_points.size());
    const std::string word_query = EncodeCodePoints(word_points, 0, word_end);

    std::vector<size_t> context_offsets;
    context_offsets.reserve(context_points.size() - context_begin);
    size_t offset = 0;
    for (size_t index = context_begin; index < context_points.size(); ++index) {
        context_offsets.push_back(offset);
        std::string encoded;
        EncodeCodePoint(context_points[index], &encoded);
        offset += encoded.size();
    }

    double result = kNonCollocationPenalty;
    GrammarMapping::Match matches[8] {};
    for (size_t suffix = 0; suffix < context_offsets.size(); ++suffix) {
        const std::string_view encoded_context(
            context_query.data() + context_offsets[suffix],
            context_query.size() - context_offsets[suffix]);
        const size_t count = grammar_->Lookup(
            encoded_context, word_query, matches, std::size(matches));
        const size_t stored = (std::min)(count, std::size(matches));
        for (size_t index = 0; index < stored; ++index) {
            const size_t match_characters = CountEncodedCharacters(
                word_query, matches[index].length);
            const size_t context_characters = context_offsets.size() - suffix;
            const size_t collocation_length =
                context_characters + match_characters;
            const bool whole_query = suffix == 0 &&
                matches[index].length == word_query.size();
            const double penalty =
                collocation_length >= static_cast<size_t>(kCollocationMinLength) ||
                    whole_query
                ? kCollocationPenalty
                : kWeakCollocationPenalty;
            result = (std::max)(result,
                static_cast<double>(matches[index].value) /
                    kGrammarValueScale + penalty);
        }
    }

    if (is_rear && word_end == word_points.size()) {
        const size_t count = grammar_->Lookup(
            word_query, "$", matches, std::size(matches));
        if (count > 0) {
            result = (std::max)(result,
                static_cast<double>(matches[0].value) /
                    kGrammarValueScale + kRearPenalty);
        }
    }
    return result;
}

}  // namespace shuru
