#include "engine/english_dict.h"
#include "common/com_utils.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool CheckCandidate(
    const std::vector<shuru::Candidate>& candidates,
    const std::wstring& text,
    const std::string& key) {
    return !candidates.empty() && candidates.front().text == text &&
        candidates.front().pinyin == key && candidates.front().is_english;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: english_dict_test <en_dict.txt>\n";
        return 2;
    }
    shuru::EnglishDictionary dictionary;
    if (!dictionary.LoadFromFile(std::filesystem::path(argv[1]).wstring()) ||
        dictionary.Size() < 200000) {
        std::cerr << "expanded English dictionary did not load\n";
        return 1;
    }
    if (!CheckCandidate(dictionary.LookupExact("easy"), L"easy", "easy") ||
        !CheckCandidate(dictionary.LookupExact("ENGLISH"), L"English", "english") ||
        !CheckCandidate(dictionary.LookupPrefix("engli", 9), L"English", "english")) {
        std::cerr << "English exact/prefix lookup or canonical casing failed\n";
        const auto prefix = dictionary.LookupPrefix("engli", 9);
        for (const auto& candidate : prefix) {
            std::cerr << candidate.pinyin << ':' << candidate.frequency << ',';
        }
        std::cerr << '\n';
        return 1;
    }
    std::cout << "english_dictionary: OK entries=" << dictionary.Size() << '\n';
    return 0;
}
