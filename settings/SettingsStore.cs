using System.Globalization;
using System.IO;

namespace ShuruSettings;

public sealed record AppSettings(
    bool EnglishDefault = false,
    bool LearningEnabled = true,
    bool ContentLogging = false,
    bool FuzzyEnabled = true,
    bool FuzzyInitials = true,
    bool FuzzyFinals = true,
    bool FuzzyMissingVowel = true,
    bool ShuangpinXiaohe = false,
    bool FullWidthPunctuation = true,
    int CandidateCount = 9,
    int CandidateFontSize = 19,
    string DisplayName = SettingsStore.DefaultDisplayName,
    bool VModeOpenWindow = false,
    bool VvModeOpenWindow = false)
{
    public AppSettings Validated() => this with
    {
        CandidateCount = CandidateCount is >= 3 and <= 9 ? CandidateCount : 9,
        CandidateFontSize = CandidateFontSize is >= 14 and <= 32 ? CandidateFontSize : 19,
        DisplayName = SettingsStore.NormalizeDisplayName(DisplayName) ?? SettingsStore.DefaultDisplayName
    };
}

public static class SettingsStore
{
    public const string DefaultDisplayName = "财神输入法";

    public static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CaishenPinyin", "settings.ini");

    public static AppSettings Load(string? path = null)
    {
        var defaults = new AppSettings();
        path ??= DefaultPath;
        try
        {
            if (!File.Exists(path)) return defaults;
            var values = File.ReadLines(path)
                .Select(line => line.Split('=', 2))
                .Where(parts => parts.Length == 2)
                .ToDictionary(parts => parts[0].Trim(), parts => parts[1].Trim(), StringComparer.OrdinalIgnoreCase);
            bool B(string key, bool fallback) => values.TryGetValue(key, out var v) &&
                (v == "0" || v == "1") ? v == "1" : fallback;
            int I(string key, int fallback) => values.TryGetValue(key, out var v) &&
                int.TryParse(v, NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : fallback;
            string S(string key, string fallback) => values.TryGetValue(key, out var v) ? v : fallback;
            return new AppSettings(
                B("EnglishDefault", defaults.EnglishDefault),
                B("LearningEnabled", defaults.LearningEnabled),
                B("ContentLogging", defaults.ContentLogging),
                B("FuzzyEnabled", defaults.FuzzyEnabled),
                B("FuzzyInitials", defaults.FuzzyInitials),
                B("FuzzyFinals", defaults.FuzzyFinals),
                B("FuzzyMissingVowel", defaults.FuzzyMissingVowel),
                B("ShuangpinXiaohe", defaults.ShuangpinXiaohe),
                B("FullWidthPunctuation", defaults.FullWidthPunctuation),
                I("CandidateCount", defaults.CandidateCount),
                I("CandidateFontSize", defaults.CandidateFontSize),
                S("DisplayName", defaults.DisplayName),
                B("VModeOpenWindow", defaults.VModeOpenWindow),
                B("VvModeOpenWindow", defaults.VvModeOpenWindow)).Validated();
        }
        catch (IOException) { return defaults; }
        catch (UnauthorizedAccessException) { return defaults; }
        catch (ArgumentException) { return defaults; }
    }

    public static void Save(AppSettings settings, string? path = null)
    {
        settings = settings.Validated();
        path ??= DefaultPath;
        var directory = Path.GetDirectoryName(path) ?? throw new InvalidOperationException("设置路径无效。");
        Directory.CreateDirectory(directory);
        var temp = path + ".tmp-" + Guid.NewGuid().ToString("N");
        var content = string.Join("\n", new[]
        {
            "# Caishen Pinyin settings v1",
            $"EnglishDefault={Bit(settings.EnglishDefault)}",
            $"LearningEnabled={Bit(settings.LearningEnabled)}",
            $"ContentLogging={Bit(settings.ContentLogging)}",
            $"FuzzyEnabled={Bit(settings.FuzzyEnabled)}",
            $"FuzzyInitials={Bit(settings.FuzzyInitials)}",
            $"FuzzyFinals={Bit(settings.FuzzyFinals)}",
            $"FuzzyMissingVowel={Bit(settings.FuzzyMissingVowel)}",
            $"ShuangpinXiaohe={Bit(settings.ShuangpinXiaohe)}",
            $"FullWidthPunctuation={Bit(settings.FullWidthPunctuation)}",
            $"CandidateCount={settings.CandidateCount}",
            $"CandidateFontSize={settings.CandidateFontSize}",
            $"DisplayName={settings.DisplayName}",
            $"VModeOpenWindow={Bit(settings.VModeOpenWindow)}",
            $"VvModeOpenWindow={Bit(settings.VvModeOpenWindow)}",
            ""
        });
        try
        {
            using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                       4096, FileOptions.WriteThrough))
            using (var writer = new StreamWriter(stream, new System.Text.UTF8Encoding(false)))
            {
                writer.Write(content);
                writer.Flush();
                stream.Flush(true);
            }
            File.Move(temp, path, true); // same-volume atomic replacement
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
    }

    private static int Bit(bool value) => value ? 1 : 0;

    public static string? NormalizeDisplayName(string? value)
    {
        var normalized = value?.Trim();
        return !string.IsNullOrEmpty(normalized) && normalized.Length <= 24 &&
               normalized.All(ch => !char.IsControl(ch) && !char.IsSurrogate(ch))
            ? normalized
            : null;
    }
}
