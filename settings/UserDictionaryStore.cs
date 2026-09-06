using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace ShuruSettings;

internal sealed record LearnedWord(string Pinyin, string Word, int Frequency, int Count, long LastUsed);
internal sealed record UserDictionaryDocument(
    string Generation, string BigramGeneration, IReadOnlyList<LearnedWord> Words);

internal static class UserDictionaryStore
{
    internal const string MutexName = @"Local\CaishenPinyin.UserDictionary";
    private static readonly UTF8Encoding Utf8 = new(false, true);

    internal static UserDictionaryDocument Read(string path)
    {
        if (!File.Exists(path)) return new(new string('0', 32), new string('0', 32), Array.Empty<LearnedWord>());
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length > 64 * 1024 * 1024) throw new InvalidDataException("用户词文件超过 64 MiB。");
        if (!GetFileInformationByHandle(stream.SafeFileHandle, out var info))
            throw new IOException("无法读取用户词文件身份。");
        using var reader = new StreamReader(stream, Utf8, detectEncodingFromByteOrderMarks: true);
        var words = new Dictionary<(string, string), LearnedWord>();
        string? generation = null;
        string? bigramGeneration = null;
        string? line;
        var lineNumber = 0;
        while ((line = reader.ReadLine()) != null)
        {
            ++lineNumber;
            line = line.Trim().TrimStart('\uFEFF');
            if (line.Length == 0) continue;
            if (line.StartsWith("# generation=", StringComparison.Ordinal))
                generation = ReadGeneration(line[13..]);
            else if (line.StartsWith("# bigram_generation=", StringComparison.Ordinal))
                bigramGeneration = ReadGeneration(line[20..]);
            else if (!line.StartsWith('#'))
            {
                var fields = line.Split('\t');
                if (fields.Length is < 3 or > 5)
                    throw new InvalidDataException($"第 {lineNumber} 行用户词列数错误。");
                var pinyin = fields[0].Trim().ToLowerInvariant();
                var word = fields[1].Trim();
                if (pinyin.Length == 0 || word.Length == 0 ||
                    pinyin.Any(ch => ch is not (>= 'a' and <= 'z') && ch != '\'') ||
                    word.Any(char.IsControl) ||
                    !int.TryParse(fields[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out var frequency) || frequency < 0)
                    throw new InvalidDataException($"第 {lineNumber} 行用户词格式错误。");
                var count = 0;
                long time = 0;
                if (fields.Length >= 4 && (!int.TryParse(fields[3], out count) || count < 0) ||
                    fields.Length >= 5 && (!long.TryParse(fields[4], out time) || time < 0))
                    throw new InvalidDataException($"第 {lineNumber} 行学习计数或时间错误。");
                Merge(words, new(pinyin, word, frequency, count, time));
            }
        }
        generation ??= LegacyGeneration(info);
        return new(generation, bigramGeneration ?? generation, words.Values.ToArray());
    }

    internal static void Clear(string path)
    {
        WithLock(() =>
        {
            var generation = Guid.NewGuid().ToString("N");
            Write(path, new(generation, generation, Array.Empty<LearnedWord>()));
            File.Delete(Path.Combine(Path.GetDirectoryName(path)!, "user_bigram.txt"));
        });
    }

    internal static void Import(string source, string target)
    {
        if (!File.Exists(source)) throw new FileNotFoundException("要导入的用户词文件不存在。", source);
        var imported = Read(source);
        WithLock(() =>
        {
            var current = Read(target);
            var words = new Dictionary<(string, string), LearnedWord>();
            foreach (var word in current.Words.Concat(imported.Words)) Merge(words, word);
            Write(target, new(Guid.NewGuid().ToString("N"), current.BigramGeneration, words.Values.ToArray()));
        });
    }

    private static void Merge(Dictionary<(string, string), LearnedWord> words, LearnedWord incoming)
    {
        var key = (incoming.Pinyin, incoming.Word);
        words[key] = words.TryGetValue(key, out var current)
            ? incoming with { Frequency = Math.Max(current.Frequency, incoming.Frequency),
                Count = Math.Max(current.Count, incoming.Count), LastUsed = Math.Max(current.LastUsed, incoming.LastUsed) }
            : incoming;
    }

    private static void WithLock(Action action)
    {
        using var mutex = new Mutex(false, MutexName);
        var owns = false;
        try
        {
            try { owns = mutex.WaitOne(TimeSpan.FromSeconds(5)); }
            catch (AbandonedMutexException) { owns = true; }
            if (!owns) throw new IOException("等待用户词保存锁超时。");
            action();
        }
        finally { if (owns) mutex.ReleaseMutex(); }
    }

    private static void Write(string path, UserDictionaryDocument document)
    {
        UserDataPrivacy.ProtectDirectory(Path.GetDirectoryName(path)!);
        UserDataPrivacy.ProtectFile(path);
        var output = new StringBuilder("# 财神输入法用户词库\n");
        output.Append("# generation=").Append(document.Generation).Append('\n');
        output.Append("# bigram_generation=").Append(document.BigramGeneration).Append('\n');
        foreach (var word in document.Words.OrderByDescending(item => item.Count)
                     .ThenBy(item => item.Pinyin, StringComparer.Ordinal).ThenBy(item => item.Word, StringComparer.Ordinal))
            output.Append(word.Pinyin).Append('\t').Append(word.Word).Append('\t')
                .Append(word.Frequency).Append('\t').Append(word.Count).Append('\t').Append(word.LastUsed).Append('\n');
        var temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                       FileShare.None, 4096, FileOptions.WriteThrough))
            {
                stream.Write(Utf8.GetBytes(output.ToString()));
                stream.Flush(true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    private static string ReadGeneration(string generation)
    {
        if (generation.Length != 32 || generation.Any(ch => ch is not (>= '0' and <= '9') and not (>= 'a' and <= 'f')))
            throw new InvalidDataException("用户词代次标记损坏。");
        return generation;
    }

    private static string LegacyGeneration(FileInformation info)
    {
        Span<byte> bytes = stackalloc byte[16];
        BinaryPrimitives.WriteUInt64LittleEndian(bytes,
            (((ulong)info.IndexHigh << 32) | info.IndexLow) ^ info.Volume);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes[8..],
            (((ulong)info.WriteTimeHigh << 32) | info.WriteTimeLow) ^ (((ulong)info.SizeHigh << 32) | info.SizeLow));
        return Convert.ToHexString(bytes).ToLowerInvariant();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileInformation
    {
        internal uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh;
        internal uint WriteTimeLow, WriteTimeHigh, Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation info);
}
