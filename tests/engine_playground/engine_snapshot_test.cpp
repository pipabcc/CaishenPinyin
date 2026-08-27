// EngineSnapshot v2 一致性与失效测试：
// 1. 同一词库分别走堆装载与只读快照映射，五类查询结果必须完全一致；
// 2. 快照内容被篡改时必须被结构校验拒绝，词典保持未映射状态；
// 3. 源文件 size/mtime 变化后旧快照不得复用；
// 4. 输出阶段耗时（生成/装载），作为阶段1基准数据。
#include "engine/dictionary.h"
#include "engine/english_dict.h"
#include "engine/engine_snapshot.h"
#include "common/logger.h"
#include "common/user_data_paths.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestCase {
    const char* pinyin;
    const wchar_t* word;
    int frequency;
};

const TestCase kCases[] = {
    {"ni hao", L"你好", 900000},
    {"ni", L"你", 800000},
    {"ni", L"呢", 1000},
    {"hao", L"好", 850000},
    {"shi jie", L"世界", 700000},
    {"ji suan", L"计算", 500000},
    {"ji", L"机", 600000},
    {"suan", L"算", 550000},
    {"ren gong zhi neng", L"人工智能", 300000},
    {"xian", L"先", 400000},
    {"xi an", L"西安", 200000},
    {"bei jing", L"北京", 620000},
};

const char* kEnglishLines[] = {
    "hello\t530\n",
    "world\t520\n",
    "keyboard\t310\n",
    "ka\t90\n",
};

bool SameCandidate(const shuru::Candidate& a, const shuru::Candidate& b) {
    return a.text == b.text && a.pinyin == b.pinyin &&
           a.frequency == b.frequency &&
           a.selection_count == b.selection_count &&
           a.last_used_unix == b.last_used_unix &&
           a.from_user == b.from_user &&
           a.learning_score == b.learning_score;
}

bool SameCandidates(const std::vector<shuru::Candidate>& a,
                    const std::vector<shuru::Candidate>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!SameCandidate(a[i], b[i])) return false;
    }
    return true;
}

std::wstring WriteTempLexicon(const std::wstring& dir, const wchar_t* name,
                              const std::vector<std::string>& lines) {
    std::wstring path = dir + L"\\" + name;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) out << line << "\n";
    return path;
}

std::vector<char> ReadFileBytes(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

bool WriteFileBytes(const std::wstring& path, const std::vector<char>& data) {
    std::ofstream out(std::filesystem::path(path),
                      std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace shuru;
    const char* build_tool_path = argc > 1 ? argv[1] : nullptr;

    const std::wstring dir = [] {
        wchar_t temp_path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_path);
        return std::wstring(temp_path) + L"caishen_snapshot_test";
    }();
    std::filesystem::remove_all(std::filesystem::path(dir));
    std::filesystem::create_directories(std::filesystem::path(dir));

    std::vector<std::string> lines;
    for (const auto& item : kCases) {
        std::string utf8;
        for (const wchar_t ch : std::wstring(item.word)) {
            // 测试词均为 ASCII 拼音键 + BMP 中文词；这里直接用 WideChar 转 UTF-8
            char buffer[8] = {};
            const int n = WideCharToMultiByte(
                CP_UTF8, 0, &ch, 1, buffer, sizeof(buffer), nullptr, nullptr);
            utf8.append(buffer, static_cast<size_t>(n));
        }
        lines.push_back(std::string(item.pinyin) + "\t" + utf8 + "\t" +
                        std::to_string(item.frequency));
    }
    const std::wstring base_path = WriteTempLexicon(dir, L"base_dict.txt", lines);
    const std::wstring en_path = WriteTempLexicon(
        dir, L"en_dict.txt",
        std::vector<std::string>(std::begin(kEnglishLines),
                                 std::end(kEnglishLines)));
    // char_dict 缺席：走 DeriveSingleCharacters 分支，产物同样应进快照。
    const std::wstring missing_chars = dir + L"\\char_dict.txt";

    // ---- 堆参考实现 ----
    Dictionary reference;
    reference.BeginBulkLoad();
    if (!reference.LoadFromFile(base_path, false)) {
        std::cout << "FAIL: reference base load\n";
        return 1;
    }
    reference.DeriveSingleCharacters();
    reference.EndBulkLoad();
    EnglishDictionary reference_en;
    if (!reference_en.LoadFromFile(en_path)) {
        std::cout << "FAIL: reference english load\n";
        return 1;
    }

    // ---- 序列化 + 存储 ----
    const auto build_started = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> blob;
    Dictionary* reference_ptr = &reference;
    EnglishDictionary* reference_en_ptr = &reference_en;
    if (!SerializeEngineSnapshot(reference_ptr, reference_en_ptr,
                                 base_path, missing_chars, en_path, &blob)) {
        std::cout << "FAIL: serialize\n";
        return 1;
    }
    const double build_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - build_started)
                                .count();

    std::string tag;
    if (!ComputeEngineSnapshotTag(base_path, missing_chars, en_path, &tag)) {
        std::cout << "FAIL: tag\n";
        return 1;
    }
    if (!StoreEngineSnapshot(tag, blob)) {
        std::cout << "FAIL: store\n";
        return 1;
    }

    // ---- 篡改快照必须被结构校验拒绝 ----
    {
        std::vector<char> bytes = ReadFileBytes(EngineSnapshotCachePath(tag));
        if (bytes.size() < sizeof(EngineSnapshotHeader)) {
            std::cout << "FAIL: stored snapshot too small\n";
            return 1;
        }
        // 破坏 Trie 区段中段一个节点指针字段（保持文件长度不变）。
        const size_t poison = sizeof(EngineSnapshotHeader) + 4096;
        if (poison >= bytes.size()) {
            std::cout << "FAIL: snapshot smaller than expected\n";
            return 1;
        }
        bytes[poison] = static_cast<char>(0xFF);
        if (!WriteFileBytes(EngineSnapshotCachePath(tag), bytes)) {
            std::cout << "FAIL: rewrite poisoned snapshot\n";
            return 1;
        }
        Dictionary reject_dict;
        EnglishDictionary reject_en;
        if (TryAdoptEngineSnapshot(base_path, missing_chars, en_path,
                                   &reject_dict, &reject_en)) {
            std::cout << "FAIL: poisoned snapshot accepted\n";
            return 1;
        }
        if (reject_dict.is_mapped()) {
            std::cout << "FAIL: rejected snapshot left mapped flag\n";
            return 1;
        }
        // 还原，供后续正式装载与 mtime 失效用例使用干净状态。
        if (!StoreEngineSnapshot(tag, blob)) {
            std::cout << "FAIL: restore snapshot\n";
            return 1;
        }
    }

    // ---- 快照路径装载并逐项比对 ----
    Dictionary mapped;
    EnglishDictionary mapped_en;
    const auto adopt_started = std::chrono::steady_clock::now();
    if (!TryAdoptEngineSnapshot(base_path, missing_chars, en_path,
                                &mapped, &mapped_en)) {
        std::cout << "FAIL: adopt\n";
        return 1;
    }
    const double adopt_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - adopt_started)
                                .count();
    if (!mapped.is_mapped() || !mapped_en.is_mapped()) {
        std::cout << "FAIL: mapped flags\n";
        return 1;
    }
    if (mapped.Size() != reference.Size() ||
        mapped.JianpinSize() != reference.JianpinSize()) {
        std::cout << "FAIL: sizes differ mapped=" << mapped.Size()
                  << " ref=" << reference.Size() << "\n";
        return 1;
    }

    size_t checked_candidates = 0;
    auto compare_all = [&](const std::string& key) {
        if (!SameCandidates(mapped.LookupExact(key),
                            reference.LookupExact(key))) {
            std::cout << "FAIL: exact mismatch " << key << "\n";
            exit(1);
        }
        if (!SameCandidates(mapped.LookupPrefix(key, 16),
                            reference.LookupPrefix(key, 16))) {
            std::cout << "FAIL: prefix mismatch " << key << "\n";
            exit(1);
        }
        if (!SameCandidates(mapped.LookupJianpin(key, 16),
                            reference.LookupJianpin(key, 16))) {
            std::cout << "FAIL: jianpin mismatch " << key << "\n";
            exit(1);
        }
        if (!SameCandidates(mapped.LookupMixed(key, 16),
                            reference.LookupMixed(key, 16))) {
            std::cout << "FAIL: mixed mismatch " << key << "\n";
            exit(1);
        }
        const auto matches_a =
            mapped.LookupMixedPrefixes(key, 32);
        const auto matches_b =
            reference.LookupMixedPrefixes(key, 32);
        if (matches_a.size() != matches_b.size()) {
            std::cout << "FAIL: mixed prefixes count " << key << "\n";
            exit(1);
        }
        for (size_t i = 0; i < matches_a.size(); ++i) {
            if (!SameCandidate(matches_a[i].candidate, matches_b[i].candidate) ||
                matches_a[i].consumed_input != matches_b[i].consumed_input ||
                matches_a[i].segmented_input != matches_b[i].segmented_input) {
                std::cout << "FAIL: mixed prefixes item " << key << "\n";
                exit(1);
            }
        }
        for (const auto& item : kCases) {
            const std::wstring word(item.word);
            checked_candidates += 1;
            if (mapped.ContainsWord(word) != reference.ContainsWord(word) ||
                mapped.ContainsWordPinyin(word, item.pinyin) !=
                    reference.ContainsWordPinyin(word, item.pinyin) ||
                mapped.LookupFrequency(item.pinyin, word) !=
                    reference.LookupFrequency(item.pinyin, word)) {
                std::cout << "FAIL: contains/frequency " << item.pinyin << "\n";
                exit(1);
            }
        }
    };
    for (const auto& item : kCases) compare_all(item.pinyin);

    // 英文查询一致
    for (const char* probe : {"hel", "wor", "keyb", "ka"}) {
        if (!SameCandidates(mapped_en.LookupExact(probe),
                            reference_en.LookupExact(probe)) ||
            !SameCandidates(mapped_en.LookupPrefix(probe, 8),
                            reference_en.LookupPrefix(probe, 8))) {
            std::cout << "FAIL: english mismatch " << probe << "\n";
            return 1;
        }
    }

    // 映射模式拒绝对系统词典的写入（防御性），且不崩溃。
    mapped.AddWord("zzz", L"测试", 10, true);
    if (mapped.Size() != reference.Size()) {
        std::cout << "FAIL: mutation leaked into mapped dict\n";
        return 1;
    }

    // ---- 源文件变化后旧快照不得复用 ----
    {
        std::vector<std::string> appended = lines;
        appended.push_back("ma ma\t妈妈\t480000");
        const std::wstring changed = WriteTempLexicon(dir, L"base_dict.txt", appended);
        if (changed != base_path) {
            std::cout << "FAIL: path unstable\n";
            return 1;
        }
        Dictionary stale_dict;
        EnglishDictionary stale_en;
        if (TryAdoptEngineSnapshot(base_path, missing_chars, en_path,
                                   &stale_dict, &stale_en)) {
            std::cout << "FAIL: stale snapshot accepted after source change\n";
            return 1;
        }
    }

    // ---- 安装期预生成工具（方案 C）端到端 ----
    // 清空用户缓存后调用工具；它应按运行时相同的装载路径生成快照并
    // 写入 tag 命名的缓存路径，随后 TryAdoptEngineSnapshot 直接命中。
    if (build_tool_path != nullptr && build_tool_path[0] != '\0') {
        std::error_code snapshot_fs_ec;
        std::filesystem::remove_all(
            std::filesystem::path(shuru::CaishenUserDataPath(L"snapshot")),
            snapshot_fs_ec);
        const int tool_path_len = MultiByteToWideChar(
            CP_UTF8, 0, build_tool_path, -1, nullptr, 0);
        std::wstring tool_path(static_cast<size_t>(tool_path_len > 1 ?
            tool_path_len - 1 : 0), L'\0');
        if (tool_path_len > 1) {
            MultiByteToWideChar(CP_UTF8, 0, build_tool_path, -1,
                                tool_path.data(), tool_path_len);
        }
        std::wstring cmdline = L"\"" + tool_path +
            L"\" --lexicon-dir \"" + dir + L"\"";
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        PROCESS_INFORMATION process_info{};
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup_info, &process_info)) {
            std::cout << "FAIL: spawn build tool\n";
            return 1;
        }
        WaitForSingleObject(process_info.hProcess, 120000);
        DWORD exit_code = 1;
        GetExitCodeProcess(process_info.hProcess, &exit_code);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        if (exit_code != 0) {
            std::cout << "FAIL: build tool exit=" << exit_code << "\n";
            return 1;
        }

        Dictionary tool_dict;
        EnglishDictionary tool_en;
        if (!TryAdoptEngineSnapshot(base_path, missing_chars, en_path,
                                    &tool_dict, &tool_en)) {
            std::cout << "FAIL: adopt snapshot produced by build tool\n";
            return 1;
        }
        if (!SameCandidates(tool_dict.LookupExact("ni hao"),
                            reference.LookupExact("ni hao")) ||
            !SameCandidates(tool_en.LookupPrefix("hel", 8),
                            reference_en.LookupPrefix("hel", 8))) {
            std::cout << "FAIL: build-tool snapshot query mismatch\n";
            return 1;
        }
    }

    std::cout.precision(2);
    std::cout << std::fixed
              << "engine_snapshot ok candidates_checked=" << checked_candidates
              << " serialize_ms=" << build_ms
              << " adopt_ms=" << adopt_ms
              << " bytes=" << blob.size() << "\n";
    return 0;
}
