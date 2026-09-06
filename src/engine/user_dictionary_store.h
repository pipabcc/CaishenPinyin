#pragma once

#include "dictionary.h"
#include "../common/private_acl.h"
#include "../common/user_data_file.h"

#include <filesystem>
#include <sstream>

namespace shuru {

struct UserDictionaryChange {
    std::uint64_t sequence = 0;
    std::string pinyin;
    std::wstring word;
    int minimum_frequency = 0;
    std::int64_t time = 0;
    bool undo = false;
};

struct UserDictionaryState {
    Dictionary dictionary;
    std::string generation;
    std::string bigram_generation;
    UserDataFileStamp stamp;
    bool legacy = false;
};

inline void ApplyUserDictionaryChange(Dictionary* dictionary, const UserDictionaryChange& change) {
    if (change.undo) dictionary->DecreaseUserWord(change.pinyin, change.word, 20);
    else dictionary->IncreaseUserWord(change.pinyin, change.word, 20,
                                     change.minimum_frequency, change.time);
}

inline bool LoadUserDictionaryState(const std::wstring& path, UserDictionaryState* state) {
    if (state == nullptr) return false;
    std::string text;
    UserDictionaryState loaded;
    if (!ReadUserDataFile(path, &text, &loaded.stamp) ||
        !ParseUserDataGeneration(text, &loaded.generation)) return false;
    loaded.legacy = loaded.generation.empty();
    if (loaded.legacy) loaded.generation = LegacyUserDataGeneration(loaded.stamp);
    if (!ParseUserDataGeneration(text, &loaded.bigram_generation, kBigramGenerationPrefix)) return false;
    if (loaded.bigram_generation.empty()) loaded.bigram_generation = loaded.generation;
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    bool has_rows = false;
    while (std::getline(input, line)) {
        if (lines.empty() && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        const auto first = line.find_first_not_of(" \t\r");
        has_rows = has_rows || (first != std::string::npos && line[first] != '#');
        lines.push_back(std::move(line));
    }
    if (has_rows) {
        loaded.dictionary.BeginBulkLoad();
        const bool ok = loaded.dictionary.LoadFromUtf8Lines(lines, true);
        loaded.dictionary.EndBulkLoad();
        if (!ok) return false;
    }
    loaded.dictionary.clear_dirty();
    *state = std::move(loaded);
    return true;
}

inline bool InitializeUserDictionaryState(
    const std::wstring& path, const std::wstring& legacy_path, UserDictionaryState* state) {
    UserLearningFileLock lock;
    if (!lock.owns() || !EnsureCurrentUserOnlyPath(path, false) ||
        !LoadUserDictionaryState(path, state)) return false;
    if (state->stamp.present) return true;
    if (legacy_path != path && ReadUserDataFileStamp(legacy_path).present) {
        UserDictionaryState legacy;
        if (!LoadUserDictionaryState(legacy_path, &legacy)) return false;
        state->dictionary = std::move(legacy.dictionary);
    }
    state->generation = NewUserDataGeneration();
    state->bigram_generation = state->generation;
    if (state->generation.empty() || !state->dictionary.SaveUserToFile(path, state->generation, state->bigram_generation))
        return false;
    state->legacy = false;
    state->stamp = ReadUserDataFileStamp(path);
    return true;
}

inline bool PersistUserDictionaryChanges(
    const std::wstring& path, const std::string& generation,
    const std::vector<UserDictionaryChange>& changes, UserDictionaryState* saved) {
    UserLearningFileLock lock;
    if (!lock.owns() || !EnsureCurrentUserOnlyPath(path, false) ||
        !LoadUserDictionaryState(path, saved)) return false;
    // 清空和导入的代次优先于任何旧宿主的待保存操作。
    if (saved->generation != generation) return true;
    for (const auto& change : changes) ApplyUserDictionaryChange(&saved->dictionary, change);
    if (!saved->dictionary.SaveUserToFile(path, generation, saved->bigram_generation)) return false;
    // 写入成功即确认；后续身份查询失败不能导致同一批增量再次累加。
    saved->stamp = ReadUserDataFileStamp(path);
    saved->legacy = false;
    return true;
}

inline bool ReplaceUserDictionary(
    const std::wstring& path, const std::vector<UserDictionaryEntry>& entries,
    bool merge, UserDictionaryState* saved) {
    UserLearningFileLock lock;
    if (!lock.owns() || !EnsureCurrentUserOnlyPath(path, false)) return false;
    UserDictionaryState updated;
    if (merge && !LoadUserDictionaryState(path, &updated)) return false;
    updated.dictionary.ImportUserEntries(entries);
    updated.generation = NewUserDataGeneration();
    if (!merge || updated.bigram_generation.empty()) updated.bigram_generation = updated.generation;
    if (updated.generation.empty() ||
        !updated.dictionary.SaveUserToFile(path, updated.generation, updated.bigram_generation)) return false;
    updated.stamp = ReadUserDataFileStamp(path);
    updated.legacy = false;
    *saved = std::move(updated);
    if (!merge) {
        const auto bigram = (std::filesystem::path(path).parent_path() / L"user_bigram.txt").wstring();
        if (!DeleteFileW(bigram.c_str())) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
    }
    return true;
}

}  // namespace shuru
