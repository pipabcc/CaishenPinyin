#pragma once

// 引擎系统词库只读快照（EngineSnapshot v2）。
//
// 目标：把 PinyinEngine 冷加载从"逐行解析 + 重排 + 重建 Trie"（数百毫秒，
// 且每个宿主进程都重复一次）压缩为 CreateFileMapping + 结构校验（页缓存
// 热时毫秒级）。快照由首次传统装载完成后在后台线程生成，存放在用户可写
// 的 LOCALAPPDATA 缓存目录；文件名带源标识 tag，词库升级后旧快照自然失效，
// 新 tag 文件另行生成，避免与仍在映射旧文件的宿主进程争抢替换。
//
// 设计约束：
// - 只读映射：Dictionary 进入映射模式后拒绝一切可变操作。引擎本来就只对
//   用户词典实例做学习写入，系统词典纯查询，因此不需要覆盖层。
// - 不保存进程指针，全部偏移量；x64 小端，结构体按自然对齐布局并在
//   加载侧用 static_assert 锁定尺寸。
// - 完整性校验分两层：头部记录各源文件的 size+mtime+SHA-256（SHA 仅在
//   生成时计算一次），加载时只 stat 源文件比对 size+mtime，再对映射内容
//   做一遍 O(n) 结构校验遍，防止损坏文件让宿主进程崩溃。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace shuru {

constexpr char kEngineSnapshotMagic[8] = {'C', 'S', 'I', 'M', 'E', 'S', 'N', '2'};
constexpr std::uint32_t kEngineSnapshotFormatVersion = 2;

// 源文件身份戳：加载时只比对 size 与 mtime；sha256 仅供诊断，不在加载路径重算。
struct SnapshotSourceStamp {
    std::uint64_t file_size = 0;
    std::uint64_t mtime_utc = 0;  // FILETIME 转 64 位
    std::uint32_t present = 0;    // 生成时该源文件是否存在
    std::uint32_t reserved = 0;
    std::uint8_t sha256[32] = {};
};

#pragma pack(push, 8)
struct EngineSnapshotHeader {
    char magic[8];
    std::uint32_t format_version;
    std::uint32_t header_size;
    std::uint32_t product_version;  // 生成时的输入法版本，仅诊断用
    std::uint32_t flags;            // bit0: 已含单字反推结果
    std::uint64_t total_file_size;

    std::uint32_t key_count;      // 拼音键数量
    std::uint32_t entry_count;    // 词条总数
    std::uint32_t trie_count;     // 拼音 Trie 节点数
    std::uint32_t str_trie_count; // 音节（简拼）Trie 节点数
    std::uint32_t syllable_count; // 音节表条目数
    std::uint32_t fp_count;       // 词指纹数量
    std::uint32_t en_count;       // 英文词条数
    std::uint32_t reserved0;

    // 各区段相对映射基址的偏移与字节长度；全部按 8 字节对齐存放。
    std::uint64_t ofs_keys_blob;      std::uint64_t len_keys_blob;
    std::uint64_t ofs_key_index;      std::uint64_t len_key_index;
    std::uint64_t ofs_entries;        std::uint64_t len_entries;
    std::uint64_t ofs_words_blob;     std::uint64_t len_words_blob;
    std::uint64_t ofs_trie;           std::uint64_t len_trie;
    std::uint64_t ofs_str_trie;       std::uint64_t len_str_trie;
    std::uint64_t ofs_root_children;  std::uint64_t len_root_children;
    std::uint64_t ofs_syl_offsets;    std::uint64_t len_syl_offsets;
    std::uint64_t ofs_syl_blob;       std::uint64_t len_syl_blob;
    std::uint64_t ofs_fingerprints;   std::uint64_t len_fingerprints;
    std::uint64_t ofs_en_records;     std::uint64_t len_en_records;
    std::uint64_t ofs_en_blob;        std::uint64_t len_en_blob;

    SnapshotSourceStamp source_base_dict;
    SnapshotSourceStamp source_char_dict;
    SnapshotSourceStamp source_en_dict;
};

// 键索引项：替代每进程重建的 unordered_map<string, bucket>。
// 按 hash 升序排布，二分查找后回比键字节防碰撞。
struct SnapshotKeyIndexEntry {
    std::uint64_t hash;
    std::uint32_t key_offset;    // keys blob 内偏移
    std::uint32_t key_length;
    std::uint32_t entries_first; // 首个词条在词条区中的下标
    std::uint32_t entry_count;
};

// 词条记录：Entry 的平铺只读形态（word 存于 UTF-16 词blob）。
struct SnapshotEntryRecord {
    std::int32_t word_offset;      // UTF-16 码元偏移
    std::int32_t word_units;       // UTF-16 码元长度
    std::int32_t frequency;
    std::int32_t selection_count;
    std::int64_t last_used_unix;
    std::uint32_t flags;           // bit0: from_user
    std::uint32_t reserved;
};

// 英文词条记录（按规范化键升序）。
struct SnapshotEnglishRecord {
    std::uint32_t key_offset;
    std::uint32_t key_length;
    std::uint32_t display_offset;
    std::uint32_t display_length;
    std::int32_t frequency;
    std::uint32_t reserved;
};
#pragma pack(pop)

// 快照缓存文件的完整路径（LOCALAPPDATA 下，带源标识 tag）。
std::wstring EngineSnapshotCachePath(const std::string& source_tag);

// 由三个源文件身份推导的稳定短标识；同一词库内容得到同一 tag。
bool ComputeEngineSnapshotTag(
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    std::string* out_tag);

// 校验映射好的快照并返回头指针；失败返回 nullptr（结构非法/版本不符/源不匹配）。
// source 目录用于源文件 size+mtime 复核。
const EngineSnapshotHeader* ValidateEngineSnapshot(
    const void* base, std::size_t size,
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path);

// 把已构建完成的系统词库与英文词库序列化为快照字节流（生成路径，仅在
// 传统装载成功后调用一次）。失败返回 false 且不产生部分文件。
bool SerializeEngineSnapshot(
    class Dictionary* system_dictionary,
    class EnglishDictionary* english_dictionary,
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    std::vector<std::uint8_t>* out_blob);

// 尝试从缓存目录加载快照并注入两个词典（进入只读映射模式）。
// 成功时顺带清理其它过期 tag 的快照文件（尽力而为，忽略锁定冲突）。
bool TryAdoptEngineSnapshot(
    const std::wstring& base_dict_path,
    const std::wstring& char_dict_path,
    const std::wstring& en_dict_path,
    Dictionary* system_dictionary,
    EnglishDictionary* english_dictionary);

// 把字节流写入缓存目录（临时文件 + 原子替换），供装载后台线程调用。
bool StoreEngineSnapshot(
    const std::string& source_tag,
    const std::vector<std::uint8_t>& blob);

// 写入指定完整路径（同样临时文件 + 原子替换）；供安装期预生成等
// 非默认缓存位置的调用方使用。
bool StoreEngineSnapshotToPath(
    const std::wstring& target_path,
    const std::vector<std::uint8_t>& blob);

}  // namespace shuru
