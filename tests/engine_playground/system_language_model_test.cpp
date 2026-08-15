#include "engine/system_language_model.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t bigram_count;
    std::uint32_t trigram_count;
};

struct Bigram {
    std::uint32_t key;
    std::uint32_t count;
};

struct Trigram {
    std::uint64_t key;
    std::uint32_t count;
};
#pragma pack(pop)

std::uint32_t BigramKey(wchar_t first, wchar_t second) {
    return (static_cast<std::uint32_t>(first) << 16) |
        static_cast<std::uint16_t>(second);
}

std::uint64_t TrigramKey(wchar_t first, wchar_t second, wchar_t third) {
    return (static_cast<std::uint64_t>(first) << 32) |
        (static_cast<std::uint64_t>(second) << 16) |
        static_cast<std::uint16_t>(third);
}

bool WriteValidModel(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const Header header {{'C', 'S', 'N', 'G', 'R', 'M', '1', '\0'}, 1, 1, 1};
    const Bigram bigram {BigramKey(L'北', L'京'), 20};
    const Trigram trigram {TrigramKey(L'去', L'北', L'京'), 10};
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&bigram), sizeof(bigram));
    output.write(reinterpret_cast<const char*>(&trigram), sizeof(trigram));
    return static_cast<bool>(output);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        (L"CaishenSystemLanguageModel-" +
         std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) return 1;

    const fs::path valid = root / L"valid.bin";
    if (!WriteValidModel(valid)) return 2;
    shuru::SystemLanguageModel model;
    if (!model.LoadFromFile(valid.wstring()) ||
        model.bigram_size() != 1 || model.trigram_size() != 1 ||
        model.AppendScore(L"去北", L"京") <= 0.0) {
        return 3;
    }

    const fs::path bad_magic = root / L"bad-magic.bin";
    fs::copy_file(valid, bad_magic, fs::copy_options::overwrite_existing, error);
    {
        std::fstream file(bad_magic, std::ios::binary | std::ios::in | std::ios::out);
        file.put('X');
    }
    if (model.LoadFromFile(bad_magic.wstring()) ||
        model.bigram_size() != 1 || model.trigram_size() != 1) {
        return 4;
    }

    const fs::path truncated = root / L"truncated.bin";
    fs::copy_file(valid, truncated, fs::copy_options::overwrite_existing, error);
    fs::resize_file(truncated, sizeof(Header) + sizeof(Bigram), error);
    if (error || model.LoadFromFile(truncated.wstring()) ||
        model.bigram_size() != 1 || model.trigram_size() != 1) {
        return 5;
    }

    if (argc >= 2) {
        const fs::path grammar_path(argv[1]);
        shuru::SystemLanguageModel grammar;
        if (!grammar.LoadFromFile(grammar_path.wstring()) ||
            !grammar.is_grammar() || grammar.grammar_unit_count() == 0 ||
            grammar.mapped_bytes() != fs::file_size(grammar_path, error) || error) {
            return 6;
        }
        const double known = grammar.AppendScore(L"查缺", L"补漏");
        const double beauty = grammar.AppendScore(L"学校里有一个", L"美女", true);
        const double yearly = grammar.AppendScore(L"学校里有一个", L"每年", true);
        const double missing = grammar.AppendScore(L"不存在的上下文", L"不存在的词");
        std::cout << "grammar units=" << grammar.grammar_unit_count()
                  << " bytes=" << grammar.mapped_bytes()
                  << " known=" << known << " beauty=" << beauty
                  << " yearly=" << yearly << " missing=" << missing << '\n';
        if (!std::isfinite(known) || !std::isfinite(missing) ||
            known <= -4.0 || beauty <= yearly || beauty <= -4.0 ||
            missing != -4.0) {
            return 7;
        }
    }

    fs::remove_all(root, error);
    std::wcout << L"system_language_model: OK\n";
    return 0;
}
