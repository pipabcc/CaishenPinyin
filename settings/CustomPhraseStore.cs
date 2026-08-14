using System.IO;
using System.Text;

namespace ShuruSettings;

public sealed record CustomPhrase(string Code, string Phrase, int Position);

public static class CustomPhraseStore
{
    private const int MaximumCodeLength = 32;
    private const int MaximumPhraseLength = 128;
    private const string MutexName = @"Local\CaishenPinyin.CustomPhrases";

    public static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CaishenPinyin", "data", "lexicon", "custom_phrases.txt");

    public static IReadOnlyList<CustomPhrase> Load(string? path = null) =>
        Read(path ?? DefaultPath, strict: false);

    public static IReadOnlyList<CustomPhrase> Import(string path) =>
        Read(path, strict: true);

    public static void Save(IEnumerable<CustomPhrase> phrases, string? path = null)
    {
        path ??= DefaultPath;
        var validated = ValidateCollection(phrases).ToArray();
        var directory = Path.GetDirectoryName(path) ??
            throw new InvalidOperationException("自定义短语路径无效。");
        Directory.CreateDirectory(directory);

        using var mutex = new Mutex(false, MutexName);
        var ownsMutex = false;
        try
        {
            try { ownsMutex = mutex.WaitOne(TimeSpan.FromSeconds(5)); }
            catch (AbandonedMutexException) { ownsMutex = true; }
            if (!ownsMutex) throw new IOException("自定义短语正在被其他操作占用，请稍后重试。");

            var temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
            try
            {
                using (var stream = new FileStream(
                           temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                           4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(true)))
                {
                    writer.WriteLine("# 财神输入法自定义短语 v1");
                    writer.WriteLine("# code<TAB>phrase<TAB>position");
                    foreach (var item in validated)
                        writer.WriteLine($"{item.Code}\t{item.Phrase}\t{item.Position}");
                    writer.Flush();
                    stream.Flush(true);
                }
                File.Move(temporary, path, true);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }
        finally
        {
            if (ownsMutex) mutex.ReleaseMutex();
        }
    }

    public static CustomPhrase Validate(CustomPhrase phrase, int lineNumber = 0)
    {
        var prefix = lineNumber > 0 ? $"第 {lineNumber} 行：" : string.Empty;
        var code = phrase.Code.Trim().ToLowerInvariant();
        var text = phrase.Phrase.Trim();
        if (code.Length is < 1 or > MaximumCodeLength ||
            code.Any(ch => ch is < 'a' or > 'z'))
            throw new InvalidDataException(prefix + "输入码只能包含 1–32 个英文字母。");
        if (text.Length is < 1 or > MaximumPhraseLength || text.Any(char.IsControl))
            throw new InvalidDataException(prefix + "短语必须为 1–128 个可见字符，且不能换行。");
        if (phrase.Position is < 1 or > 9)
            throw new InvalidDataException(prefix + "候选位置必须为 1–9。");
        return phrase with { Code = code, Phrase = text };
    }

    private static IReadOnlyList<CustomPhrase> Read(string path, bool strict)
    {
        if (!File.Exists(path)) return Array.Empty<CustomPhrase>();
        var result = new List<CustomPhrase>();
        var identities = new HashSet<string>(StringComparer.Ordinal);
        var lineNumber = 0;
        foreach (var rawLine in File.ReadLines(path, Encoding.UTF8))
        {
            ++lineNumber;
            var line = rawLine.TrimStart('\uFEFF');
            if (string.IsNullOrWhiteSpace(line) || line.TrimStart().StartsWith('#') ||
                line.TrimStart().StartsWith(';')) continue;
            try
            {
                var fields = line.Split('\t');
                if (fields.Length != 3 || !int.TryParse(fields[2], out var position))
                    throw new InvalidDataException($"第 {lineNumber} 行：应为输入码、短语、候选位置三列。");
                var item = Validate(new CustomPhrase(fields[0], fields[1], position), lineNumber);
                var identity = item.Code + "\0" + item.Phrase;
                if (!identities.Add(identity))
                    throw new InvalidDataException($"第 {lineNumber} 行：输入码和短语重复。");
                result.Add(item);
            }
            catch (InvalidDataException) when (!strict)
            {
                // 运行时读取以可恢复为先；导入时使用 strict=true 明确提示错误。
            }
        }
        return result;
    }

    private static IEnumerable<CustomPhrase> ValidateCollection(IEnumerable<CustomPhrase> phrases)
    {
        ArgumentNullException.ThrowIfNull(phrases);
        var identities = new HashSet<string>(StringComparer.Ordinal);
        var index = 0;
        foreach (var phrase in phrases)
        {
            var validated = Validate(phrase, ++index);
            if (!identities.Add(validated.Code + "\0" + validated.Phrase))
                throw new InvalidDataException($"第 {index} 条：输入码和短语重复。");
            yield return validated;
        }
    }
}
