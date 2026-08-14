#include "system_lexeme_prior.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace shuru {
namespace {

constexpr char kMagic[8] = {'C', 'S', 'L', 'X', 'P', 'R', '1', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kMaxRecords = 500'000;
constexpr std::uint16_t kMaxPinyinBytes = 128;
constexpr std::uint16_t kMaxWordBytes = 256;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t record_count;
};

struct FileRecordHeader {
    std::uint16_t pinyin_bytes;
    std::uint16_t word_bytes;
    std::uint32_t score;
};
#pragma pack(pop)

bool IsValidPinyin(const std::string& pinyin) {
    return !pinyin.empty() &&
        std::all_of(pinyin.begin(), pinyin.end(), [](unsigned char ch) {
            return ch >= 'a' && ch <= 'z';
        });
}

bool DecodeChineseWord(const std::string& utf8, std::wstring* word) {
    if (word == nullptr || utf8.empty() ||
        utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    const int source_size = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_size, nullptr, 0);
    if (needed <= 0) return false;
    std::wstring decoded(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_size,
            decoded.data(), needed) != needed) {
        return false;
    }
    if (!std::all_of(decoded.begin(), decoded.end(), [](wchar_t ch) {
            return ch >= L'\x4e00' && ch <= L'\x9fff';
        })) {
        return false;
    }
    *word = std::move(decoded);
    return true;
}

bool ReadBytes(std::ifstream* input, size_t count, std::string* value) {
    if (input == nullptr || value == nullptr) return false;
    value->resize(count);
    return count == 0 || static_cast<bool>(input->read(
        value->data(), static_cast<std::streamsize>(count)));
}

template <typename T>
bool ReadExact(std::ifstream* input, T* value) {
    if (input == nullptr || value == nullptr) return false;
    return static_cast<bool>(input->read(
        reinterpret_cast<char*>(value), static_cast<std::streamsize>(sizeof(T))));
}

bool KeyLess(
    const std::string& left_pinyin, const std::wstring& left_word,
    const std::string& right_pinyin, const std::wstring& right_word) {
    if (left_pinyin != right_pinyin) return left_pinyin < right_pinyin;
    return left_word < right_word;
}

}  // namespace

bool SystemLexemePriorModel::LoadFromFile(const std::wstring& path) {
    std::vector<Record> records;
    try {
        const std::filesystem::path file(path);
        const std::uintmax_t file_size = std::filesystem::file_size(file);
        std::ifstream input(file, std::ios::binary);
        FileHeader header {};
        if (!input || !ReadExact(&input, &header) ||
            std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
            header.version != kFormatVersion ||
            header.record_count > kMaxRecords ||
            file_size < sizeof(FileHeader) +
                static_cast<std::uintmax_t>(header.record_count) *
                    sizeof(FileRecordHeader)) {
            return false;
        }

        records.reserve(header.record_count);
        for (std::uint32_t index = 0; index < header.record_count; ++index) {
            FileRecordHeader record_header {};
            if (!ReadExact(&input, &record_header) ||
                record_header.pinyin_bytes == 0 ||
                record_header.pinyin_bytes > kMaxPinyinBytes ||
                record_header.word_bytes == 0 ||
                record_header.word_bytes > kMaxWordBytes ||
                record_header.score == 0) {
                return false;
            }
            std::string pinyin;
            std::string word_utf8;
            std::wstring word;
            if (!ReadBytes(&input, record_header.pinyin_bytes, &pinyin) ||
                !ReadBytes(&input, record_header.word_bytes, &word_utf8) ||
                !IsValidPinyin(pinyin) ||
                !DecodeChineseWord(word_utf8, &word)) {
                return false;
            }
            if (!records.empty() &&
                !KeyLess(records.back().pinyin, records.back().word,
                         pinyin, word)) {
                return false;
            }
            records.push_back({std::move(pinyin), std::move(word),
                               record_header.score});
        }
        if (input.peek() != std::ifstream::traits_type::eof()) return false;
    } catch (...) {
        return false;
    }

    records_ = std::move(records);
    return true;
}

std::uint32_t SystemLexemePriorModel::Lookup(
    const std::string& pinyin, const std::wstring& word) const {
    const auto found = std::lower_bound(
        records_.begin(), records_.end(), pinyin,
        [&](const Record& record, const std::string& key_pinyin) {
            return KeyLess(record.pinyin, record.word, key_pinyin, word);
        });
    return found != records_.end() && found->pinyin == pinyin &&
            found->word == word
        ? found->score
        : 0;
}

}  // namespace shuru
