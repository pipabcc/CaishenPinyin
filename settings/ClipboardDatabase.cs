using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace ShuruSettings;

internal readonly record struct ClipboardDeleteResult(
    int DeletedCount,
    IReadOnlyList<string> ImagePaths);

internal sealed class ClipboardDatabase : IDisposable
{
    private const string FtsSchemaMetadataKey = "fts_schema_version";
    private const string FtsSchemaVersion = "1";
    private const int SqliteOk = 0;
    private const int SqliteRow = 100;
    private const int SqliteDone = 101;
    private const int OpenReadWrite = 0x00000002;
    private const int OpenCreate = 0x00000004;
    private const int OpenFullMutex = 0x00010000;
    private static readonly IntPtr SqliteTransient = new(-1);

    private IntPtr handle_;

    private ClipboardDatabase(IntPtr handle) => handle_ = handle;

    internal static ClipboardDatabase Open(string path)
    {
        var result = Native.sqlite3_open_v2(
            path, out var handle, OpenReadWrite | OpenCreate | OpenFullMutex,
            IntPtr.Zero);
        if (result != SqliteOk || handle == IntPtr.Zero)
        {
            var message = handle == IntPtr.Zero
                ? $"无法打开剪贴板数据库，错误码 {result}"
                : ReadNativeText(Native.sqlite3_errmsg(handle));
            if (handle != IntPtr.Zero) Native.sqlite3_close_v2(handle);
            throw new InvalidOperationException(message);
        }
        Native.sqlite3_busy_timeout(handle, 2000);
        return new ClipboardDatabase(handle);
    }

    internal void InitializeSchema()
    {
        Execute("PRAGMA journal_mode=WAL;");
        Execute("PRAGMA synchronous=NORMAL;");
        Execute("PRAGMA foreign_keys=ON;");
        Execute("""
            CREATE TABLE IF NOT EXISTS clipboard_records (
                row_id INTEGER PRIMARY KEY AUTOINCREMENT,
                id TEXT NOT NULL UNIQUE,
                type INTEGER NOT NULL,
                content TEXT NOT NULL,
                display_title TEXT NOT NULL,
                image_path TEXT NOT NULL DEFAULT '',
                created_utc_ms INTEGER NOT NULL,
                metadata_json TEXT NOT NULL DEFAULT '{}'
            );
            CREATE INDEX IF NOT EXISTS idx_clipboard_created
                ON clipboard_records(created_utc_ms DESC, row_id DESC);
            CREATE TABLE IF NOT EXISTS clipboard_meta (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            """);
        try
        {
            Execute("""
                CREATE VIRTUAL TABLE IF NOT EXISTS clipboard_records_fts USING fts5(
                    display_title,
                    content,
                    content='clipboard_records',
                    content_rowid='row_id',
                    tokenize='trigram'
                );
                CREATE TRIGGER IF NOT EXISTS clipboard_records_ai AFTER INSERT ON clipboard_records BEGIN
                    INSERT INTO clipboard_records_fts(rowid, display_title, content)
                    VALUES (new.row_id, new.display_title, new.content);
                END;
                CREATE TRIGGER IF NOT EXISTS clipboard_records_ad AFTER DELETE ON clipboard_records BEGIN
                    INSERT INTO clipboard_records_fts(clipboard_records_fts, rowid, display_title, content)
                    VALUES ('delete', old.row_id, old.display_title, old.content);
                END;
                CREATE TRIGGER IF NOT EXISTS clipboard_records_au AFTER UPDATE ON clipboard_records BEGIN
                    INSERT INTO clipboard_records_fts(clipboard_records_fts, rowid, display_title, content)
                    VALUES ('delete', old.row_id, old.display_title, old.content);
                    INSERT INTO clipboard_records_fts(rowid, display_title, content)
                    VALUES (new.row_id, new.display_title, new.content);
                END;
                """);
            if (!string.Equals(
                    GetMetadata(FtsSchemaMetadataKey), FtsSchemaVersion,
                    StringComparison.Ordinal))
            {
                Execute("INSERT INTO clipboard_records_fts(clipboard_records_fts) VALUES('rebuild');");
                SetMetadata(FtsSchemaMetadataKey, FtsSchemaVersion);
            }
        }
        catch (Exception ex)
        {
            // 极旧 Windows 的 winsqlite3 可能没有 FTS5；基础表查询仍可工作。
            CrashLogger.Log("ClipboardDatabase.InitializeFts", ex);
        }
    }

    internal string? GetMetadata(string key)
    {
        using var statement = Prepare(
            "SELECT value FROM clipboard_meta WHERE key=?1 LIMIT 1;");
        statement.BindText(1, key);
        return statement.Step() == SqliteRow ? statement.ColumnText(0) : null;
    }

    internal void SetMetadata(string key, string value)
    {
        using var statement = Prepare("""
            INSERT INTO clipboard_meta(key, value) VALUES(?1, ?2)
            ON CONFLICT(key) DO UPDATE SET value=excluded.value;
            """);
        statement.BindText(1, key);
        statement.BindText(2, value);
        statement.RequireDone();
    }

    internal int Count(string query = "")
    {
        query = query.Trim();
        var useFts = query.Length >= 3 && HasFtsTable();
        var sql = useFts
            ? """
              SELECT COUNT(*) FROM clipboard_records AS r
              JOIN clipboard_records_fts ON clipboard_records_fts.rowid=r.row_id
              WHERE clipboard_records_fts MATCH ?1;
              """
            : string.IsNullOrEmpty(query)
                ? "SELECT COUNT(*) FROM clipboard_records;"
                : """
                  SELECT COUNT(*) FROM clipboard_records
                  WHERE instr(lower(display_title), lower(?1)) > 0
                     OR instr(lower(content), lower(?1)) > 0;
                  """;
        using var statement = Prepare(sql);
        if (!string.IsNullOrEmpty(query))
            statement.BindText(1, useFts ? QuoteFtsPhrase(query) : query);
        return statement.Step() == SqliteRow
            ? checked((int)statement.ColumnInt64(0))
            : 0;
    }

    internal List<ClipboardRecord> Query(string query, int limit, int offset)
    {
        query = query.Trim();
        limit = Math.Clamp(limit, 1, 100000);
        offset = Math.Max(0, offset);
        var useFts = query.Length >= 3 && HasFtsTable();
        var sql = useFts
            ? """
              SELECT r.id, r.type, r.content, r.display_title, r.image_path,
                     r.created_utc_ms, r.metadata_json
              FROM clipboard_records AS r
              JOIN clipboard_records_fts ON clipboard_records_fts.rowid=r.row_id
              WHERE clipboard_records_fts MATCH ?1
              ORDER BY r.created_utc_ms DESC, r.row_id DESC
              LIMIT ?2 OFFSET ?3;
              """
            : string.IsNullOrEmpty(query)
                ? """
                  SELECT id, type, content, display_title, image_path,
                         created_utc_ms, metadata_json
                  FROM clipboard_records
                  ORDER BY created_utc_ms DESC, row_id DESC
                  LIMIT ?1 OFFSET ?2;
                  """
                : """
                  SELECT id, type, content, display_title, image_path,
                         created_utc_ms, metadata_json
                  FROM clipboard_records
                  WHERE instr(lower(display_title), lower(?1)) > 0
                     OR instr(lower(content), lower(?1)) > 0
                  ORDER BY created_utc_ms DESC, row_id DESC
                  LIMIT ?2 OFFSET ?3;
                  """;
        using var statement = Prepare(sql);
        if (string.IsNullOrEmpty(query))
        {
            statement.BindInt(1, limit);
            statement.BindInt(2, offset);
        }
        else
        {
            statement.BindText(1, useFts ? QuoteFtsPhrase(query) : query);
            statement.BindInt(2, limit);
            statement.BindInt(3, offset);
        }

        var records = new List<ClipboardRecord>();
        while (statement.Step() == SqliteRow)
            records.Add(ReadRecord(statement));
        return records;
    }

    internal ClipboardRecord? FindById(string id)
    {
        using var statement = Prepare("""
            SELECT id, type, content, display_title, image_path,
                   created_utc_ms, metadata_json
            FROM clipboard_records WHERE id=?1 LIMIT 1;
            """);
        statement.BindText(1, id);
        return statement.Step() == SqliteRow ? ReadRecord(statement) : null;
    }

    internal void ReplaceAll(IEnumerable<ClipboardRecord> records)
    {
        InTransaction(() =>
        {
            Execute("DELETE FROM clipboard_records;");
            foreach (var record in records) Insert(record);
        });
    }

    internal List<string> Add(ClipboardRecord record, int maximumRecords)
    {
        var removedImages = new List<string>();
        InTransaction(() =>
        {
            using (var duplicate = Prepare("""
                SELECT image_path, type FROM clipboard_records
                WHERE type=?1 AND content=?2;
                """))
            {
                duplicate.BindInt(1, (int)record.Type);
                duplicate.BindText(2, record.Content);
                while (duplicate.Step() == SqliteRow)
                {
                    if (duplicate.ColumnInt(1) == (int)ClipboardItemType.Image)
                    {
                        var path = duplicate.ColumnText(0);
                        if (!string.IsNullOrEmpty(path) && path != record.ImagePath)
                            removedImages.Add(path);
                    }
                }
            }
            using (var deleteDuplicate = Prepare(
                       "DELETE FROM clipboard_records WHERE type=?1 AND content=?2;"))
            {
                deleteDuplicate.BindInt(1, (int)record.Type);
                deleteDuplicate.BindText(2, record.Content);
                deleteDuplicate.RequireDone();
            }
            Insert(record);

            using (var overflow = Prepare("""
                SELECT image_path, type FROM clipboard_records
                ORDER BY created_utc_ms DESC, row_id DESC
                LIMIT -1 OFFSET ?1;
                """))
            {
                overflow.BindInt(1, maximumRecords);
                while (overflow.Step() == SqliteRow)
                {
                    if (overflow.ColumnInt(1) == (int)ClipboardItemType.Image)
                    {
                        var path = overflow.ColumnText(0);
                        if (!string.IsNullOrEmpty(path)) removedImages.Add(path);
                    }
                }
            }
            using var trim = Prepare("""
                DELETE FROM clipboard_records WHERE row_id IN (
                    SELECT row_id FROM clipboard_records
                    ORDER BY created_utc_ms DESC, row_id DESC
                    LIMIT -1 OFFSET ?1
                );
                """);
            trim.BindInt(1, maximumRecords);
            trim.RequireDone();
        });
        return removedImages;
    }

    internal void UpdateText(string id, string content, string displayTitle)
    {
        using var statement = Prepare("""
            UPDATE clipboard_records
            SET content=?2, display_title=?3
            WHERE id=?1 AND type<>?4;
            """);
        statement.BindText(1, id);
        statement.BindText(2, content);
        statement.BindText(3, displayTitle);
        statement.BindInt(4, (int)ClipboardItemType.Image);
        statement.RequireDone();
    }

    internal ClipboardRecord? Delete(string id)
    {
        ClipboardRecord? removed = null;
        InTransaction(() =>
        {
            removed = FindById(id);
            if (removed == null) return;
            using var statement = Prepare(
                "DELETE FROM clipboard_records WHERE id=?1;");
            statement.BindText(1, id);
            statement.RequireDone();
        });
        return removed;
    }

    internal ClipboardDeleteResult DeleteMany(IEnumerable<string> ids)
    {
        var normalizedIds = ids
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        if (normalizedIds.Length == 0)
            return new ClipboardDeleteResult(0, Array.Empty<string>());

        var imagePaths = new List<string>();
        var deletedCount = 0;
        const int batchSize = 400;
        InTransaction(() =>
        {
            for (var offset = 0; offset < normalizedIds.Length; offset += batchSize)
            {
                var batch = normalizedIds
                    .Skip(offset)
                    .Take(batchSize)
                    .ToArray();
                var placeholders = string.Join(",",
                    Enumerable.Range(1, batch.Length).Select(index => $"?{index}"));

                using (var select = Prepare($"""
                    SELECT image_path FROM clipboard_records
                    WHERE type={(int)ClipboardItemType.Image}
                      AND image_path<>''
                      AND id IN ({placeholders});
                    """))
                {
                    for (var index = 0; index < batch.Length; ++index)
                        select.BindText(index + 1, batch[index]);
                    while (select.Step() == SqliteRow)
                        imagePaths.Add(select.ColumnText(0));
                }

                using var delete = Prepare($"""
                    DELETE FROM clipboard_records
                    WHERE id IN ({placeholders});
                    """);
                for (var index = 0; index < batch.Length; ++index)
                    delete.BindText(index + 1, batch[index]);
                delete.RequireDone();
                deletedCount += Native.sqlite3_changes(handle_);
            }
        });
        return new ClipboardDeleteResult(deletedCount, imagePaths);
    }

    internal List<string> Clear()
    {
        var images = new List<string>();
        InTransaction(() =>
        {
            using (var statement = Prepare("""
                SELECT image_path FROM clipboard_records
                WHERE type=?1 AND image_path<>'';
                """))
            {
                statement.BindInt(1, (int)ClipboardItemType.Image);
                while (statement.Step() == SqliteRow)
                    images.Add(statement.ColumnText(0));
            }
            Execute("DELETE FROM clipboard_records;");
        });
        return images;
    }

    private void Insert(ClipboardRecord record)
    {
        using var statement = Prepare("""
            INSERT INTO clipboard_records(
                id, type, content, display_title, image_path,
                created_utc_ms, metadata_json)
            VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)
            ON CONFLICT(id) DO UPDATE SET
                type=excluded.type,
                content=excluded.content,
                display_title=excluded.display_title,
                image_path=excluded.image_path,
                created_utc_ms=excluded.created_utc_ms,
                metadata_json=excluded.metadata_json;
            """);
        statement.BindText(1, string.IsNullOrWhiteSpace(record.Id)
            ? Guid.NewGuid().ToString("N") : record.Id);
        statement.BindInt(2, (int)record.Type);
        statement.BindText(3, record.Content ?? string.Empty);
        statement.BindText(4, record.DisplayTitle ?? string.Empty);
        statement.BindText(5, record.ImagePath ?? string.Empty);
        statement.BindInt64(6, new DateTimeOffset(record.CreatedTime.ToUniversalTime())
            .ToUnixTimeMilliseconds());
        statement.BindText(7, JsonSerializer.Serialize(
            record.AdditionalData ?? new Dictionary<string, JsonElement>()));
        statement.RequireDone();
    }

    private ClipboardRecord ReadRecord(Statement statement)
    {
        var rawType = statement.ColumnInt(1);
        var type = Enum.IsDefined(typeof(ClipboardItemType), rawType)
            ? (ClipboardItemType)rawType
            : ClipboardItemType.Text;
        Dictionary<string, JsonElement>? metadata = null;
        var metadataJson = statement.ColumnText(6);
        if (!string.IsNullOrWhiteSpace(metadataJson) && metadataJson != "{}")
        {
            try
            {
                metadata = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(
                    metadataJson);
            }
            catch (JsonException ex)
            {
                CrashLogger.Log("ClipboardDatabase.Metadata", ex);
            }
        }
        return new ClipboardRecord
        {
            Id = statement.ColumnText(0),
            Type = type,
            Content = statement.ColumnText(2),
            DisplayTitle = statement.ColumnText(3),
            ImagePath = statement.ColumnText(4),
            CreatedTime = DateTimeOffset.FromUnixTimeMilliseconds(
                statement.ColumnInt64(5)).LocalDateTime,
            AdditionalData = metadata
        };
    }

    private bool HasFtsTable()
    {
        using var statement = Prepare("""
            SELECT 1 FROM sqlite_master
            WHERE type='table' AND name='clipboard_records_fts' LIMIT 1;
            """);
        return statement.Step() == SqliteRow;
    }

    private static string QuoteFtsPhrase(string query) =>
        $"\"{query.Replace("\"", "\"\"")}\"";

    private void InTransaction(Action action)
    {
        Execute("BEGIN IMMEDIATE;");
        try
        {
            action();
            Execute("COMMIT;");
        }
        catch
        {
            try { Execute("ROLLBACK;"); }
            catch { }
            throw;
        }
    }

    private Statement Prepare(string sql)
    {
        var result = Native.sqlite3_prepare_v2(
            handle_, sql, -1, out var statement, IntPtr.Zero);
        if (result != SqliteOk || statement == IntPtr.Zero)
            throw CreateException(result, "准备数据库语句失败");
        return new Statement(this, statement);
    }

    private void Execute(string sql)
    {
        var result = Native.sqlite3_exec(
            handle_, sql, IntPtr.Zero, IntPtr.Zero, out var error);
        if (result == SqliteOk) return;
        var message = error == IntPtr.Zero
            ? ReadNativeText(Native.sqlite3_errmsg(handle_))
            : ReadNativeText(error);
        if (error != IntPtr.Zero) Native.sqlite3_free(error);
        throw new InvalidOperationException($"SQLite {result}: {message}");
    }

    private Exception CreateException(int result, string operation) =>
        new InvalidOperationException(
            $"{operation}，SQLite {result}: {ReadNativeText(Native.sqlite3_errmsg(handle_))}");

    public void Dispose()
    {
        var handle = System.Threading.Interlocked.Exchange(ref handle_, IntPtr.Zero);
        if (handle != IntPtr.Zero) Native.sqlite3_close_v2(handle);
    }

    private static string ReadNativeText(IntPtr value) =>
        value == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(value) ?? string.Empty;

    private sealed class Statement : IDisposable
    {
        private readonly ClipboardDatabase owner_;
        private IntPtr handle_;

        internal Statement(ClipboardDatabase owner, IntPtr handle)
        {
            owner_ = owner;
            handle_ = handle;
        }

        internal void BindText(int index, string value)
        {
            var bytes = Encoding.UTF8.GetBytes((value ?? string.Empty) + "\0");
            Check(Native.sqlite3_bind_text(
                handle_, index, bytes, bytes.Length - 1, SqliteTransient));
        }

        internal void BindInt(int index, int value) =>
            Check(Native.sqlite3_bind_int(handle_, index, value));

        internal void BindInt64(int index, long value) =>
            Check(Native.sqlite3_bind_int64(handle_, index, value));

        internal int Step()
        {
            var result = Native.sqlite3_step(handle_);
            if (result != SqliteRow && result != SqliteDone) Check(result);
            return result;
        }

        internal void RequireDone()
        {
            if (Step() != SqliteDone)
                throw new InvalidOperationException("SQLite 写入未完成");
        }

        internal string ColumnText(int column)
        {
            var value = Native.sqlite3_column_text(handle_, column);
            var length = Native.sqlite3_column_bytes(handle_, column);
            return value == IntPtr.Zero || length <= 0
                ? string.Empty
                : Marshal.PtrToStringUTF8(value, length) ?? string.Empty;
        }

        internal int ColumnInt(int column) =>
            Native.sqlite3_column_int(handle_, column);

        internal long ColumnInt64(int column) =>
            Native.sqlite3_column_int64(handle_, column);

        private void Check(int result)
        {
            if (result != SqliteOk) throw owner_.CreateException(result, "数据库语句失败");
        }

        public void Dispose()
        {
            var handle = System.Threading.Interlocked.Exchange(ref handle_, IntPtr.Zero);
            if (handle != IntPtr.Zero) Native.sqlite3_finalize(handle);
        }
    }

    private static class Native
    {
        private const string Library = "winsqlite3.dll";

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Ansi)]
        internal static extern int sqlite3_open_v2(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string filename,
            out IntPtr database, int flags, IntPtr vfs);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_close_v2(IntPtr database);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_busy_timeout(IntPtr database, int milliseconds);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Ansi)]
        internal static extern int sqlite3_exec(
            IntPtr database, [MarshalAs(UnmanagedType.LPUTF8Str)] string sql,
            IntPtr callback, IntPtr context, out IntPtr errorMessage);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void sqlite3_free(IntPtr value);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr sqlite3_errmsg(IntPtr database);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl,
            CharSet = CharSet.Ansi)]
        internal static extern int sqlite3_prepare_v2(
            IntPtr database, [MarshalAs(UnmanagedType.LPUTF8Str)] string sql,
            int byteCount, out IntPtr statement, IntPtr tail);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_finalize(IntPtr statement);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_step(IntPtr statement);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_changes(IntPtr database);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_bind_text(
            IntPtr statement, int index, byte[] value, int byteCount,
            IntPtr destructor);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_bind_int(
            IntPtr statement, int index, int value);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_bind_int64(
            IntPtr statement, int index, long value);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr sqlite3_column_text(IntPtr statement, int column);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_column_bytes(IntPtr statement, int column);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int sqlite3_column_int(IntPtr statement, int column);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern long sqlite3_column_int64(IntPtr statement, int column);
    }
}
