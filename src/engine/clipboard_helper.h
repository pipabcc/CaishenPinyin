#pragma once

#include "candidate.h"

#include <cstddef>
#include <string>
#include <vector>

namespace shuru {

inline constexpr std::size_t kDirectTextCommitLimit = 4096;

bool ShouldPasteTextExternally(std::size_t text_length) noexcept;

// 为超长文本创建一次性跨进程请求。返回的 token 不包含目录或扩展名，
// 设置辅助进程读取后会立即删除请求文件。
bool CreateTextPasteRequest(
    const std::wstring& text,
    std::wstring* request_token);
bool DeleteTextPasteRequest(const std::wstring& request_token) noexcept;

// 获取剪贴板历史候选词列表（query 为 v 后面的过滤关键词，如 v138 中的 138）
std::vector<Candidate> GetClipboardCandidates(
    const std::string& query,
    size_t limit = 100);

// 获取自定义短语候选词列表（query 为 vv 后面的过滤关键词，如 vvjz 中的 jz）
std::vector<Candidate> GetCustomPhraseCandidates(
    const std::string& query,
    size_t limit = 100);

// 删除某条剪贴板记录
bool DeleteClipboardCandidate(
    const std::wstring& full_content,
    const std::wstring& record_id = {});

// 删除某条自定义短语
bool DeleteCustomPhraseCandidate(const std::wstring& phrase);

}  // namespace shuru
