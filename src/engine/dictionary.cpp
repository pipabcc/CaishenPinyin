#include "dictionary.h"
#include "lexicon_cache.h"

#include "pinyin_syllables.h"
#include "pinyin_lattice.h"
#include "fuzzy_pinyin.h"
#include "../common/com_utils.h"
#include "../common/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace shuru {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string NormalizePinyin(std::string pinyin) {
    for (char& c : pinyin) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return pinyin;
}

bool EnsureParentDir(const std::wstring& path) {
    try {
        const std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        return true;
    } catch (...) {
        return false;
    }
}

int SaturatingAdd(int value, int delta) {
    if (delta > 0 && value > (std::numeric_limits<int>::max)() - delta) {
        return (std::numeric_limits<int>::max)();
    }
    if (delta < 0 && value < (std::numeric_limits<int>::min)() - delta) {
        return (std::numeric_limits<int>::min)();
    }
    return value + delta;
}

}  // namespace

void Dictionary::EnsureTrieRoot() {
    if (trie_.empty()) {
        trie_.emplace_back();
    }
}

void Dictionary::TrieInsert(const std::string& pinyin) {
    EnsureTrieRoot();
    int node = 0;
    for (unsigned char ch : pinyin) {
        if (ch < 'a' || ch > 'z') {
            return;
        }
        const int idx = ch - 'a';
        if (trie_[node].child[idx] < 0) {
            trie_[node].child[idx] = static_cast<int>(trie_.size());
            trie_.emplace_back();
        }
        node = trie_[node].child[idx];
    }
    auto& terms = trie_[node].terminals;
    if (std::find(terms.begin(), terms.end(), pinyin) == terms.end()) {
        terms.push_back(pinyin);
    }
}

void Dictionary::CollectTrieSubtree(int node, size_t limit, std::vector<std::string>* out_keys) const {
    if (!out_keys || node < 0 || node >= static_cast<int>(trie_.size())) {
        return;
    }
    for (const auto& k : trie_[node].terminals) {
        out_keys->push_back(k);
        if (out_keys->size() >= limit) {
            return;
        }
    }
    for (int c = 0; c < 26; ++c) {
        const int child = trie_[node].child[c];
        if (child >= 0) {
            CollectTrieSubtree(child, limit, out_keys);
            if (out_keys->size() >= limit) {
                return;
            }
        }
    }
}

void Dictionary::CollectTriePrefix(const std::string& prefix, size_t limit, std::vector<std::string>* out_keys) const {
    if (!out_keys || trie_.empty()) {
        return;
    }
    int node = 0;
    for (unsigned char ch : prefix) {
        if (ch < 'a' || ch > 'z') {
            return;
        }
        const int idx = ch - 'a';
        const int child = trie_[node].child[idx];
        if (child < 0) {
            return;
        }
        node = child;
    }
    CollectTrieSubtree(node, limit, out_keys);
}

bool Dictionary::LoadFromFile(const std::wstring& path, bool from_user) {
    if (!from_user) {
        const std::wstring cache = path + L".bin";
        size_t row_count = 0;
        if (VisitLexiconCache(path, cache, [&](CachedLexiconLine&& row) {
                AddWord(row.pinyin, Utf8ToWide(row.word_utf8), row.frequency, false);
                return true;
            }, &row_count)) {
            RebuildJianpinIndex();
            SHURU_LOG_INFO("validated streaming lexicon cache loaded entries=%zu", row_count);
            return row_count != 0;
        }
    }
    std::ifstream in{std::filesystem::path(path)};
    if (!in) {
        SHURU_LOG_ERROR("failed to open dict: path_len=%zu", path.size());
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    const bool ok = LoadFromUtf8Lines(lines, from_user);
    if (ok && !from_user) {
        // 损坏、过期或缺失时文本始终可用；缓存重建失败不影响初始化。
        BuildLexiconCache(path, path + L".bin");
    }
    return ok;
}

bool Dictionary::LoadFromUtf8Lines(const std::vector<std::string>& lines, bool from_user) {
    size_t loaded = 0;
    for (const auto& raw : lines) {
        const std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // skip UTF-8 BOM residual on first field
        std::string pinyin;
        std::string word_utf8;
        int frequency = 1;
        int selection_count = 0;
        std::int64_t last_used_unix = 0;
        std::stringstream ss(line);
        if (!std::getline(ss, pinyin, '\t')) {
            continue;
        }
        if (!pinyin.empty() && static_cast<unsigned char>(pinyin[0]) == 0xEF) {
            // bom handled if whole line had bom - trim weird prefix
        }
        if (!std::getline(ss, word_utf8, '\t')) {
            continue;
        }
        std::string freq_text;
        if (std::getline(ss, freq_text, '\t')) {
            try {
                frequency = std::stoi(Trim(freq_text));
            } catch (...) {
                frequency = 1;
            }
        }
        std::string count_text;
        std::string last_used_text;
        if (from_user && std::getline(ss, count_text, '\t')) {
            try { selection_count = (std::max)(0, std::stoi(Trim(count_text))); } catch (...) { continue; }
            if (std::getline(ss, last_used_text, '\t')) {
                try { last_used_unix = (std::max)(std::int64_t{0}, std::stoll(Trim(last_used_text))); } catch (...) { continue; }
            }
        }
        pinyin = NormalizePinyin(Trim(pinyin));
        // strip BOM if attached to pinyin
        while (!pinyin.empty() && static_cast<unsigned char>(pinyin[0]) > 127) {
            pinyin.erase(pinyin.begin());
        }
        word_utf8 = Trim(word_utf8);
        if (pinyin.empty() || word_utf8.empty()) {
            continue;
        }
        const std::wstring word = Utf8ToWide(word_utf8);
        if (word.empty() || frequency < 0) continue;
        AddWord(pinyin, word, frequency, from_user);
        if (from_user) {
            auto& meta = user_entries_[{pinyin, word}];
            meta = UserDictionaryEntry{pinyin, word, frequency, selection_count, last_used_unix};
            auto& bucket = map_[pinyin];
            for (auto& item : bucket) if (item.word == word) {
                item.selection_count = selection_count;
                item.last_used_unix = last_used_unix;
            }
        }
        ++loaded;
    }
    RebuildJianpinIndex();
    SHURU_LOG_INFO("dictionary loaded entries=%zu keys=%zu jianpin=%zu trie=%zu user=%d",
                   loaded, map_.size(), jianpin_index_.size(), trie_.size(), from_user ? 1 : 0);
    return loaded > 0;
}

void Dictionary::IndexPinyinKey(const std::string& pinyin) {
    TrieInsert(pinyin);
    const std::string jp = pinyin_data::ToJianpin(pinyin);
    if (jp.empty()) {
        return;
    }
    auto& list = jianpin_index_[jp];
    if (std::find(list.begin(), list.end(), pinyin) == list.end()) {
        list.push_back(pinyin);
    }
}

void Dictionary::RebuildJianpinSortedKeys() {
    jianpin_keys_sorted_.clear();
    jianpin_keys_sorted_.reserve(jianpin_index_.size());
    for (const auto& kv : jianpin_index_) {
        jianpin_keys_sorted_.push_back(kv.first);
    }
    std::sort(jianpin_keys_sorted_.begin(), jianpin_keys_sorted_.end());
}

void Dictionary::RebuildJianpinIndex() {
    jianpin_index_.clear();
    trie_.clear();
    EnsureTrieRoot();
    for (const auto& kv : map_) {
        IndexPinyinKey(kv.first);
    }
    RebuildJianpinSortedKeys();
}

void Dictionary::RebuildWordPinyinIndex() {
    word_pinyins_.clear();
    for (const auto& [pinyin, entries] : map_) {
        for (const auto& entry : entries) {
            auto& values = word_pinyins_[entry.word];
            if (std::find(values.begin(), values.end(), pinyin) == values.end())
                values.push_back(pinyin);
        }
    }
}

void Dictionary::AddWord(const std::string& pinyin, const std::wstring& word, int frequency, bool from_user) {
    if (pinyin.empty() || word.empty()) {
        return;
    }
    const std::string key = NormalizePinyin(pinyin);
    auto& word_pinyins = word_pinyins_[word];
    if (std::find(word_pinyins.begin(), word_pinyins.end(), key) == word_pinyins.end())
        word_pinyins.push_back(key);
    auto& entries = map_[key];
    for (auto& entry : entries) {
        if (entry.word == word) {
            entry.frequency = (std::max)(entry.frequency, frequency);
            entry.from_user = entry.from_user || from_user;
            const int updated_frequency = entry.frequency;
            SortEntries(entries);
            if (from_user) {
                user_entries_[{key, word}] = UserDictionaryEntry{key, word, updated_frequency, entry.selection_count, entry.last_used_unix};
                dirty_ = true;
            }
            return;
        }
    }
    const bool is_new_key = entries.empty();
    entries.push_back(Entry{word, frequency, from_user});
    SortEntries(entries);
    if (from_user) {
        user_entries_[{key, word}] = UserDictionaryEntry{key, word, frequency, 0, 0};
    }
    if (is_new_key) {
        IndexPinyinKey(key);
        // 增量维护 sorted jianpin（全量重建成本高，这里简单 insert）
        const std::string jp = pinyin_data::ToJianpin(key);
        if (!jp.empty()) {
            auto it = std::lower_bound(jianpin_keys_sorted_.begin(), jianpin_keys_sorted_.end(), jp);
            if (it == jianpin_keys_sorted_.end() || *it != jp) {
                jianpin_keys_sorted_.insert(it, jp);
            }
        }
    }
    if (from_user) {
        dirty_ = true;
    }
}

void Dictionary::IncreaseUserWord(
    const std::string& pinyin,
    const std::wstring& word,
    int delta,
    int minimum_frequency,
    std::int64_t now_unix) {
    if (pinyin.empty() || word.empty()) {
        return;
    }
    const int increment = (std::max)(delta, 1);
    if (now_unix <= 0) now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int frequency_floor = (std::max)(minimum_frequency, 0);
    const std::string key = NormalizePinyin(pinyin);
    auto& entries = map_[key];
    for (auto& entry : entries) {
        if (entry.word == word) {
            entry.frequency = SaturatingAdd(
                (std::max)(entry.frequency, frequency_floor), increment);
            entry.from_user = true;
            entry.selection_count = SaturatingAdd(entry.selection_count, 1);
            entry.last_used_unix = now_unix;
            const int updated_frequency = entry.frequency;
            SortEntries(entries);
            user_entries_[{key, word}] = UserDictionaryEntry{key, word, updated_frequency, entry.selection_count, entry.last_used_unix};
            dirty_ = true;
            return;
        }
    }
    const bool is_new_key = entries.empty();
    const int initial_frequency = SaturatingAdd(frequency_floor, increment);
    entries.push_back(Entry{word, initial_frequency, true, 1, now_unix});
    SortEntries(entries);
    user_entries_[{key, word}] = UserDictionaryEntry{key, word, initial_frequency, 1, now_unix};
    if (is_new_key) {
        IndexPinyinKey(key);
        const std::string jp = pinyin_data::ToJianpin(key);
        if (!jp.empty()) {
            auto it = std::lower_bound(jianpin_keys_sorted_.begin(), jianpin_keys_sorted_.end(), jp);
            if (it == jianpin_keys_sorted_.end() || *it != jp) {
                jianpin_keys_sorted_.insert(it, jp);
            }
        }
    }
    dirty_ = true;
}

int Dictionary::LookupFrequency(
    const std::string& pinyin,
    const std::wstring& word) const {
    const auto found = map_.find(NormalizePinyin(pinyin));
    if (found == map_.end()) {
        return 0;
    }
    for (const Entry& entry : found->second) {
        if (entry.word == word) {
            return entry.frequency;
        }
    }
    return 0;
}

bool Dictionary::ContainsWord(const std::wstring& word) const {
    return word_pinyins_.find(word) != word_pinyins_.end();
}

bool Dictionary::ContainsWordPinyin(
    const std::wstring& word,
    const std::string& pinyin) const {
    const auto word_it = word_pinyins_.find(word);
    if (word_it == word_pinyins_.end()) return false;
    const std::string normalized = NormalizePinyin(pinyin);
    return std::find(word_it->second.begin(), word_it->second.end(), normalized) !=
        word_it->second.end();
}


int Dictionary::ComputeLearningScore(int selection_count, std::int64_t last_used_unix, std::int64_t now_unix) {
    if (selection_count <= 0) return 0;
    if (now_unix <= 0) now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const double age_days = last_used_unix > 0 ? (std::max)(0.0, double(now_unix - last_used_unix) / 86400.0) : 180.0;
    const double recency = std::exp(-age_days / 30.0);
    const double evidence = std::log2(1.0 + (std::min)(selection_count, 1024));
    return (std::min)(90, static_cast<int>(std::lround(12.0 * evidence * (0.25 + 0.75 * recency))));
}

bool Dictionary::DecreaseUserWord(const std::string& pinyin, const std::wstring& word, int delta) {
    const std::string key = NormalizePinyin(pinyin);
    auto user = user_entries_.find({key, word});
    auto bucket = map_.find(key);
    if (user == user_entries_.end() || bucket == map_.end()) return false;
    for (auto it = bucket->second.begin(); it != bucket->second.end(); ++it) {
        if (it->word != word || !it->from_user) continue;
        it->frequency -= (std::max)(1, delta);
        it->selection_count = (std::max)(0, it->selection_count - 1);
        if (it->frequency <= 0 || it->selection_count <= 0) {
            bucket->second.erase(it);
            user_entries_.erase(user);
        } else {
            user->second.frequency = it->frequency;
            user->second.selection_count = it->selection_count;
            user->second.last_used_unix = it->last_used_unix;
            SortEntries(bucket->second);
        }
        RebuildWordPinyinIndex();
        dirty_ = true;
        return true;
    }
    return false;
}

void Dictionary::ClearUserEntries() {
    for (auto map_it = map_.begin(); map_it != map_.end();) {
        auto& entries = map_it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [](const Entry& entry) { return entry.from_user; }), entries.end());
        if (entries.empty()) map_it = map_.erase(map_it); else ++map_it;
    }
    user_entries_.clear();
    RebuildJianpinIndex();
    RebuildWordPinyinIndex();
    dirty_ = true;
}

bool Dictionary::SaveUserToFile(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (!EnsureParentDir(path)) {
        SHURU_LOG_ERROR("SaveUserToFile create dir failed");
        return false;
    }
    std::vector<UserDictionaryEntry> rows;
    rows.reserve(user_entries_.size());
    for (const auto& entry : user_entries_) {
        rows.push_back(entry.second);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.selection_count != b.selection_count) return a.selection_count > b.selection_count;
        if (a.pinyin != b.pinyin) return a.pinyin < b.pinyin;
        return a.word < b.word;
    });
    const std::filesystem::path target_path(path);
    const std::filesystem::path temp_path = target_path.wstring() +
        L".tmp." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetCurrentThreadId());
    std::ofstream out{temp_path, std::ios::binary | std::ios::trunc};
    if (!out) {
        SHURU_LOG_ERROR("SaveUserToFile open failed");
        return false;
    }
    out << "\xEF\xBB\xBF";
    out << "# 发财拼音用户词库\n";
    out << "# v2: pinyin<TAB>词<TAB>词频<TAB>selection_count<TAB>last_used_unix\n";
    for (const auto& row : rows) {
        out << row.pinyin << '\t' << WideToUtf8(row.word) << '\t' << row.frequency << '\t' << row.selection_count << '\t' << row.last_used_unix << '\n';
    }
    out.flush();
    if (!out) {
        out.close();
        DeleteFileW(temp_path.c_str());
        SHURU_LOG_ERROR("SaveUserToFile write failed");
        return false;
    }
    out.close();
    if (!MoveFileExW(
            temp_path.c_str(),
            target_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp_path.c_str());
        SHURU_LOG_ERROR("SaveUserToFile replace failed: error=%lu", GetLastError());
        return false;
    }
    dirty_ = false;
    SHURU_LOG_INFO("SaveUserToFile ok count=%zu", rows.size());
    return true;
}

std::vector<UserDictionaryEntry> Dictionary::SnapshotUserEntries() const {
    std::vector<UserDictionaryEntry> entries;
    entries.reserve(user_entries_.size());
    for (const auto& entry : user_entries_) {
        entries.push_back(entry.second);
    }
    return entries;
}

void Dictionary::ImportUserEntries(const std::vector<UserDictionaryEntry>& entries) {
    const bool was_dirty = dirty_;
    for (const auto& entry : entries) {
        AddWord(entry.pinyin, entry.word, entry.frequency, true);
        auto& meta = user_entries_[{NormalizePinyin(entry.pinyin), entry.word}];
        if (entry.frequency > meta.frequency || entry.selection_count > meta.selection_count || entry.last_used_unix > meta.last_used_unix)
            meta = entry;
        auto& bucket = map_[NormalizePinyin(entry.pinyin)];
        for (auto& item : bucket) if (item.word == entry.word) {
            item.frequency = (std::max)(item.frequency, entry.frequency);
            item.selection_count = (std::max)(item.selection_count, entry.selection_count);
            item.last_used_unix = (std::max)(item.last_used_unix, entry.last_used_unix);
        }
    }
    // 导入的是已持久化数据，不应触发另一轮保存。
    dirty_ = was_dirty;
}

std::vector<Candidate> Dictionary::LookupExact(const std::string& pinyin) const {
    const auto it = map_.find(NormalizePinyin(pinyin));
    if (it == map_.end()) {
        return {};
    }
    return ToCandidates(it->first, it->second);
}

std::vector<Candidate> Dictionary::LookupPrefix(const std::string& pinyin_prefix, size_t limit) const {
    const std::string prefix = NormalizePinyin(pinyin_prefix);
    if (prefix.empty() || limit == 0) {
        return {};
    }
    std::vector<std::string> keys;
    // 引擎已经传入候选预算。这里只保留有限的跨词条超采样，避免
    // UI、引擎、词典三层逐级放大扫描量。
    const size_t key_limit = (std::min)(size_t{256},
        limit > (std::numeric_limits<size_t>::max)() / 2 ? limit : limit * 2);
    keys.reserve(key_limit);
    CollectTriePrefix(prefix, key_limit, &keys);

    std::vector<Candidate> all;
    for (const auto& key : keys) {
        const auto it = map_.find(key);
        if (it == map_.end()) {
            continue;
        }
        auto part = ToCandidates(it->first, it->second);
        all.insert(all.end(), part.begin(), part.end());
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) {
        if (a.frequency != b.frequency) {
            return a.frequency > b.frequency;
        }
        return a.text < b.text;
    });
    if (all.size() > limit) {
        all.resize(limit);
    }
    return all;
}

std::vector<Candidate> Dictionary::LookupJianpin(const std::string& jianpin, size_t limit) const {
    const std::string jp = NormalizePinyin(jianpin);
    if (jp.empty()) {
        return {};
    }
    std::vector<Candidate> all;
    auto append_full = [&](const std::string& full_py) {
        const auto map_it = map_.find(full_py);
        if (map_it == map_.end()) {
            return;
        }
        auto part = ToCandidates(full_py, map_it->second);
        all.insert(all.end(), part.begin(), part.end());
    };

    // 精确简拼 O(1)
    const auto exact = jianpin_index_.find(jp);
    if (exact != jianpin_index_.end()) {
        for (const auto& full_py : exact->second) {
            append_full(full_py);
            if (all.size() > limit * 8) {
                break;
            }
        }
    }

    // 前缀：有序键 lower_bound，避免全表扫描
    if (all.size() < limit && jp.size() <= 4 && !jianpin_keys_sorted_.empty()) {
        auto it = std::lower_bound(jianpin_keys_sorted_.begin(), jianpin_keys_sorted_.end(), jp);
        for (; it != jianpin_keys_sorted_.end(); ++it) {
            if (it->size() < jp.size() || it->compare(0, jp.size(), jp) != 0) {
                break;
            }
            if (*it == jp) {
                continue;
            }
            const auto jit = jianpin_index_.find(*it);
            if (jit == jianpin_index_.end()) {
                continue;
            }
            for (const auto& full_py : jit->second) {
                append_full(full_py);
                if (all.size() > limit * 8) {
                    break;
                }
            }
            if (all.size() > limit * 8) {
                break;
            }
        }
    }

    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) {
        if (a.from_user != b.from_user) {
            return a.from_user > b.from_user;
        }
        if (a.frequency != b.frequency) {
            return a.frequency > b.frequency;
        }
        if (a.pinyin.size() != b.pinyin.size()) {
            return a.pinyin.size() < b.pinyin.size();
        }
        return a.text < b.text;
    });

    std::vector<Candidate> unique;
    unique.reserve((std::min)(limit, all.size()));
    for (const auto& c : all) {
        bool exists = false;
        for (const auto& u : unique) {
            if (u.text == c.text) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            unique.push_back(c);
            if (unique.size() >= limit) {
                break;
            }
        }
    }
    return unique;
}

std::vector<Candidate> Dictionary::LookupMixed(const std::string& input, size_t limit) const {
    const std::string pattern = NormalizePinyin(input);
    if (pattern.size() < 2 || limit == 0) return {};
    std::vector<Candidate> all;
    for (const auto& kv : map_) {
        // 混合拼写只由完整音节和声母组成，因此输入必须与完整拼音首字母相同，
        // 且是其有序子序列。先做廉价过滤，避免为几乎每个词条构建音节词图。
        if (kv.first.size() < pattern.size() || kv.first.front() != pattern.front()) continue;
        size_t subsequence = 0;
        for (const char ch : kv.first) {
            if (ch == pattern[subsequence] && ++subsequence == pattern.size()) break;
        }
        if (subsequence != pattern.size()) continue;
        const auto paths = pinyin_data::BuildSyllableLattice(kv.first, 16);
        bool matches = false;
        for (const auto& path : paths) {
            if (!path.complete || path.covered != kv.first.size()) continue;
            size_t cursor = 0;
            bool used_short = false;
            for (const auto& edge : path.edges) {
                if (cursor >= pattern.size()) { matches = true; break; }
                const std::string& syl = edge.syllable;
                size_t full = 0;
                while (full < syl.size() && cursor + full < pattern.size() && pattern[cursor + full] == syl[full]) ++full;
                if (full == syl.size()) cursor += full;
                else if (pattern[cursor] == syl[0]) { ++cursor; used_short = true; }
                else { cursor = pattern.size() + 1; break; }
            }
            matches = cursor == pattern.size() && used_short;
            if (matches) break;
        }
        if (!matches) continue;
        auto part = ToCandidates(kv.first, kv.second);
        all.insert(all.end(), part.begin(), part.end());
    }
    std::sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) {
        if (a.learning_score != b.learning_score) return a.learning_score > b.learning_score;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        if (a.pinyin.size() != b.pinyin.size()) return a.pinyin.size() < b.pinyin.size();
        return a.text < b.text;
    });
    if (all.size() > limit) all.resize(limit);
    return all;
}

size_t Dictionary::Size() const {
    size_t n = 0;
    for (const auto& kv : map_) {
        n += kv.second.size();
    }
    return n;
}

size_t Dictionary::JianpinSize() const {
    return jianpin_index_.size();
}


size_t Dictionary::DeriveSingleCharacters() {
    // 快照，避免遍历中修改 map_
    struct Item {
        std::string py;
        std::wstring word;
        int frequency;
    };
    std::vector<Item> items;
    items.reserve(map_.size() * 2);
    for (const auto& kv : map_) {
        for (const auto& e : kv.second) {
            items.push_back(Item{kv.first, e.word, e.frequency});
        }
    }

    // py -> (char -> best freq)
    std::unordered_map<std::string, std::unordered_map<std::wstring, int>> singles;
    size_t paired = 0;
    for (const auto& it : items) {
        if (it.word.empty() || it.py.empty()) {
            continue;
        }
        // 仅 BMP 汉字按 UTF-16 码元计长度（词库几乎都是 BMP）
        const size_t nchar = it.word.size();
        if (nchar == 0 || nchar > 8) {
            continue;
        }
        const auto segs = SegmentPinyin(it.py);
        if (segs.size() != nchar) {
            continue;
        }
        size_t cover = 0;
        bool ok = true;
        for (const auto& s : segs) {
            if (s.empty() || pinyin_data::Syllables().find(s) == pinyin_data::Syllables().end()) {
                ok = false;
                break;
            }
            cover += s.size();
        }
        if (!ok || cover != it.py.size()) {
            continue;
        }
        ++paired;
        for (size_t i = 0; i < nchar; ++i) {
            std::wstring ch(1, it.word[i]);
            auto& slot = singles[segs[i]][ch];
            if (it.frequency > slot) {
                slot = it.frequency;
            }
        }
    }

    size_t added_or_boosted = 0;
    for (const auto& py_kv : singles) {
        for (const auto& ch_kv : py_kv.second) {
            // 单字保底权重，避免被超高频词组完全淹没前仍可进 exact 桶
            const int freq = (std::max)(ch_kv.second, 1000);
            const auto before = map_.find(py_kv.first);
            bool existed = false;
            if (before != map_.end()) {
                for (const auto& e : before->second) {
                    if (e.word == ch_kv.first) {
                        existed = true;
                        break;
                    }
                }
            }
            AddWord(py_kv.first, ch_kv.first, freq, false);
            if (!existed) {
                ++added_or_boosted;
            }
        }
    }
    RebuildJianpinIndex();
    SHURU_LOG_INFO("DeriveSingleCharacters paired_words=%zu pure_syllable_keys=%zu new_singles~=%zu",
                   paired, singles.size(), added_or_boosted);
    return added_or_boosted;
}

void Dictionary::SortEntries(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.from_user != b.from_user) {
            return a.from_user > b.from_user;
        }
        if (a.frequency != b.frequency) {
            return a.frequency > b.frequency;
        }
        return a.word < b.word;
    });
}

std::vector<Candidate> Dictionary::ToCandidates(const std::string& pinyin, const std::vector<Entry>& entries) {
    std::vector<Candidate> out;
    out.reserve(entries.size());
    for (const auto& entry : entries) {
        Candidate c;
        c.text = entry.word;
        c.pinyin = pinyin;
        c.frequency = entry.frequency;
        c.selection_count = entry.selection_count;
        c.last_used_unix = entry.last_used_unix;
        c.learning_score = ComputeLearningScore(entry.selection_count, entry.last_used_unix);
        c.from_user = entry.from_user;
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace shuru
