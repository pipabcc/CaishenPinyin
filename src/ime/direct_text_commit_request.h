#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace shuru {

inline constexpr std::size_t kDirectTextCommitMaximumPayloadBytes =
    16U * 1024U * 1024U;

enum class DirectTextCommitReadResult {
    NotReady,
    Ready,
    Invalid,
};

enum class DirectTextCommitResult {
    Success,
    TargetUnavailable,
    ContextChanged,
    SensitiveContext,
    RequestExpired,
    InvalidRequest,
    CommitFailed,
};

bool IsDirectTextCommitToken(const std::wstring& token) noexcept;
bool IsDirectTextCommitPayloadSizeValid(
    std::uint64_t file_size) noexcept;
std::wstring CreateDirectTextCommitToken();

// 会话目录可由 CAISHEN_DIRECT_COMMIT_REQUEST_DIR 覆盖，供隔离测试使用。
std::wstring DirectTextCommitRequestDirectory();
std::wstring DirectTextCommitRequestPath(const std::wstring& token);
std::wstring DirectTextCommitResultPath(const std::wstring& token);
std::wstring DirectTextCommitCancelPath(const std::wstring& token);

// 创建目录并清除同一随机令牌的残留文件。调用方必须在把令牌交给设置程序
// 之前执行，避免上一轮异常退出留下的结果被误认为本轮响应。
bool PrepareDirectTextCommitSession(const std::wstring& token);

// 只消费已完成原子改名的请求文件。NotReady 表示文件尚未出现或正被短暂
// 占用；Invalid 表示文件已经出现但内容不可信，并会删除该文件。
DirectTextCommitReadResult ReadAndDeleteDirectTextCommitRequest(
    const std::wstring& token,
    std::wstring* text);

bool WriteDirectTextCommitResult(
    const std::wstring& token,
    DirectTextCommitResult result);

// 独立窗口未选择内容就关闭时会发布取消标记，使目标进程立即释放所持
// ITfContext；标记只表达生命周期，不携带正文。
bool ConsumeDirectTextCommitCancellation(
    const std::wstring& token) noexcept;

void DeleteDirectTextCommitSessionFiles(
    const std::wstring& token) noexcept;

const char* DirectTextCommitResultName(
    DirectTextCommitResult result) noexcept;

}  // namespace shuru
