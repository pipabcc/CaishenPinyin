using System;
using System.IO;

namespace ShuruSettings;

internal static class CrashLogger
{
    private static readonly object SyncLock = new();

    internal static void Log(string source, object? error)
    {
        try
        {
            var logPath = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "CaishenPinyin", "logs", "settings_crash.log");
            var directory = Path.GetDirectoryName(logPath);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            var message = error?.ToString() ?? "未知异常";
            lock (SyncLock)
            {
                File.AppendAllText(
                    logPath,
                    $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{source}] {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // 崩溃记录自身不得再抛出异常。
        }
    }
}
