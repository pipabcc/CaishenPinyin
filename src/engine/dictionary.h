#pragma once

#include "candidate.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace shuru {

struct UserDictionaryEntry {
    std::string pinyin;
    std::wstring word;
    int frequency = 0;
    int selection_count = 0;
    std::int64_t last_used_unix = 0;
};

struct MixedPrefixMatch {
    Candidate candidate;
    size_t consumed_input = 0;
    size_t abbreviated_syllables = 0;
    size_t omitted_letters = 0;
    size_t syllable_count = 0;
    std::string segmented_input;
};

class Dictionary {
public:
    bool LoadFromFile(const std::wstring& path, bool from_user = false);
    bool LoadFromUtf8Lines(const std::vector<std::string>& lines, bool from_user = false);

    // 批量装载模式：期间 AddWord 跳过桶内排序与简拼/trie 增量索引维护，
    // EndBulkLoad 统一排序并重建一次索引。仅用于初始加载，加载耗时从
    // 每插入一次 sort 整桶的 O(N·B·logB) 降为一次性 O(N·logB)。
    void BeginBulkLoad();
    void EndBulkLoad();

    void AddWord(const std::string& pinyin, const std::wstring& word, int frequency, bool from_user);
    void IncreaseUserWord(
        const std::string& pinyin,
        const std::wstring& word,
        int delta = 10,
        int minimum_frequency = 0,
        std::int64_t now_unix = 0);
    int LookupFrequency(const std::string& pinyin, const std::wstring& word) const;
    bool ContainsWord(const std::wstring& word) const;
    bool ContainsWordPinyin(const std::wstring& word, const std::string& pinyin) const;
    bool DecreaseUserWord(const std::string& pinyin, const std::wstring& word, int delta = 20);
    static int ComputeLearningScore(int selection_count, std::int64_t last_used_unix, std::int64_t now_unix = 0);
    void ClearUserEntries();

    bool SaveUserToFile(const std::wstring& path) const;
    std::vector<UserDictionaryEntry> SnapshotUserEntries() const;
    void ImportUserEntries(const std::vector<UserDictionaryEntry>& entries);
    bool dirty() const { return dirty_; }
    void clear_dirty() const { dirty_ = false; }

    std::vector<Candidate> LookupExact(const std::string& pinyin) const;
    std::vector<Candidate> LookupPrefix(const std::string& pinyin_prefix, size_t limit = 64) const;
    std::vector<Candidate> LookupJianpin(const std::string& jianpin, size_t limit = 64) const;
    std::vector<Candidate> LookupMixed(const std::string& input, size_t limit = 64) const;
    std::vector<MixedPrefixMatch> LookupMixedPrefixes(
        const std::string& input, size_t limit = 64) const;

    size_t Size() const;
    size_t JianpinSize() const;

    // 从多字词按音节对齐反推单字，解决 sun 只有「损失」没有「孙/损」
    size_t DeriveSingleCharacters();

private:
    struct Entry {
        std::wstring word;
        int frequency = 0;
        bool from_user = false;
        int selection_count = 0;
        std::int64_t last_used_unix = 0;
    };

    struct TrieNode {
        int child[26] {};
        // 以该节点结尾的完整拼音（通常 0/1 个，重码少）
        std::vector<std::string> terminals;
        TrieNode() {
            for (int& c : child) {
                c = -1;
            }
        }
    };

    struct SyllableTrieNode {
        struct Child {
            std::string syllable;
            int node = -1;
        };
        struct Terminal {
            std::string pinyin;
            size_t syllable_count = 0;
        };
        std::vector<Child> children;
        std::vector<Terminal> terminals;
        int max_frequency = 0;
    };

    std::unordered_map<std::string, std::vector<Entry>> map_;
    std::unordered_map<std::wstring, std::vector<std::string>> word_pinyins_;
    std::map<std::pair<std::string, std::wstring>, UserDictionaryEntry> user_entries_;
    std::unordered_map<std::string, std::vector<std::string>> jianpin_index_;
    std::vector<std::string> jianpin_keys_sorted_;
    std::vector<TrieNode> trie_;  // trie_[0] = root
    std::vector<SyllableTrieNode> syllable_trie_;  // syllable_trie_[0] = root
    mutable bool dirty_ = false;
    bool bulk_loading_ = false;

    void EnsureTrieRoot();
    void EnsureSyllableTrieRoot();
    void TrieInsert(const std::string& pinyin);
    void SyllableTrieInsert(const std::string& pinyin);
    void CollectTriePrefix(const std::string& prefix, size_t limit, std::vector<std::string>* out_keys) const;
    void CollectTrieSubtree(int node, size_t limit, std::vector<std::string>* out_keys) const;

    void RebuildJianpinIndex();
    void RebuildWordPinyinIndex();
    void IndexPinyinKey(const std::string& pinyin);
    void RebuildJianpinSortedKeys();

    static void SortEntries(std::vector<Entry>& entries);
    static std::vector<Candidate> ToCandidates(const std::string& pinyin, const std::vector<Entry>& entries);
};

}  // namespace shuru
