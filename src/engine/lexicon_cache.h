#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shuru {

struct CachedLexiconLine {
    std::string pinyin;
    std::string word_utf8;
    std::int32_t frequency = 0;
};

// 只读缓存含 magic/version、源文件 SHA-256、负载 SHA-256 和边界检查。
// visitor 版本在校验完整缓存后逐行发布，避免同时保留缓存副本和 rows。
using LexiconRowVisitor = std::function<bool(CachedLexiconLine&&)>;
bool VisitLexiconCache(const std::wstring& source_path, const std::wstring& cache_path,
                       const LexiconRowVisitor& visitor, size_t* row_count = nullptr);
bool LoadLexiconCache(const std::wstring& source_path, const std::wstring& cache_path,
                      std::vector<CachedLexiconLine>* rows);
bool BuildLexiconCache(const std::wstring& source_path, const std::wstring& cache_path);

}  // namespace shuru
