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
    string CandidateFontFamily = "",
    string CandidateFontSizeMode = SettingsStore.FollowSkinFontSizeMode,
    string DisplayName = SettingsStore.DefaultDisplayName,
    bool VModeOpenWindow = false,
    bool VvModeOpenWindow = false,
    string SkinId = "classic_blue")
{
    public AppSettings Validated() => this with
    {
        CandidateCount = CandidateCount is >= 3 and <= 11 ? CandidateCount : 9,
        CandidateFontFamily = SettingsStore.NormalizeCandidateFontFamily(CandidateFontFamily) ?? "",
        CandidateFontSizeMode = SettingsStore.NormalizeCandidateFontSizeMode(CandidateFontSizeMode),
        DisplayName = SettingsStore.NormalizeDisplayName(DisplayName) ?? SettingsStore.DefaultDisplayName,
        SkinId = string.IsNullOrWhiteSpace(SkinId) ? "classic_blue" : SkinId.Trim()
    };
}

public static class SettingsStore
{
    public const string DefaultDisplayName = "财神输入法";
    public const string FollowSkinFontSizeMode = "follow_skin";

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
            var fontSizeMode = values.ContainsKey("CandidateFontSizeMode")
                ? S("CandidateFontSizeMode", FollowSkinFontSizeMode)
                : CandidateFontSizeModeFromLegacy(I("CandidateFontSize", -1));
            return new AppSettings(
                EnglishDefault: B("EnglishDefault", defaults.EnglishDefault),
                LearningEnabled: B("LearningEnabled", defaults.LearningEnabled),
                ContentLogging: B("ContentLogging", defaults.ContentLogging),
                FuzzyEnabled: B("FuzzyEnabled", defaults.FuzzyEnabled),
                FuzzyInitials: B("FuzzyInitials", defaults.FuzzyInitials),
                FuzzyFinals: B("FuzzyFinals", defaults.FuzzyFinals),
                FuzzyMissingVowel: B("FuzzyMissingVowel", defaults.FuzzyMissingVowel),
                ShuangpinXiaohe: B("ShuangpinXiaohe", defaults.ShuangpinXiaohe),
                FullWidthPunctuation: B("FullWidthPunctuation", defaults.FullWidthPunctuation),
                CandidateCount: I("CandidateCount", defaults.CandidateCount),
                CandidateFontFamily: S("CandidateFontFamily", defaults.CandidateFontFamily),
                CandidateFontSizeMode: fontSizeMode,
                DisplayName: S("DisplayName", defaults.DisplayName),
                VModeOpenWindow: B("VModeOpenWindow", defaults.VModeOpenWindow),
                VvModeOpenWindow: B("VvModeOpenWindow", defaults.VvModeOpenWindow),
                SkinId: S("SkinId", defaults.SkinId)).Validated();
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
            "# Caishen Pinyin settings v2",
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
            $"CandidateFontFamily={settings.CandidateFontFamily}",
            $"CandidateFontSizeMode={settings.CandidateFontSizeMode}",
            $"CandidateFontSize={LegacyCandidateFontSize(settings.CandidateFontSizeMode)}",
            $"DisplayName={settings.DisplayName}",
            $"VModeOpenWindow={Bit(settings.VModeOpenWindow)}",
            $"VvModeOpenWindow={Bit(settings.VvModeOpenWindow)}",
            $"SkinId={settings.SkinId}",
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

    public static string? NormalizeCandidateFontFamily(string? value)
    {
        var normalized = value?.Trim();
        if (string.IsNullOrEmpty(normalized)) return null;
        return normalized.Length <= 64 &&
               normalized.All(ch => !char.IsControl(ch) && !char.IsSurrogate(ch))
            ? normalized
            : null;
    }

    public static string NormalizeCandidateFontSizeMode(string? value) =>
        value?.Trim().ToLowerInvariant() switch
        {
            "small" => "small",
            "standard" => "standard",
            "large" => "large",
            "extra_large" => "extra_large",
            _ => FollowSkinFontSizeMode
        };

    public static string CandidateFontSizeModeFromLegacy(int value) => value switch
    {
        >= 14 and <= 17 => "small",
        >= 18 and <= 20 => "standard",
        >= 21 and <= 24 => "large",
        >= 25 and <= 32 => "extra_large",
        _ => FollowSkinFontSizeMode
    };

    public static int LegacyCandidateFontSize(string? mode) =>
        NormalizeCandidateFontSizeMode(mode) switch
        {
            "small" => 16,
            "standard" => 19,
            "large" => 22,
            "extra_large" => 26,
            _ => 19
        };
}
