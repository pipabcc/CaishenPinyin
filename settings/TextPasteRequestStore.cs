using System.IO;
using System.Linq;
using System.Text;

namespace ShuruSettings;

internal static class TextPasteRequestStore
{
    internal const string Header = "CAISHEN_TEXT_PASTE_V1\n";
    private const long MaximumPayloadBytes = 64L * 1024 * 1024;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static string RequestDirectory
    {
        get
        {
            var testOverride = Environment.GetEnvironmentVariable(
                "CAISHEN_PASTE_REQUEST_DIR");
            if (!string.IsNullOrWhiteSpace(testOverride))
                return Path.GetFullPath(testOverride);
            return Path.Combine(
                Environment.GetFolderPath(
                    Environment.SpecialFolder.LocalApplicationData),
                "CaishenPinyin",
                "paste_requests");
        }
    }

    internal static string ReadAndDelete(string requestToken)
    {
        if (!IsValidToken(requestToken))
            throw new InvalidDataException("大文本粘贴请求标识无效。");

        Directory.CreateDirectory(RequestDirectory);
        var path = Path.Combine(RequestDirectory, requestToken + ".txt");
        try
        {
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.None,
                64 * 1024,
                FileOptions.SequentialScan);
            if (stream.Length <= Header.Length || stream.Length > MaximumPayloadBytes)
                throw new InvalidDataException("大文本粘贴请求大小无效。");
            using var reader = new StreamReader(
                stream, StrictUtf8, detectEncodingFromByteOrderMarks: false);
            var payload = reader.ReadToEnd();
            if (!payload.StartsWith(Header, StringComparison.Ordinal))
                throw new InvalidDataException("大文本粘贴请求格式无效。");
            return payload[Header.Length..];
        }
        finally
        {
            try { File.Delete(path); }
            catch (Exception ex) { CrashLogger.Log("TextPasteRequestStore.Delete", ex); }
        }
    }

    internal static void CleanupExpired()
    {
        try
        {
            if (!Directory.Exists(RequestDirectory)) return;
            var cutoff = DateTime.UtcNow.AddDays(-1);
            foreach (var path in Directory.EnumerateFiles(
                         RequestDirectory, "*.txt", SearchOption.TopDirectoryOnly))
            {
                if (File.GetLastWriteTimeUtc(path) < cutoff) File.Delete(path);
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("TextPasteRequestStore.Cleanup", ex);
        }
    }

    private static bool IsValidToken(string token) =>
        token.Length == 32 && token.All(Uri.IsHexDigit);
}
