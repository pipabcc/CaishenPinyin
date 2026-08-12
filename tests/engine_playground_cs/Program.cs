using System.Text;

namespace ShuruEnginePlayground;

internal sealed class Candidate
{
    public string Text { get; init; } = "";
    public string Pinyin { get; init; } = "";
    public int Frequency { get; init; }
}

internal sealed class Dictionary
{
    private readonly Dictionary<string, List<(string Word, int Frequency)>> _map = new(StringComparer.Ordinal);

    public bool Load(string path)
    {
        if (!File.Exists(path)) return false;
        foreach (var raw in File.ReadLines(path, Encoding.UTF8))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var parts = line.Split('\t');
            if (parts.Length < 2) continue;
            var py = parts[0].Trim().ToLowerInvariant();
            var word = parts[1].Trim();
            var freq = 1;
            if (parts.Length >= 3 && int.TryParse(parts[2].Trim(), out var parsed)) freq = parsed;
            if (py.Length == 0 || word.Length == 0) continue;
            if (!_map.TryGetValue(py, out var list))
            {
                list = new List<(string, int)>();
                _map[py] = list;
            }
            var idx = list.FindIndex(x => x.Word == word);
            if (idx >= 0) list[idx] = (word, Math.Max(list[idx].Frequency, freq));
            else list.Add((word, freq));
        }
        foreach (var key in _map.Keys.ToList())
        {
            _map[key] = _map[key].OrderByDescending(x => x.Frequency).ThenBy(x => x.Word).ToList();
        }
        return _map.Count > 0;
    }

    public IEnumerable<Candidate> LookupExact(string pinyin)
    {
        if (!_map.TryGetValue(pinyin, out var list)) yield break;
        foreach (var item in list)
            yield return new Candidate { Text = item.Word, Pinyin = pinyin, Frequency = item.Frequency };
    }

    public IEnumerable<Candidate> LookupPrefix(string prefix, int limit)
    {
        return _map.Where(kv => kv.Key.StartsWith(prefix, StringComparison.Ordinal))
            .SelectMany(kv => kv.Value.Select(v => new Candidate { Text = v.Word, Pinyin = kv.Key, Frequency = v.Frequency }))
            .OrderByDescending(c => c.Frequency)
            .ThenBy(c => c.Pinyin.Length)
            .ThenBy(c => c.Text)
            .Take(limit);
    }
}

internal static class Program
{
    private static string Normalize(string input)
        => new string(input.Where(ch => ch is >= 'a' and <= 'z' or >= 'A' and <= 'Z')
            .Select(char.ToLowerInvariant).ToArray());

    private static List<Candidate> Query(Dictionary dict, string raw, int limit = 9)
    {
        var pinyin = Normalize(raw);
        var result = new List<Candidate>();
        var seen = new HashSet<string>();

        void AddRange(IEnumerable<Candidate> items)
        {
            foreach (var item in items)
            {
                if (!seen.Add(item.Text)) continue;
                result.Add(item);
                if (result.Count >= limit) return;
            }
        }

        if (pinyin.Length == 0) return result;
        AddRange(dict.LookupExact(pinyin));
        if (result.Count >= limit) return result;
        AddRange(dict.LookupPrefix(pinyin, limit * 4));
        if (result.Count >= limit) return result;
        for (var len = pinyin.Length; len > 0 && result.Count < limit; len--)
        {
            AddRange(dict.LookupExact(pinyin[..len]));
        }
        if (result.Count == 0)
        {
            result.Add(new Candidate { Text = pinyin, Pinyin = pinyin, Frequency = 0 });
        }
        return result;
    }

    public static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        var root = FindRepoRoot();
        var dictPath = args.Length > 0 ? args[0] : Path.Combine(root, "data", "lexicon", "base_dict.txt");
        var dict = new Dictionary();
        if (!dict.Load(dictPath))
        {
            Console.WriteLine($"词库加载失败: {dictPath}");
            return 1;
        }

        Console.WriteLine("发财拼音引擎演练 (C#)。输入拼音后回车，quit 退出。");
        Console.WriteLine($"词库: {dictPath}");
        while (true)
        {
            Console.Write("> ");
            var line = Console.ReadLine();
            if (line is null) break;
            if (line is "quit" or "exit") break;
            var cands = Query(dict, line);
            for (var i = 0; i < cands.Count; i++)
            {
                var c = cands[i];
                Console.WriteLine($"{i + 1}. {c.Text}  [{c.Pinyin}] f={c.Frequency}");
            }
        }
        return 0;
    }

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "data", "lexicon", "base_dict.txt")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return Directory.GetCurrentDirectory();
    }
}
