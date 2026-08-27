using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace ShuruSettings;

internal enum DirectTextCommitResult
{
    Success,
    TargetUnavailable,
    ContextChanged,
    SensitiveContext,
    RequestExpired,
    InvalidRequest,
    CommitFailed,
    Timeout
}

internal static class DirectTextCommitRequestStore
{
    internal const string RequestHeader = "CAISHEN_DIRECT_COMMIT_V1\n";
    internal const string ResultHeader =
        "CAISHEN_DIRECT_COMMIT_RESULT_V1\n";
    internal const int MaximumPayloadBytes = 16 * 1024 * 1024;

    private static readonly UTF8Encoding StrictUtf8 = new(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);

    internal static string RequestDirectory
    {
        get
        {
            var overridden = Environment.GetEnvironmentVariable(
                "CAISHEN_DIRECT_COMMIT_REQUEST_DIR");
            return string.IsNullOrWhiteSpace(overridden)
                ? Path.Combine(
                    Environment.GetFolderPath(
                        Environment.SpecialFolder.LocalApplicationData),
                    "CaishenPinyin", "direct_commit_requests")
                : Path.GetFullPath(overridden);
        }
    }

    internal static bool IsValidToken(string? token) =>
        token is { Length: 32 } && token.All(Uri.IsHexDigit);

    internal static bool IsPayloadSizeValid(long fileSize) =>
        fileSize > RequestHeader.Length &&
        fileSize <= RequestHeader.Length + MaximumPayloadBytes;

    internal static string RequestPath(string token) =>
        Path.Combine(RequestDirectory, ValidateToken(token) + ".request");

    internal static string ResultPath(string token) =>
        Path.Combine(RequestDirectory, ValidateToken(token) + ".result");

    internal static string CancelPath(string token) =>
        Path.Combine(RequestDirectory, ValidateToken(token) + ".cancel");

    internal static async Task<DirectTextCommitResult> PublishAndWaitAsync(
        string token,
        string text,
        CancellationToken cancellationToken = default)
    {
        var normalizedToken = ValidateToken(token);
        ArgumentNullException.ThrowIfNull(text);
        var payload = StrictUtf8.GetBytes(text);
        if (payload.Length == 0 || payload.Length > MaximumPayloadBytes)
            throw new InvalidDataException("直接上屏文本大小无效。");

        Directory.CreateDirectory(RequestDirectory);
        var requestPath = RequestPath(normalizedToken);
        var resultPath = ResultPath(normalizedToken);
        var header = StrictUtf8.GetBytes(RequestHeader);
        var content = new byte[checked(header.Length + payload.Length)];
        Buffer.BlockCopy(header, 0, content, 0, header.Length);
        Buffer.BlockCopy(payload, 0, content, header.Length, payload.Length);
        WriteAtomically(requestPath, content);

        try
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            while (DateTime.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var result = TryReadAndDeleteResult(resultPath);
                if (result.HasValue) return result.Value;
                await Task.Delay(25, cancellationToken).ConfigureAwait(true);
            }
            return DirectTextCommitResult.Timeout;
        }
        finally
        {
            // 若目标进程从未消费请求，删除它可防止用户已经看到超时提示后
            // 文本又在稍后的意外时刻上屏。已消费时此操作自然无效。
            TryDelete(requestPath, "DirectTextCommit.DeleteRequest");
        }
    }

    internal static void CleanupExpired()
    {
        try
        {
            if (!Directory.Exists(RequestDirectory)) return;
            var cutoff = DateTime.UtcNow.AddDays(-1);
            foreach (var path in Directory.EnumerateFiles(
                         RequestDirectory, "*", SearchOption.TopDirectoryOnly))
            {
                var extension = Path.GetExtension(path);
                if (extension is not ".request" and not ".result" and not ".cancel" &&
                    !extension.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                if (File.GetLastWriteTimeUtc(path) < cutoff) File.Delete(path);
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("DirectTextCommit.Cleanup", ex);
        }
    }

    private static DirectTextCommitResult? TryReadAndDeleteResult(
        string path)
    {
        if (!File.Exists(path)) return null;
        var shouldDelete = false;
        try
        {
            using var stream = new FileStream(
                path, FileMode.Open, FileAccess.Read, FileShare.None,
                bufferSize: 256, FileOptions.SequentialScan);
            if (stream.Length <= ResultHeader.Length || stream.Length > 256)
            {
                shouldDelete = true;
                return DirectTextCommitResult.InvalidRequest;
            }
            using var reader = new StreamReader(
                stream, StrictUtf8, detectEncodingFromByteOrderMarks: false);
            var value = reader.ReadToEnd();
            shouldDelete = true;
            if (!value.StartsWith(ResultHeader, StringComparison.Ordinal))
                return DirectTextCommitResult.InvalidRequest;
            return ParseResult(value[ResultHeader.Length..].Trim());
        }
        catch (IOException)
        {
            // 目标进程可能刚完成原子改名，下一轮再读即可。
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
        finally
        {
            // 只有完整读出结果后才删除。读取竞争时必须保留文件，避免把
            // 目标进程刚写好的成功状态误删成超时。
            if (shouldDelete)
                TryDelete(path, "DirectTextCommit.DeleteResult");
        }
    }

    private static DirectTextCommitResult ParseResult(string value) =>
        value switch
        {
            "success" => DirectTextCommitResult.Success,
            "target-unavailable" => DirectTextCommitResult.TargetUnavailable,
            "context-changed" => DirectTextCommitResult.ContextChanged,
            "sensitive-context" => DirectTextCommitResult.SensitiveContext,
            "request-expired" => DirectTextCommitResult.RequestExpired,
            "invalid-request" => DirectTextCommitResult.InvalidRequest,
            "commit-failed" => DirectTextCommitResult.CommitFailed,
            _ => DirectTextCommitResult.InvalidRequest
        };

    private static void WriteAtomically(string path, byte[] content)
    {
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var stream = new FileStream(
                       temporary, FileMode.CreateNew, FileAccess.Write,
                       FileShare.None, 64 * 1024, FileOptions.WriteThrough))
            {
                stream.Write(content);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: false);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private static string ValidateToken(string? token)
    {
        if (!IsValidToken(token))
            throw new InvalidDataException("直接上屏会话标识无效。");
        return token!.ToLowerInvariant();
    }

    private static void TryDelete(string path, string logSource)
    {
        try { File.Delete(path); }
        catch (Exception ex) { CrashLogger.Log(logSource, ex); }
    }

    internal static void DeleteSessionFiles(string token)
    {
        var normalizedToken = ValidateToken(token);
        TryDelete(RequestPath(normalizedToken), "DirectTextCommit.DeleteRequest");
        TryDelete(ResultPath(normalizedToken), "DirectTextCommit.DeleteResult");
        TryDelete(CancelPath(normalizedToken), "DirectTextCommit.DeleteCancel");
    }

    internal static void CancelSession(string token)
    {
        var normalizedToken = ValidateToken(token);
        Directory.CreateDirectory(RequestDirectory);
        TryDelete(RequestPath(normalizedToken), "DirectTextCommit.DeleteRequest");
        TryDelete(ResultPath(normalizedToken), "DirectTextCommit.DeleteResult");
        TryDelete(CancelPath(normalizedToken), "DirectTextCommit.DeleteCancel");
        WriteAtomically(
            CancelPath(normalizedToken),
            StrictUtf8.GetBytes("CAISHEN_DIRECT_COMMIT_CANCEL_V1\n"));
    }
}
