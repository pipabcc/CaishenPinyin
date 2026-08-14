#include "engine/system_lexeme_prior.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t record_count;
};

struct RecordHeader {
    std::uint16_t pinyin_bytes;
    std::uint16_t word_bytes;
    std::uint32_t score;
};
#pragma pack(pop)

struct Record {
    std::string pinyin;
    std::string word;
    std::uint32_t score;
};

bool WriteModel(
    const std::filesystem::path& path,
    const std::vector<Record>& records,
    bool valid_magic = true) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Header header {{'C', 'S', 'L', 'X', 'P', 'R', '1', '\0'}, 1,
                   static_cast<std::uint32_t>(records.size())};
    if (!valid_magic) header.magic[0] = 'X';
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& record : records) {
        const RecordHeader item {
            static_cast<std::uint16_t>(record.pinyin.size()),
            static_cast<std::uint16_t>(record.word.size()), record.score};
        output.write(reinterpret_cast<const char*>(&item), sizeof(item));
        output.write(record.pinyin.data(),
                     static_cast<std::streamsize>(record.pinyin.size()));
        output.write(record.word.data(),
                     static_cast<std::streamsize>(record.word.size()));
    }
    return static_cast<bool>(output);
}

}  // namespace

int wmain() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        (L"CaishenSystemLexemePrior-" +
         std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) return 1;

    const std::vector<Record> valid_records {
        {"xian", u8"现", 200},
        {"xiang", u8"想", 300},
    };
    const fs::path valid = root / L"valid.bin";
    if (!WriteModel(valid, valid_records)) return 2;

    shuru::SystemLexemePriorModel model;
    if (!model.LoadFromFile(valid.wstring()) || model.size() != 2 ||
        model.Lookup("xiang", L"想") != 300 ||
        model.Lookup("xiang", L"相") != 0) {
        return 3;
    }

    const fs::path bad_magic = root / L"bad-magic.bin";
    if (!WriteModel(bad_magic, valid_records, false) ||
        model.LoadFromFile(bad_magic.wstring()) || model.size() != 2) {
        return 4;
    }

    const fs::path truncated = root / L"truncated.bin";
    fs::copy_file(valid, truncated, fs::copy_options::overwrite_existing, error);
    fs::resize_file(truncated, fs::file_size(truncated) - 1, error);
    if (error || model.LoadFromFile(truncated.wstring()) || model.size() != 2) {
        return 5;
    }

    const fs::path unordered = root / L"unordered.bin";
    if (!WriteModel(unordered, {valid_records[1], valid_records[0]}) ||
        model.LoadFromFile(unordered.wstring()) || model.size() != 2) {
        return 6;
    }

    const fs::path zero_score = root / L"zero-score.bin";
    if (!WriteModel(zero_score, {{"xian", u8"现", 0}}) ||
        model.LoadFromFile(zero_score.wstring()) || model.size() != 2) {
        return 7;
    }

    fs::remove_all(root, error);
    std::wcout << L"system_lexeme_prior: OK\n";
    return 0;
}
