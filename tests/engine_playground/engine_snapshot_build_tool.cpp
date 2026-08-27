// 安装期快照预生成工具（方案 C）：按运行时完全相同的传统装载路径构建
// 系统词库，再把快照写入当前用户的本地缓存（默认）或指定路径。
// 安装器在部署成功后以隐藏方式调用本工具；失败静默，不影响安装结果，
// 首次冷启动会自动回退到传统装载并自行再生快照。
//
//   engine_snapshot_build_tool --lexicon-dir <dir> [--out <file>]
#include "engine/dictionary.h"
#include "engine/english_dict.h"
#include "engine/engine_snapshot.h"
#include "common/logger.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::wstring WideFromArg(const char* arg) {
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, arg, -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, arg, -1, wide.data(), len);
    }
    return wide;
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring lexicon_dir;
    std::wstring out_path;
    // 同时接受 "--flag value" 与 "--flag=value"：NSIS ExecShell 以等号
    // 连写形式传参，避免路径引号歧义。
    for (int i = 1; i < argc; ++i) {
        const char* flag = argv[i];
        std::string owned_value;
        const char* value = nullptr;
        const char* equals = std::strchr(flag, '=');
        if (equals != nullptr) {
            owned_value.assign(equals + 1);
            value = owned_value.c_str();
        } else {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                return 2;
            }
            value = argv[++i];
        }
        const size_t flag_len = equals != nullptr
            ? static_cast<size_t>(equals - flag)
            : std::strlen(flag);
        if (flag_len == std::strlen("--lexicon-dir") &&
            std::strncmp(flag, "--lexicon-dir", flag_len) == 0) {
            lexicon_dir = WideFromArg(value);
        } else if (flag_len == std::strlen("--out") &&
                   std::strncmp(flag, "--out", flag_len) == 0) {
            out_path = WideFromArg(value);
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", flag);
            return 2;
        }
    }
    if (lexicon_dir.empty()) {
        std::fprintf(stderr,
                     "usage: engine_snapshot_build_tool --lexicon-dir <dir> "
                     "[--out <file>]\n");
        return 2;
    }
    if (!lexicon_dir.empty() && lexicon_dir.back() == L'\\') {
        lexicon_dir.pop_back();
    }

    const auto overall_started = std::chrono::steady_clock::now();

    // 与 PinyinEngine::Initialize 传统路径逐步一致：base -> char(可选)
    // -> 单字反推(仅 char 缺失时) -> EndBulkLoad；英文词库缺失时留空。
    shuru::Dictionary dictionary;
    dictionary.BeginBulkLoad();
    const std::wstring base_path = lexicon_dir + L"\\base_dict.txt";
    const bool base_ok = dictionary.LoadFromFile(base_path, false);
    bool char_ok = false;
    const std::wstring chars_path = lexicon_dir + L"\\char_dict.txt";
    if (GetFileAttributesW(chars_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        char_ok = dictionary.LoadFromFile(chars_path, false);
    }
    if (base_ok && !char_ok) {
        dictionary.DeriveSingleCharacters();
    }
    dictionary.EndBulkLoad();
    if (!base_ok) {
        std::fprintf(stderr, "failed to load %ls\n", base_path.c_str());
        return 1;
    }

    shuru::EnglishDictionary english;
    const std::wstring en_path = lexicon_dir + L"\\en_dict.txt";
    if (GetFileAttributesW(en_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        english.LoadFromFile(en_path);
    }

    const auto build_started = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> blob;
    if (!shuru::SerializeEngineSnapshot(
            &dictionary, &english, base_path, chars_path, en_path, &blob)) {
        std::fprintf(stderr, "serialize failed\n");
        return 1;
    }

    // 默认写入当前用户缓存目录的 tag 命名路径：与运行期
    // TryAdoptEngineSnapshot 的首选查找完全一致，首次击键即命中。
    bool stored = false;
    if (out_path.empty()) {
        std::string tag;
        if (!shuru::ComputeEngineSnapshotTag(
                base_path, chars_path, en_path, &tag)) {
            std::fprintf(stderr, "compute tag failed\n");
            return 1;
        }
        stored = shuru::StoreEngineSnapshot(tag, blob);
        out_path = shuru::EngineSnapshotCachePath(tag);
    } else {
        stored = shuru::StoreEngineSnapshotToPath(out_path, blob);
    }
    if (!stored) {
        std::fprintf(stderr, "store failed: %ls\n", out_path.c_str());
        return 1;
    }

    const double total_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - overall_started)
            .count();
    const double build_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - build_started)
            .count();
    std::printf("snapshot ok entries=%zu bytes=%zu build=%.0fms total=%.0fms -> %ls\n",
                dictionary.Size(), blob.size(), build_ms, total_ms,
                out_path.c_str());
    return 0;
}
