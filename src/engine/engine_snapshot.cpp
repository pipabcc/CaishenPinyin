#include "engine_snapshot.h"

#include "dictionary.h"
#include "english_dict.h"

#include "../common/logger.h"
#include "../common/user_data_paths.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace shuru {

namespace {

// 与 Dictionary 内部节点同布局的镜像结构：本文件不引用其私有嵌套类型，
// 仅依赖布局稳定（两侧均有 static_assert 锁定）。
struct RawTrieNode {
    std::int32_t first_child;
    std::int32_t next_sibling;
    std::int32_t max_frequency;
    std::int32_t terminal_frequency;
    char label;
    bool terminal;
};
static_assert(sizeof(RawTrieNode) == 20 && alignof(RawTrieNode) == 4,
              "TrieNode layout must stay stable: snapshot stores raw copies");

struct RawSyllableTrieNode {
    std::int32_t first_child;
    std::int32_t next_sibling;
    std::int32_t max_frequency;
    std::uint16_t syllable_id;
    bool terminal;
};
static_assert(sizeof(RawSyllableTrieNode) == 16 &&
                  alignof(RawSyllableTrieNode) == 4,
              "SyllableTrieNode layout must stay stable");

constexpr std::uint32_t kMaxKeys = 5000000;
constexpr std::uint32_t kMaxEntries = 10000000;
constexpr std::uint32_t kMaxTrieNodes = 10000000;
constexpr std::uint32_t kMaxSyllables = 4096;
constexpr std::uint32_t kMaxFingerprints = 10000000;
constexpr std::uint32_t kMaxEnglishRecords = 1000000;

std::uint64_t Fnva1a64(const void* data, size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct FileStamp {
    bool present = false;
    std::uint64_t size = 0;
    std::uint64_t mtime_utc = 0;
};

FileStamp StatFile(const std::wstring& path) {
    FileStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA attributes {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return stamp;
    }
    stamp.present = true;
    const ULARGE_INTEGER size{{attributes.nFileSizeLow, attributes.nFileSizeHigh}};
    stamp.size = size.QuadPart;
    const ULARGE_INTEGER mtime{
        {attributes.ftLastWriteTime.dwLowDateTime,
         attributes.ftLastWriteTime.dwHighDateTime}};
    stamp.mtime_utc = mtime.QuadPart;
    return stamp;
}

bool Sha256Data(const unsigned char* data, size_t size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objlen = 0, cb = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objlen), sizeof(objlen),
                          &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<unsigned char> obj(objlen);
    const bool ok =
        BCryptCreateHash(alg, &hash, obj.data(), objlen, nullptr, 0, 0) >= 0 &&
        (size == 0 ||
         BCryptHashData(hash, const_cast<PUCHAR>(data),
                        static_cast<ULONG>(size), 0) >= 0) &&
        BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 仅在快照生成路径调用；加载路径不重算源文件强哈希。
bool Sha256FileForStamp(const std::wstring& path, unsigned char out[32]) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> block(64 * 1024);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objlen = 0, cb = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objlen), sizeof(objlen),
                          &cb, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<unsigned char> obj(objlen);
    bool ok = BCryptCreateHash(alg, &hash, obj.data(), objlen, nullptr, 0, 0) >= 0;
    while (ok && in) {
        in.read(reinterpret_cast<char*>(block.data()),
                static_cast<std::streamsize>(block.size()));
        const auto n = in.gcount();
        if (n > 0) {
            ok = BCryptHashData(hash, block.data(), static_cast<ULONG>(n), 0) >= 0;
        }
    }
    ok = ok && in.eof() && BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

void FillSourceStamp(const std::wstring& path, SnapshotSourceStamp* stamp) {
    const FileStamp file = StatFile(path);
    stamp->present = file.present ? 1 : 0;
    stamp->file_size = file.size;
    stamp->mtime_utc = file.mtime_utc;
    if (file.present &&
        Sha256FileForStamp(path, stamp->sha256)) {
        return;
    }
    std::memset(stamp->sha256, 0, sizeof(stamp->sha256));
}

bool StampsMatch(const SnapshotSourceStamp& stamp, const std::wstring& path) {
    const FileStamp file = StatFile(path);
    if (stamp.present != 0) {
        return file.present && file.size == stamp.file_size &&
               file.mtime_utc == stamp.mtime_utc;
    }
    return !file.present;
}

template <class T>
void PutSection(std::vector<std::uint8_t>* blob, const T* data, size_t count) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    blob->insert(blob->end(), bytes, bytes + count * sizeof(T));
}

std::uint64_t Align8(std::uint64_t value) {
    return (value + 7ull) & ~7ull;
}

// 映射生命周期持有者：由 shared_ptr 控制块管理，最后一个词典释放时解映射。
struct MappedFileRegion {
    void* view = nullptr;
    HANDLE mapping = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    ~MappedFileRegion() {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

bool OpenReadOnlyMapping(
    const std::wstring& path, const void** out_base, size_t* out_size,
    std::shared_ptr<void>* out_region) {
    *out_base = nullptr;
    *out_size = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(1ull << 31)) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0,
                                        nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    // 把整文件读入内存的等待交给预取接口，与后续校验的 CPU 时间重叠。
    WIN32_MEMORY_RANGE_ENTRY range{};
    range.VirtualAddress = view;
    range.NumberOfBytes = static_cast<SIZE_T>(size.QuadPart);
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
    auto* region = new (std::nothrow) MappedFileRegion();
    if (region == nullptr) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    region->view = view;
    region->mapping = mapping;
    region->file = file;
    *out_base = view;
    *out_size = static_cast<size_t>(size.QuadPart);
    // deleter 收到的是控制块持有的指针；整个 region 对象才是托管资源。
    *out_region = std::shared_ptr<void>(region, [](void* p) {
        delete static_cast<MappedFileRegion*>(p);
    });
    return true;
}

bool SectionInRange(std::uint64_t offset, std::uint64_t length,
                    std::uint64_t file_size) {
    return offset <= file_size && length <= file_size - offset;
}

// 结构校验遍：索引/Trie/偏移全部边界检查，并验证 Trie 无环，保证后续查询
// 在损坏文件上也不会越界或死循环。
bool ValidateStructure(const EngineSnapshotHeader& header) {
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(&header);
    const auto* key_index = reinterpret_cast<const SnapshotKeyIndexEntry*>(
        base + header.ofs_key_index);
    const auto* entries = reinterpret_cast<const SnapshotEntryRecord*>(
        base + header.ofs_entries);
    const char* keys_blob =
        reinterpret_cast<const char*>(base + header.ofs_keys_blob);
    const std::uint64_t word_units_total =
        header.len_words_blob / sizeof(wchar_t);

    // 键索引：严格递增，桶区间恰好连续覆盖词条区。
    std::uint32_t expected_first = 0;
    for (std::uint32_t i = 0; i < header.key_count; ++i) {
        const SnapshotKeyIndexEntry& item = key_index[i];
        if (item.key_length == 0 ||
            item.key_offset > header.len_keys_blob ||
            item.key_length > header.len_keys_blob - item.key_offset) {
            return false;
        }
        if (item.entry_count == 0 ||
            item.entries_first != expected_first ||
            expected_first > header.entry_count ||
            item.entry_count > header.entry_count - expected_first) {
            return false;
        }
        expected_first += item.entry_count;
        if (i > 0) {
            const SnapshotKeyIndexEntry& prev = key_index[i - 1];
            if (item.hash == prev.hash) return false;
            if (item.hash < prev.hash) return false;
        }
    }
    if (expected_first != header.entry_count) return false;

    // 词条：词引用落在词 blob 内且非空。
    for (std::uint32_t i = 0; i < header.entry_count; ++i) {
        const SnapshotEntryRecord& record = entries[i];
        if (record.word_units <= 0 ||
            static_cast<std::uint64_t>(record.word_offset) >
                word_units_total ||
            static_cast<std::uint64_t>(record.word_units) >
                word_units_total - static_cast<std::uint64_t>(record.word_offset)) {
            return false;
        }
    }

    // 键字节本身（哈希碰撞回比需要读取）。
    for (std::uint32_t i = 0; i < header.key_count; ++i) {
        const SnapshotKeyIndexEntry& item = key_index[i];
        const std::uint64_t hash =
            Fnva1a64(keys_blob + item.key_offset, item.key_length);
        if (hash != item.hash) return false;
    }

    // 拼音/音节 Trie：顺序扫描做索引边界检查（对预取与缓存友好）。
    // 环防护不在装载期做指针追逐，而由查询侧的兄弟链跳数上限兜底，
    // 使冷加载保持纯顺序访存。
    {
        const auto* nodes = reinterpret_cast<const RawTrieNode*>(
            base + header.ofs_trie);
        for (std::uint32_t i = 0; i < header.trie_count; ++i) {
            const std::int32_t first = nodes[i].first_child;
            const std::int32_t sibling = nodes[i].next_sibling;
            if (first >= static_cast<std::int32_t>(header.trie_count) ||
                sibling >= static_cast<std::int32_t>(header.trie_count)) {
                return false;
            }
        }
        const auto* str_nodes = reinterpret_cast<const RawSyllableTrieNode*>(
            base + header.ofs_str_trie);
        for (std::uint32_t i = 0; i < header.str_trie_count; ++i) {
            const std::int32_t first = str_nodes[i].first_child;
            const std::int32_t sibling = str_nodes[i].next_sibling;
            if (first >= static_cast<std::int32_t>(header.str_trie_count) ||
                sibling >= static_cast<std::int32_t>(header.str_trie_count) ||
                str_nodes[i].syllable_id >= header.syllable_count) {
                return false;
            }
        }
    }

    // 根子节点与音节表偏移。
    const auto* root_children = reinterpret_cast<const std::int32_t*>(
        base + header.ofs_root_children);
    for (std::uint32_t i = 0; i < header.syllable_count; ++i) {
        const std::int32_t child = root_children[i];
        if (child >= static_cast<std::int32_t>(header.str_trie_count)) {
            return false;
        }
    }
    const auto* syl_offsets = reinterpret_cast<const std::uint32_t*>(
        base + header.ofs_syl_offsets);
    const char* syl_blob =
        reinterpret_cast<const char*>(base + header.ofs_syl_blob);
    for (std::uint32_t i = 0; i < header.syllable_count; ++i) {
        if (syl_offsets[i] > syl_offsets[i + 1] ||
            syl_offsets[i + 1] > header.len_syl_blob) {
            return false;
        }
        for (std::uint32_t p = syl_offsets[i]; p < syl_offsets[i + 1]; ++p) {
            if (syl_blob[p] < 'a' || syl_blob[p] > 'z') return false;
        }
    }

    // 词指纹严格递增（生成时排序去重）。
    const auto(*fingerprints)[2] =
        reinterpret_cast<const std::uint64_t (*)[2]>(
            base + header.ofs_fingerprints);
    for (std::uint32_t i = 1; i < header.fp_count; ++i) {
        const std::uint64_t* previous = fingerprints[i - 1];
        const std::uint64_t* current = fingerprints[i];
        if (previous[0] > current[0] ||
            (previous[0] == current[0] && previous[1] >= current[1])) {
            return false;
        }
    }

    // 英文记录：键升序、字段落在 blob 内。
    const auto* en_records = reinterpret_cast<const SnapshotEnglishRecord*>(
        base + header.ofs_en_records);
    const char* en_blob =
        reinterpret_cast<const char*>(base + header.ofs_en_blob);
    for (std::uint32_t i = 0; i < header.en_count; ++i) {
        const SnapshotEnglishRecord& record = en_records[i];
        if (record.key_length < 2 || record.display_length == 0 ||
            record.key_offset > header.len_en_blob ||
            record.key_length > header.len_en_blob - record.key_offset ||
            record.display_offset > header.len_en_blob ||
            record.display_length >
                header.len_en_blob - record.display_offset) {
            return false;
        }
        if (i > 0) {
            const std::string_view previous(
                en_blob + en_records[i - 1].key_offset,
                en_records[i - 1].key_length);
            const std::string_view current(
                en_blob + record.key_offset, record.key_length);
            if (!(previous < current)) return false;
        }
    }
    return true;
}

void CleanupStaleSnapshots(const std::wstring& keep_path) {
    const std::wstring directory = std::filesystem::path(keep_path).parent_path().wstring();
    WIN32_FIND_DATAW find_data{};
    const std::wstring pattern = directory + L"\\system_lexicon.*.snapshot.v2.bin";
    HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring candidate = directory + L"\\" + find_data.cFileName;
        if (_wcsicmp(candidate.c_str(), keep_path.c_str()) == 0) continue;
        DeleteFileW(candidate.c_str());  // 被占用时删除失败属预期，静默跳过
    } while (FindNextFileW(find, &find_data));
    FindClose(find);
}

}  // namespace

std::wstring EngineSnapshotCachePath(const std::string& source_tag) {
    std::wstring name = CaishenUserDataPath(L"snapshot\\system_lexicon.");
    if (name.empty()) return {};
    // tag 是十六进制 ASCII，直接逐字符宽化。
    for (const char character : source_tag) {
        name.push_back(static_cast<wchar_t>(character));
    }
    name += L".snapshot.v2.bin";
    return name;
}

bool ComputeEngineSnapshotTag(
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    std::string* out_tag) {
    if (out_tag == nullptr) return false;
    const FileStamp base_stamp = StatFile(base_dict_path);
    if (!base_stamp.present) return false;
    const FileStamp char_stamp = StatFile(char_dict_path);
    const FileStamp en_stamp = StatFile(en_dict_path);

    char buffer[128];
    auto feed = [&](const FileStamp& stamp) {
        std::snprintf(buffer, sizeof(buffer), "|%llu,%llu,%d",
                      static_cast<unsigned long long>(stamp.size),
                      static_cast<unsigned long long>(stamp.mtime_utc),
                      stamp.present ? 1 : 0);
        return Fnva1a64(buffer, std::strlen(buffer));
    };
    std::uint64_t combined = Fnva1a64(kEngineSnapshotMagic, 8);
    combined = combined * 1099511628211ull ^ feed(base_stamp);
    combined = combined * 1099511628211ull ^ feed(char_stamp);
    combined = combined * 1099511628211ull ^ feed(en_stamp);
    char tag[32];
    std::snprintf(tag, sizeof(tag), "%016llx",
                  static_cast<unsigned long long>(combined));
    *out_tag = tag;
    return true;
}

const EngineSnapshotHeader* ValidateEngineSnapshot(
    const void* base, size_t size,
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path) {
    if (base == nullptr || size < sizeof(EngineSnapshotHeader)) return nullptr;
    const auto* header = static_cast<const EngineSnapshotHeader*>(base);
    if (std::memcmp(header->magic, kEngineSnapshotMagic, 8) != 0) return nullptr;
    if (header->format_version != kEngineSnapshotFormatVersion) return nullptr;
    if (header->header_size != sizeof(EngineSnapshotHeader)) return nullptr;
    if (header->total_file_size != size) return nullptr;
    if (header->key_count > kMaxKeys || header->entry_count > kMaxEntries ||
        header->trie_count > kMaxTrieNodes ||
        header->str_trie_count > kMaxTrieNodes ||
        header->syllable_count > kMaxSyllables ||
        header->fp_count > kMaxFingerprints ||
        header->en_count > kMaxEnglishRecords) {
        return nullptr;
    }

    // 区段长度与数量精确匹配，防止乘法溢出与越界。
    struct RangeCheck { std::uint64_t offset, length, expected; };
    const RangeCheck ranges[] = {
        {header->ofs_keys_blob, header->len_keys_blob, 0},
        {header->ofs_key_index, header->len_key_index,
         static_cast<std::uint64_t>(header->key_count) *
             sizeof(SnapshotKeyIndexEntry)},
        {header->ofs_entries, header->len_entries,
         static_cast<std::uint64_t>(header->entry_count) *
             sizeof(SnapshotEntryRecord)},
        {header->ofs_words_blob, header->len_words_blob, 0},
        {header->ofs_trie, header->len_trie,
         static_cast<std::uint64_t>(header->trie_count) * sizeof(RawTrieNode)},
        {header->ofs_str_trie, header->len_str_trie,
         static_cast<std::uint64_t>(header->str_trie_count) *
             sizeof(RawSyllableTrieNode)},
        {header->ofs_root_children, header->len_root_children,
         static_cast<std::uint64_t>(header->syllable_count) * 4},
        {header->ofs_syl_offsets, header->len_syl_offsets,
         (static_cast<std::uint64_t>(header->syllable_count) + 1) * 4},
        {header->ofs_syl_blob, header->len_syl_blob, 0},
        {header->ofs_fingerprints, header->len_fingerprints,
         static_cast<std::uint64_t>(header->fp_count) * 16},
        {header->ofs_en_records, header->len_en_records,
         static_cast<std::uint64_t>(header->en_count) *
             sizeof(SnapshotEnglishRecord)},
        {header->ofs_en_blob, header->len_en_blob, 0},
    };
    for (const RangeCheck& range : ranges) {
        if (!SectionInRange(range.offset, range.length, size)) return nullptr;
        if (range.expected != 0 && range.length != range.expected) return nullptr;
    }

    // 源文件身份复核：只 stat 比对 size+mtime，不在加载路径重算强哈希。
    if (!StampsMatch(header->source_base_dict, base_dict_path) ||
        !StampsMatch(header->source_char_dict, char_dict_path) ||
        !StampsMatch(header->source_en_dict, en_dict_path)) {
        return nullptr;
    }

    if (!ValidateStructure(*header)) return nullptr;
    return header;
}

bool Dictionary::AdoptMappedSnapshot(
    const void* base, size_t /*size*/, std::shared_ptr<void> region) {
    if (base == nullptr || region == nullptr) return false;
    const auto* header = static_cast<const EngineSnapshotHeader*>(base);
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(base);
    snap_base_ = bytes;
    snap_header_ = header;
    snap_key_index_ = reinterpret_cast<const SnapshotKeyIndexEntry*>(
        bytes + header->ofs_key_index);
    snap_entries_ = reinterpret_cast<const SnapshotEntryRecord*>(
        bytes + header->ofs_entries);
    snap_keys_blob_ = reinterpret_cast<const char*>(bytes + header->ofs_keys_blob);
    snap_words_blob_ = reinterpret_cast<const wchar_t*>(
        bytes + header->ofs_words_blob);
    snap_trie_ = reinterpret_cast<const TrieNode*>(
        reinterpret_cast<const RawTrieNode*>(bytes + header->ofs_trie));
    snap_str_trie_ = reinterpret_cast<const SyllableTrieNode*>(
        reinterpret_cast<const RawSyllableTrieNode*>(bytes +
                                                     header->ofs_str_trie));
    snap_root_children_ = reinterpret_cast<const std::int32_t*>(
        bytes + header->ofs_root_children);
    snap_syl_offsets_ = reinterpret_cast<const std::uint32_t*>(
        bytes + header->ofs_syl_offsets);
    snap_syl_blob_ = reinterpret_cast<const char*>(bytes + header->ofs_syl_blob);
    snap_fingerprints_ = reinterpret_cast<const std::uint64_t (*)[2]>(
        bytes + header->ofs_fingerprints);
    snap_key_count_ = header->key_count;
    snap_entry_count_ = header->entry_count;
    snap_fp_count_ = header->fp_count;
    mapped_region_ = std::move(region);
    mapped_mode_ = true;  // 最后提交：此前失败不会留下半初始化状态
    return true;
}

bool TryAdoptEngineSnapshot(
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    Dictionary* system_dictionary,
    EnglishDictionary* english_dictionary) {
    if (system_dictionary == nullptr || english_dictionary == nullptr) {
        return false;
    }
    if (system_dictionary->is_mapped() || english_dictionary->is_mapped()) {
        return false;
    }
    std::string tag;
    if (!ComputeEngineSnapshotTag(
            base_dict_path, char_dict_path, en_dict_path, &tag)) {
        return false;
    }
    const std::wstring path = EngineSnapshotCachePath(tag);
    if (path.empty()) return false;

    const void* base = nullptr;
    size_t size = 0;
    std::shared_ptr<void> region;
    if (!OpenReadOnlyMapping(path, &base, &size, &region)) {
        return false;
    }
    const EngineSnapshotHeader* header =
        ValidateEngineSnapshot(base, size, base_dict_path, char_dict_path,
                               en_dict_path);
    if (header == nullptr) {
        SHURU_LOG_WARN("engine snapshot rejected, falling back to text load");
        region.reset();
        return false;
    }

    // 校验通过后两个词典共享同一份映射生命周期。
    if (!system_dictionary->AdoptMappedSnapshot(base, size, region) ||
        !english_dictionary->AdoptMappedSnapshot(base, size, region)) {
        region.reset();
        return false;
    }
    SHURU_LOG_INFO("engine snapshot adopted tag=%s keys=%u entries=%u",
                   tag.c_str(), header->key_count, header->entry_count);
    CleanupStaleSnapshots(path);
    return true;
}

bool StoreEngineSnapshotToPath(
    const std::wstring& target_path,
    const std::vector<std::uint8_t>& blob) {
    if (blob.empty() || target_path.empty()) return false;
    const std::filesystem::path directory =
        std::filesystem::path(target_path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::exists(directory)) {
        SHURU_LOG_WARN("snapshot cache dir create failed");
        return false;
    }
    const std::wstring temp = target_path + L".tmp." +
        std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL write_ok = WriteFile(
        file, blob.data(), static_cast<DWORD>(blob.size()), &written,
        nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!write_ok || written != blob.size() ||
        !MoveFileExW(temp.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool StoreEngineSnapshot(
    const std::string& source_tag,
    const std::vector<std::uint8_t>& blob) {
    // 安装期预生成工具与运行期后台再生共用：写入当前用户缓存目录的
    // tag 命名路径，保证下次冷启动按 tag 直接命中。
    return StoreEngineSnapshotToPath(EngineSnapshotCachePath(source_tag), blob);
}

bool SerializeEngineSnapshot(
    Dictionary* dictionary,
    EnglishDictionary* english_dictionary,
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    std::vector<std::uint8_t>* out_blob) {
    if (out_blob == nullptr || dictionary == nullptr ||
        english_dictionary == nullptr || dictionary->mapped_mode_ ||
        english_dictionary->is_mapped()) {
        return false;
    }

    // 词条区：按键字典序输出，桶内保持 EndBulkLoad 的既有排序。
    std::vector<const std::string*> keys;
    keys.reserve(dictionary->map_.size());
    for (const auto& kv : dictionary->map_) keys.push_back(&kv.first);
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });

    std::uint32_t entry_total = 0;
    for (const std::string* key : keys) {
        const auto found = dictionary->map_.find(*key);
        if (found == dictionary->map_.end() || found->second.empty()) {
            return false;  // 不应有空桶；防御性拒绝而不是产出坏快照
        }
        const std::uint64_t sum = static_cast<std::uint64_t>(entry_total) +
            found->second.size();
        if (sum > kMaxEntries) return false;
        entry_total = static_cast<std::uint32_t>(sum);
    }

    // 词条区与键索引统一按 FNV 哈希序排布：键索引本身即哈希二分序，
    // 桶区间在索引序下天然连续，校验与查找共用同一不变式。
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) {
                  const std::uint64_t ha = Fnva1a64(a->data(), a->size());
                  const std::uint64_t hb = Fnva1a64(b->data(), b->size());
                  if (ha != hb) return ha < hb;
                  return *a < *b;
              });

    std::vector<std::uint8_t> keys_blob;
    std::vector<std::uint8_t> words_blob;
    std::vector<SnapshotKeyIndexEntry> key_index;
    std::vector<SnapshotEntryRecord> records;
    keys_blob.reserve(1024 * 1024);
    words_blob.reserve(4 * 1024 * 1024);
    key_index.reserve(keys.size());
    records.reserve(entry_total);

    for (const std::string* key : keys) {
        const auto& bucket = dictionary->map_.find(*key)->second;
        SnapshotKeyIndexEntry index_entry{};
        index_entry.hash = Fnva1a64(key->data(), key->size());
        index_entry.key_offset = static_cast<std::uint32_t>(keys_blob.size());
        index_entry.key_length = static_cast<std::uint32_t>(key->size());
        index_entry.entries_first = static_cast<std::uint32_t>(records.size());
        index_entry.entry_count = static_cast<std::uint32_t>(bucket.size());
        keys_blob.insert(keys_blob.end(), key->begin(), key->end());

        for (const auto& entry : bucket) {
            SnapshotEntryRecord record{};
            record.word_offset = static_cast<std::int32_t>(
                words_blob.size() / sizeof(wchar_t));
            record.word_units = static_cast<std::int32_t>(entry.word.size());
            record.frequency = entry.frequency;
            record.selection_count = entry.selection_count;
            record.last_used_unix = entry.last_used_unix;
            record.flags = entry.from_user ? 1u : 0u;
            const auto* word_bytes =
                reinterpret_cast<const std::uint8_t*>(entry.word.data());
            words_blob.insert(
                words_blob.end(), word_bytes,
                word_bytes + entry.word.size() * sizeof(wchar_t));
            records.push_back(record);
        }
        key_index.push_back(index_entry);
    }

    // Trie 与指纹按内存布局原样拷贝（布局由文件头部的 static_assert 锁定）。
    const auto* trie_bytes = reinterpret_cast<const std::uint8_t*>(
        dictionary->trie_.data());
    const std::uint64_t trie_bytes_len =
        static_cast<std::uint64_t>(dictionary->trie_.size()) * sizeof(RawTrieNode);
    const auto* str_bytes = reinterpret_cast<const std::uint8_t*>(
        dictionary->syllable_trie_.data());
    const std::uint64_t str_bytes_len =
        static_cast<std::uint64_t>(dictionary->syllable_trie_.size()) *
        sizeof(RawSyllableTrieNode);

    std::vector<std::uint32_t> syl_offsets;
    std::vector<std::uint8_t> syl_blob;
    syl_offsets.reserve(dictionary->syllable_values_.size() + 1);
    for (const std::string& value : dictionary->syllable_values_) {
        syl_offsets.push_back(static_cast<std::uint32_t>(syl_blob.size()));
        syl_blob.insert(syl_blob.end(), value.begin(), value.end());
    }
    syl_offsets.push_back(static_cast<std::uint32_t>(syl_blob.size()));

    std::vector<SnapshotEnglishRecord> en_records;
    std::vector<std::uint8_t> en_blob;
    en_records.reserve(english_dictionary->words_.size());
    for (const auto& item : english_dictionary->words_) {
        SnapshotEnglishRecord record{};
        record.key_offset = static_cast<std::uint32_t>(en_blob.size());
        record.key_length = static_cast<std::uint32_t>(item.word.size());
        en_blob.insert(en_blob.end(), item.word.begin(), item.word.end());
        record.display_offset = static_cast<std::uint32_t>(en_blob.size());
        record.display_length = static_cast<std::uint32_t>(item.display.size());
        en_blob.insert(en_blob.end(), item.display.begin(), item.display.end());
        record.frequency = item.frequency;
        record.reserved = 0;
        en_records.push_back(record);
    }

    EngineSnapshotHeader header{};
    std::memcpy(header.magic, kEngineSnapshotMagic, 8);
    header.format_version = kEngineSnapshotFormatVersion;
    header.header_size = sizeof(EngineSnapshotHeader);
    header.product_version = 0;  // 版本戳仅诊断用，留空避免引入头依赖
    header.flags = 0;
    header.key_count = static_cast<std::uint32_t>(keys.size());
    header.entry_count = entry_total;
    header.trie_count = static_cast<std::uint32_t>(dictionary->trie_.size());
    header.str_trie_count =
        static_cast<std::uint32_t>(dictionary->syllable_trie_.size());
    header.syllable_count =
        static_cast<std::uint32_t>(dictionary->syllable_values_.size());
    header.fp_count = static_cast<std::uint32_t>(
        dictionary->word_fingerprints_.size());
    header.en_count = static_cast<std::uint32_t>(en_records.size());

    FillSourceStamp(base_dict_path, &header.source_base_dict);
    FillSourceStamp(char_dict_path, &header.source_char_dict);
    FillSourceStamp(en_dict_path, &header.source_en_dict);

    // 布局：头部之后各段 8 字节对齐依次排布。
    std::uint64_t cursor = Align8(sizeof(header));
    const auto section = [&cursor](std::uint64_t length) {
        const std::uint64_t offset = cursor;
        cursor = Align8(cursor + length);
        return offset;
    };
    header.ofs_keys_blob = section(keys_blob.size());
    header.len_keys_blob = keys_blob.size();
    header.ofs_key_index = section(key_index.size() * sizeof(SnapshotKeyIndexEntry));
    header.len_key_index = key_index.size() * sizeof(SnapshotKeyIndexEntry);
    header.ofs_entries = section(records.size() * sizeof(SnapshotEntryRecord));
    header.len_entries = records.size() * sizeof(SnapshotEntryRecord);
    header.ofs_words_blob = section(words_blob.size());
    header.len_words_blob = words_blob.size();
    header.ofs_trie = section(trie_bytes_len);
    header.len_trie = trie_bytes_len;
    header.ofs_str_trie = section(str_bytes_len);
    header.len_str_trie = str_bytes_len;
    header.ofs_root_children = section(
        dictionary->syllable_root_children_.size() * sizeof(std::int32_t));
    header.len_root_children =
        dictionary->syllable_root_children_.size() * sizeof(std::int32_t);
    header.ofs_syl_offsets = section(syl_offsets.size() * sizeof(std::uint32_t));
    header.len_syl_offsets = syl_offsets.size() * sizeof(std::uint32_t);
    header.ofs_syl_blob = section(syl_blob.size());
    header.len_syl_blob = syl_blob.size();
    header.ofs_fingerprints = section(
        dictionary->word_fingerprints_.size() *
        sizeof(std::array<std::uint64_t, 2>));
    header.len_fingerprints = dictionary->word_fingerprints_.size() *
        sizeof(std::array<std::uint64_t, 2>);
    header.ofs_en_records = section(en_records.size() * sizeof(SnapshotEnglishRecord));
    header.len_en_records = en_records.size() * sizeof(SnapshotEnglishRecord);
    header.ofs_en_blob = section(en_blob.size());
    header.len_en_blob = en_blob.size();
    header.total_file_size = cursor;

    out_blob->clear();
    out_blob->reserve(static_cast<size_t>(cursor));
    PutSection(out_blob, &header, 1);
    out_blob->resize(static_cast<size_t>(cursor), 0);
    std::memcpy(out_blob->data() + header.ofs_keys_blob, keys_blob.data(),
                keys_blob.size());
    std::memcpy(out_blob->data() + header.ofs_key_index, key_index.data(),
                key_index.size() * sizeof(SnapshotKeyIndexEntry));
    std::memcpy(out_blob->data() + header.ofs_entries, records.data(),
                records.size() * sizeof(SnapshotEntryRecord));
    std::memcpy(out_blob->data() + header.ofs_words_blob, words_blob.data(),
                words_blob.size());
    std::memcpy(out_blob->data() + header.ofs_trie, trie_bytes,
                static_cast<size_t>(trie_bytes_len));
    std::memcpy(out_blob->data() + header.ofs_str_trie, str_bytes,
                static_cast<size_t>(str_bytes_len));
    std::memcpy(out_blob->data() + header.ofs_root_children,
                dictionary->syllable_root_children_.data(),
                dictionary->syllable_root_children_.size() *
                    sizeof(std::int32_t));
    std::memcpy(out_blob->data() + header.ofs_syl_offsets, syl_offsets.data(),
                syl_offsets.size() * sizeof(std::uint32_t));
    std::memcpy(out_blob->data() + header.ofs_syl_blob, syl_blob.data(),
                syl_blob.size());
    std::memcpy(out_blob->data() + header.ofs_fingerprints,
                dictionary->word_fingerprints_.data(),
                dictionary->word_fingerprints_.size() *
                    sizeof(std::array<std::uint64_t, 2>));
    std::memcpy(out_blob->data() + header.ofs_en_records, en_records.data(),
                en_records.size() * sizeof(SnapshotEnglishRecord));
    std::memcpy(out_blob->data() + header.ofs_en_blob, en_blob.data(),
                en_blob.size());
    return true;
}

}  // namespace shuru
