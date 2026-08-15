using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;

namespace ShuruSettings;

public enum ClipboardItemType
{
    Text,
    Image,
    File
}

public class ClipboardRecord
{
    [JsonPropertyName("id")]
    public string Id { get; set; } = Guid.NewGuid().ToString("N");

    [JsonPropertyName("type")]
    public ClipboardItemType Type { get; set; } = ClipboardItemType.Text;

    [JsonPropertyName("content")]
    public string Content { get; set; } = string.Empty;

    [JsonPropertyName("display_title")]
    public string DisplayTitle { get; set; } = string.Empty;

    [JsonPropertyName("image_path")]
    public string ImagePath { get; set; } = string.Empty;

    [JsonPropertyName("created_time")]
    public DateTime CreatedTime { get; set; } = DateTime.Now;

    [JsonExtensionData]
    public Dictionary<string, JsonElement>? AdditionalData { get; set; }

    [JsonIgnore]
    public string FormattedTime => CreatedTime.ToString("yyyy-MM-dd HH:mm:ss");

    [JsonIgnore]
    public string TypeDisplayName => Type switch
    {
        ClipboardItemType.Image => "图片",
        ClipboardItemType.File => "文件",
        _ => "文本"
    };

    [JsonIgnore]
    public bool IsImage => Type == ClipboardItemType.Image;

    [JsonIgnore]
    public string OpenOrEditLabel => Type == ClipboardItemType.Text
        ? "编辑"
        : "打开";
}

public class ClipboardConfig
{
    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; } = true;

    [JsonPropertyName("max_records")]
    public int MaxRecords { get; set; } = 5000;
}

public static class ClipboardStore
{
    private const string HistoryMutexName =
        "Local\\CaishenPinyinClipboardHistoryV1";
    private const string MigrationMetadataKey = "legacy_json_migration";
    private static readonly object SyncLock = new();
    private static readonly object InitializationLock = new();
    private static readonly Mutex? HistoryMutex = CreateHistoryMutex();
    private static bool databaseInitialized_;

    public static readonly string BaseDir = ResolveBaseDir();
    internal static readonly string DatabasePath = Path.Combine(BaseDir, "history.db");
    internal static readonly string LegacyHistoryPath = Path.Combine(BaseDir, "history.json");
    private static readonly string ConfigPath = Path.Combine(BaseDir, "config.json");
    private static readonly string ImagesDir = Path.Combine(BaseDir, "images");

    static ClipboardStore()
    {
        try
        {
            Directory.CreateDirectory(BaseDir);
            Directory.CreateDirectory(ImagesDir);
            EnsureDatabaseInitialized();
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.Initialize", ex);
        }
    }

    private static string ResolveBaseDir()
    {
        var testOverride = Environment.GetEnvironmentVariable(
            "CAISHEN_CLIPBOARD_DATA_DIR");
        if (!string.IsNullOrWhiteSpace(testOverride))
            return Path.GetFullPath(testOverride);
        return Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CaishenPinyin", "clipboard");
    }

    private static Mutex? CreateHistoryMutex()
    {
        try
        {
            return new Mutex(false, HistoryMutexName);
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.CreateMutex", ex);
            return null;
        }
    }

    private sealed class MutexReleaser : IDisposable
    {
        private Mutex? mutex_;

        internal MutexReleaser(Mutex mutex) => mutex_ = mutex;

        public void Dispose()
        {
            var mutex = Interlocked.Exchange(ref mutex_, null);
            if (mutex == null) return;
            try { mutex.ReleaseMutex(); }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.ReleaseMutex", ex);
            }
        }
    }

    private static IDisposable? AcquireHistoryMutex()
    {
        if (HistoryMutex == null) return null;
        try
        {
            if (!HistoryMutex.WaitOne(TimeSpan.FromSeconds(2)))
            {
                CrashLogger.Log("ClipboardStore.AcquireMutex", "等待迁移锁超时");
                return null;
            }
        }
        catch (AbandonedMutexException)
        {
            // 前一进程异常退出时当前线程已经获得互斥量，可以继续恢复。
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.AcquireMutex", ex);
            return null;
        }
        return new MutexReleaser(HistoryMutex);
    }

    private static void EnsureDatabaseInitialized()
    {
        if (databaseInitialized_) return;
        lock (InitializationLock)
        {
            if (databaseInitialized_) return;
            using var migrationLock = AcquireHistoryMutex();
            if (migrationLock == null)
                throw new IOException("无法获得剪贴板数据库迁移锁");

            using var database = ClipboardDatabase.Open(DatabasePath);
            database.InitializeSchema();
            MigrateLegacyHistory(database);
            databaseInitialized_ = true;
        }
    }

    private static void MigrateLegacyHistory(ClipboardDatabase database)
    {
        if (database.GetMetadata(MigrationMetadataKey) != null) return;
        if (!File.Exists(LegacyHistoryPath))
        {
            database.SetMetadata(MigrationMetadataKey, "none");
            return;
        }

        try
        {
            var json = File.ReadAllText(LegacyHistoryPath);
            using (var document = JsonDocument.Parse(json))
            {
                if (document.RootElement.ValueKind != JsonValueKind.Array)
                    throw new InvalidDataException("剪贴板历史文件不是记录数组");
            }
            var records = JsonSerializer.Deserialize<List<ClipboardRecord>>(json) ??
                throw new InvalidDataException("剪贴板历史记录无法解析");
            database.ReplaceAll(records.OrderBy(record => record.CreatedTime));
            database.SetMetadata(MigrationMetadataKey, "completed");
            PreserveMigratedJsonBackup();
            CrashLogger.Log("ClipboardStore.Migration", $"已迁移 {records.Count} 条 JSON 记录");
        }
        catch (Exception ex)
        {
            // 损坏的旧文件原样保留；数据库从干净状态继续工作。
            database.SetMetadata(MigrationMetadataKey, "failed");
            CrashLogger.Log("ClipboardStore.Migration", ex);
        }
    }

    private static void PreserveMigratedJsonBackup()
    {
        var backup = LegacyHistoryPath + ".migrated.bak";
        if (File.Exists(backup))
        {
            backup = LegacyHistoryPath +
                $".migrated-{DateTime.Now:yyyyMMdd-HHmmss}.bak";
        }
        File.Move(LegacyHistoryPath, backup);
    }

    private static ClipboardDatabase OpenDatabase()
    {
        EnsureDatabaseInitialized();
        return ClipboardDatabase.Open(DatabasePath);
    }

    private static bool WriteTextAtomically(string path, string content)
    {
        var temporaryPath = path + $".{Environment.ProcessId}." +
            $"{Environment.CurrentManagedThreadId}.tmp";
        try
        {
            using (var stream = new FileStream(
                temporaryPath, FileMode.Create, FileAccess.Write, FileShare.None,
                4096, FileOptions.WriteThrough))
            {
                using (var writer = new StreamWriter(
                    stream, new UTF8Encoding(false), 4096, leaveOpen: true))
                {
                    writer.Write(content);
                    writer.Flush();
                }
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporaryPath, path, overwrite: true);
            return true;
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.AtomicWrite", ex);
            return false;
        }
        finally
        {
            try
            {
                if (File.Exists(temporaryPath)) File.Delete(temporaryPath);
            }
            catch
            {
                // 临时文件清理失败不影响已经完成的原子替换。
            }
        }
    }

    private static ClipboardConfig LoadConfigCore()
    {
        try
        {
            if (File.Exists(ConfigPath))
            {
                var config = JsonSerializer.Deserialize<ClipboardConfig>(
                    File.ReadAllText(ConfigPath));
                if (config != null) return config;
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.LoadConfig", ex);
        }
        return new ClipboardConfig();
    }

    public static ClipboardConfig LoadConfig()
    {
        lock (SyncLock)
        {
            using var configLock = AcquireHistoryMutex();
            return configLock == null ? new ClipboardConfig() : LoadConfigCore();
        }
    }

    public static void SaveConfig(ClipboardConfig config)
    {
        lock (SyncLock)
        {
            using var configLock = AcquireHistoryMutex();
            if (configLock == null) return;
            var normalized = new ClipboardConfig
            {
                Enabled = config.Enabled,
                MaxRecords = Math.Clamp(config.MaxRecords, 1, 100000)
            };
            var json = JsonSerializer.Serialize(
                normalized, new JsonSerializerOptions { WriteIndented = true });
            WriteTextAtomically(ConfigPath, json);
        }
    }

    public static List<ClipboardRecord> LoadHistory() =>
        QueryHistory(string.Empty, 100000, 0);

    public static List<ClipboardRecord> QueryHistory(
        string query, int limit = 5000, int offset = 0)
    {
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                return database.Query(query ?? string.Empty, limit, offset);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.QueryHistory", ex);
                return new List<ClipboardRecord>();
            }
        }
    }

    public static int CountHistory(string query = "")
    {
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                return database.Count(query ?? string.Empty);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.CountHistory", ex);
                return 0;
            }
        }
    }

    public static ClipboardRecord? FindRecord(string id)
    {
        if (string.IsNullOrWhiteSpace(id)) return null;
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                return database.FindById(id);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.FindRecord", ex);
                return null;
            }
        }
    }

    public static void SaveHistory(List<ClipboardRecord> records)
    {
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                database.ReplaceAll(records ?? new List<ClipboardRecord>());
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.SaveHistory", ex);
            }
        }
    }

    public static void AddRecord(ClipboardRecord record)
    {
        if (record == null) return;
        List<string> removedImages;
        lock (SyncLock)
        {
            try
            {
                var config = LoadConfigCore();
                if (!config.Enabled) return;
                using var database = OpenDatabase();
                removedImages = database.Add(
                    record, Math.Clamp(config.MaxRecords, 1, 100000));
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.AddRecord", ex);
                return;
            }
        }
        DeleteManagedImages(removedImages, "ClipboardStore.DeleteExpiredImage");
    }

    public static void UpdateRecordContent(string id, string newContent)
    {
        if (string.IsNullOrWhiteSpace(id)) return;
        newContent ??= string.Empty;
        var title = newContent.Trim();
        if (title.Length > 80) title = title[..80] + "...";
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                database.UpdateText(id, newContent, title);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.UpdateRecordContent", ex);
            }
        }
    }

    public static void DeleteRecord(string id)
    {
        if (!string.IsNullOrWhiteSpace(id))
            _ = DeleteRecords(new[] { id });
    }

    public static int DeleteRecords(IEnumerable<string> ids)
    {
        ArgumentNullException.ThrowIfNull(ids);
        var normalizedIds = ids
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        if (normalizedIds.Length == 0) return 0;

        ClipboardDeleteResult result;
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                result = database.DeleteMany(normalizedIds);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.DeleteRecords", ex);
                return 0;
            }
        }
        DeleteManagedImages(
            result.ImagePaths,
            "ClipboardStore.DeleteImages");
        return result.DeletedCount;
    }

    public static void ClearHistory()
    {
        List<string> images;
        lock (SyncLock)
        {
            try
            {
                using var database = OpenDatabase();
                images = database.Clear();
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardStore.ClearHistory", ex);
                return;
            }
        }
        DeleteManagedImages(images, "ClipboardStore.ClearImage");
    }

    public static string SaveImageToDisk(byte[] imageBytes)
    {
        if (imageBytes == null || imageBytes.Length == 0) return string.Empty;
        var fileName = $"clipboard_{DateTime.Now:yyyyMMdd_HHmmss_fff}_" +
            $"{Guid.NewGuid():N}.png";
        var filePath = Path.Combine(ImagesDir, fileName);
        var temporary = filePath + ".tmp";
        try
        {
            using (var stream = new FileStream(
                temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                4096, FileOptions.WriteThrough))
            {
                stream.Write(imageBytes);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, filePath);
            return filePath;
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardStore.SaveImage", ex);
            return string.Empty;
        }
        finally
        {
            try
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            catch { }
        }
    }

    private static void DeleteManagedImages(
        IEnumerable<string> paths, string logSource)
    {
        foreach (var path in paths.Where(path => !string.IsNullOrWhiteSpace(path))
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                var fullPath = Path.GetFullPath(path);
                var relative = Path.GetRelativePath(ImagesDir, fullPath);
                if (Path.IsPathRooted(relative) || relative == ".." ||
                    relative.StartsWith(".." + Path.DirectorySeparatorChar,
                        StringComparison.Ordinal))
                {
                    CrashLogger.Log(logSource, $"拒绝删除图片目录外文件：{fullPath}");
                    continue;
                }
                if (File.Exists(fullPath)) File.Delete(fullPath);
            }
            catch (Exception ex)
            {
                CrashLogger.Log(logSource, ex);
            }
        }
    }
}
