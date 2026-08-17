using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ShuruSettings;

public sealed record SkinDescriptor(
    string Id,
    string Name,
    string Author,
    string Info,
    string DirectoryPath,
    string PreviewPath,
    string PinyinColor,
    string CandidateColor,
    string HighlightColor,
    string HighlightBackgroundColor,
    string StatusTextColor,
    string SeparatorColor,
    bool UsesNativeLayout,
    bool IsBuiltIn,
    bool IsAvailable,
    string ValidationError);

public sealed class SkinCardViewModel
{
    public SkinCardViewModel(SkinDescriptor descriptor, bool isCurrent)
    {
        Descriptor = descriptor;
        IsCurrent = isCurrent;
        PreviewBackground = LoadPreviewBrush(
            descriptor.PreviewPath,
            descriptor.UsesNativeLayout,
            out var previewWidth,
            out var previewHeight);
        PreviewWidth = previewWidth;
        PreviewHeight = previewHeight;
        PinyinBrush = ColorBrush(descriptor.PinyinColor, Colors.Black);
        CandidateBrush = ColorBrush(descriptor.CandidateColor, Color.FromRgb(31, 41, 55));
        HighlightBrush = ColorBrush(descriptor.HighlightColor, Colors.White);
        HighlightBackgroundBrush = ColorBrush(
            descriptor.HighlightBackgroundColor, Color.FromRgb(47, 107, 255));
        StatusTextBrush = ColorBrush(
            descriptor.StatusTextColor, Color.FromRgb(138, 148, 163));
        SeparatorBrush = ColorBrush(
            descriptor.SeparatorColor, Color.FromRgb(230, 233, 239));
        CardBorderBrush = isCurrent
            ? ColorBrush("0x2F6BFF", Colors.Blue)
            : descriptor.IsAvailable
                ? ColorBrush("0xE6E9EF", Colors.LightGray)
                : ColorBrush("0xFECACA", Colors.LightPink);
        CardBackgroundBrush = isCurrent
            ? ColorBrush("0xF7F9FF", Colors.White)
            : ColorBrush("0xFBFCFE", Colors.White);
    }

    public SkinDescriptor Descriptor { get; }
    public string Id => Descriptor.Id;
    public string Name => Descriptor.Name;
    public string AuthorLine => $"作者：{Descriptor.Author}";
    public string Info => Descriptor.Info;
    public string SourceLabel => Descriptor.IsBuiltIn
        ? Id == SkinCatalog.DefaultSkinId ? "内置皮肤 · 默认" : "内置皮肤"
        : "已导入";
    public string ValidationText => Descriptor.IsAvailable
        ? string.Empty : $"不可用：{Descriptor.ValidationError}";
    public bool IsCurrent { get; }
    public bool CanApply => Descriptor.IsAvailable && !IsCurrent;
    public bool CanDelete => !Descriptor.IsBuiltIn;
    public string ActionText => !Descriptor.IsAvailable
        ? "不可用" : IsCurrent ? "使用中" : "应用此皮肤";
    public Brush PreviewBackground { get; }
    public double PreviewWidth { get; }
    public double PreviewHeight { get; }
    public Brush PinyinBrush { get; }
    public Brush CandidateBrush { get; }
    public Brush HighlightBrush { get; }
    public Brush HighlightBackgroundBrush { get; }
    public Brush StatusTextBrush { get; }
    public Brush SeparatorBrush { get; }
    public Brush CardBorderBrush { get; }
    public Brush CardBackgroundBrush { get; }

    private static Brush LoadPreviewBrush(
        string path,
        bool usesNativeLayout,
        out double previewWidth,
        out double previewHeight)
    {
        const double scalableWidth = 420;
        const double scalableHeight = 82;
        previewWidth = scalableWidth;
        previewHeight = scalableHeight;
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            return ColorBrush("0xFFFFFF", Colors.White);
        try
        {
            using var stream = new FileStream(
                path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.StreamSource = stream;
            bitmap.EndInit();
            bitmap.Freeze();
            if (usesNativeLayout && bitmap.PixelWidth > 0 && bitmap.PixelHeight > 0)
            {
                const double maximumWidth = 560;
                const double maximumHeight = 180;
                var scale = Math.Min(
                    1.0,
                    Math.Min(
                        maximumWidth / bitmap.PixelWidth,
                        maximumHeight / bitmap.PixelHeight));
                previewWidth = Math.Max(1, bitmap.PixelWidth * scale);
                previewHeight = Math.Max(1, bitmap.PixelHeight * scale);
            }
            var brush = new ImageBrush(bitmap)
            {
                Stretch = usesNativeLayout ? Stretch.Uniform : Stretch.UniformToFill,
                AlignmentX = AlignmentX.Center,
                AlignmentY = AlignmentY.Center
            };
            brush.Freeze();
            return brush;
        }
        catch
        {
            return ColorBrush("0xFFFFFF", Colors.White);
        }
    }

    private static SolidColorBrush ColorBrush(string value, Color fallback)
    {
        var color = SkinCatalog.TryParseColor(value, out var parsed)
            ? parsed : fallback;
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        return brush;
    }
}

public static class SkinCatalog
{
    public const string DefaultSkinId = "classic_blue";

    private static readonly string[] BuiltInOrder =
    {
        "classic_blue", "classic_gold", "minimal_light",
        "cyber_dark", "sakura_pink", "celadon_jade"
    };

    public static string UserSkinsDirectory =>
        Environment.GetEnvironmentVariable("CAISHEN_SKIN_DATA_DIR") ??
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CaishenPinyin", "skins");

    public static IReadOnlyList<SkinDescriptor> Load(
        string? builtInDirectory = null,
        string? userDirectory = null)
    {
        builtInDirectory ??= FindBuiltInDirectory();
        userDirectory ??= UserSkinsDirectory;
        var entries = new Dictionary<string, SkinDescriptor>(
            StringComparer.OrdinalIgnoreCase);

        ScanDirectory(builtInDirectory, isBuiltIn: true, entries);
        ScanDirectory(userDirectory, isBuiltIn: false, entries);

        var order = BuiltInOrder
            .Select((id, index) => (id, index))
            .ToDictionary(item => item.id, item => item.index,
                StringComparer.OrdinalIgnoreCase);
        return entries.Values
            .OrderBy(item => item.IsBuiltIn ? 0 : 1)
            .ThenBy(item => order.TryGetValue(item.Id, out var index)
                ? index : int.MaxValue)
            .ThenBy(item => item.Name, StringComparer.CurrentCultureIgnoreCase)
            .ThenBy(item => item.Id, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public static string ResolveSelectedId(
        string? selectedId,
        IReadOnlyList<SkinDescriptor> skins)
    {
        var selected = skins.FirstOrDefault(item =>
            item.IsAvailable && string.Equals(
                item.Id, selectedId, StringComparison.OrdinalIgnoreCase));
        if (selected != null) return selected.Id;
        var fallback = skins.FirstOrDefault(item =>
            item.IsAvailable && string.Equals(
                item.Id, DefaultSkinId, StringComparison.OrdinalIgnoreCase));
        return fallback?.Id ?? skins.FirstOrDefault(item => item.IsAvailable)?.Id ??
            DefaultSkinId;
    }

    public static string CreateUniqueUserSkinId(
        string sourceFilePath,
        IReadOnlyCollection<string> reservedIds,
        string? userDirectory = null)
    {
        userDirectory ??= UserSkinsDirectory;
        var baseName = Path.GetFileNameWithoutExtension(sourceFilePath).Trim();
        var invalid = Path.GetInvalidFileNameChars().ToHashSet();
        baseName = new string(baseName
            .Select(character => invalid.Contains(character) || char.IsControl(character)
                ? '_' : character)
            .ToArray()).Trim(' ', '.');
        if (baseName.Length > 64) baseName = baseName[..64].TrimEnd(' ', '.');
        if (!IsValidSkinId(baseName)) baseName = "skin";

        var occupied = new HashSet<string>(reservedIds, StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(userDirectory))
        {
            foreach (var directory in Directory.EnumerateDirectories(userDirectory))
                occupied.Add(Path.GetFileName(directory));
        }

        var candidate = baseName;
        for (var suffix = 2; occupied.Contains(candidate) ||
             Directory.Exists(Path.Combine(userDirectory, candidate)); ++suffix)
        {
            var suffixText = $"-{suffix}";
            var maximumBaseLength = Math.Max(1, 64 - suffixText.Length);
            candidate = baseName[..Math.Min(baseName.Length, maximumBaseLength)] +
                suffixText;
        }
        return candidate;
    }

    public static bool IsValidSkinId(string? value) =>
        !string.IsNullOrWhiteSpace(value) && value.Length <= 128 &&
        value is not "." and not ".." &&
        value.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 &&
        !value.Any(char.IsControl) &&
        string.Equals(Path.GetFileName(value), value, StringComparison.Ordinal);

    public static void DeleteUserSkin(
        SkinDescriptor skin,
        string? userDirectory = null)
    {
        ArgumentNullException.ThrowIfNull(skin);
        if (skin.IsBuiltIn)
            throw new InvalidOperationException("内置皮肤不能删除。");
        if (!IsValidSkinId(skin.Id))
            throw new InvalidDataException("皮肤标识无效，已拒绝删除。");

        userDirectory ??= UserSkinsDirectory;
        var root = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(userDirectory));
        if (!Directory.Exists(root))
            throw new DirectoryNotFoundException("用户皮肤根目录不存在。");
        if ((File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("用户皮肤根目录是链接，已拒绝递归删除。");
        var target = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(skin.DirectoryPath));
        var expectedTarget = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(Path.Combine(root, skin.Id)));
        if (!string.Equals(target, expectedTarget, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetDirectoryName(target), root,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "皮肤目录不属于用户皮肤根目录，已拒绝删除。");
        }
        if (!Directory.Exists(target))
            throw new DirectoryNotFoundException("要删除的皮肤目录已不存在。");

        EnsureDirectoryTreeContainsNoReparsePoints(target);
        Directory.Delete(target, recursive: true);
    }

    internal static bool TryParseColor(string? value, out Color color)
    {
        color = default;
        if (string.IsNullOrWhiteSpace(value)) return false;
        var text = value.Trim();
        try
        {
            var number = text.StartsWith('#')
                ? Convert.ToUInt32(text[1..], 16)
                : text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                    ? Convert.ToUInt32(text[2..], 16)
                    : Convert.ToUInt32(text, 10);
            if (number > 0xFFFFFF) return false;
            color = Color.FromRgb(
                (byte)((number >> 16) & 0xFF),
                (byte)((number >> 8) & 0xFF),
                (byte)(number & 0xFF));
            return true;
        }
        catch (FormatException) { return false; }
        catch (OverflowException) { return false; }
    }

    private static string? FindBuiltInDirectory()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for (var depth = 0; directory != null && depth < 8;
             ++depth, directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "data", "skins");
            if (File.Exists(Path.Combine(candidate, DefaultSkinId, "skin.ini")))
                return candidate;
        }
        return null;
    }

    private static void ScanDirectory(
        string? root,
        bool isBuiltIn,
        IDictionary<string, SkinDescriptor> entries)
    {
        if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root)) return;
        IEnumerable<string> directories;
        try { directories = Directory.EnumerateDirectories(root).ToList(); }
        catch (IOException) { return; }
        catch (UnauthorizedAccessException) { return; }

        foreach (var directory in directories)
        {
            var id = Path.GetFileName(directory);
            if (id.StartsWith(".skin-", StringComparison.OrdinalIgnoreCase) ||
                !IsValidSkinId(id) || entries.ContainsKey(id)) continue;
            if (!isBuiltIn)
            {
                try
                {
                    SsfConverter.NormalizeInstalledSkin(directory, id);
                }
                catch (Exception exception) when (exception is IOException or
                    UnauthorizedAccessException or InvalidDataException or
                    ArgumentException)
                {
                    CrashLogger.Log("SsfConverter.NormalizeInstalledSkin", exception);
                }
            }
            entries[id] = ReadDescriptor(directory, id, isBuiltIn);
        }
    }

    private static SkinDescriptor ReadDescriptor(
        string directory,
        string id,
        bool isBuiltIn)
    {
        try
        {
            var ini = IniDocument.Parse(File.ReadAllText(
                Path.Combine(directory, "skin.ini")));
            var general = ini.Section("General");
            var display = ini.Section("Display");
            var scheme = ini.Section("Scheme_H1");
            var name = Value(general, "name", "skin_name") ?? id;
            var author = Value(general, "author", "skin_author") ?? "未知作者";
            var info = Value(general, "info", "description", "skin_info") ??
                "自定义候选框皮肤";
            var configuredBackground = FirstValue(Value(
                scheme, "bg_image", "pic", "background_image"));
            if (string.IsNullOrWhiteSpace(configuredBackground))
                return Invalid("皮肤配置缺少候选框背景图");
            var configuredPreview = FirstValue(Value(
                general, "preview_image", "preview_comp"));
            var preview = (configuredPreview == null
                    ? null : ResolveAsset(directory, configuredPreview)) ??
                ResolveAsset(directory, configuredBackground);
            if (preview == null)
                return Invalid("候选框背景图不存在或路径无效");
            return new SkinDescriptor(
                id, Clean(name, id), Clean(author, "未知作者"),
                Clean(info, "自定义候选框皮肤"), directory, preview,
                Value(display, "pinyin_color") ?? "0x1F2937",
                Value(display, "candidate_color", "zhongwen_color") ?? "0x111827",
                Value(display, "highlight_color", "zhongwen_first_color") ?? "0xFFFFFF",
                Value(display, "highlight_bg_color") ?? "0x2F6BFF",
                Value(display, "status_text_color") ?? "0x8A94A3",
                Value(display, "separator_color") ?? "0xE6E9EF",
                ParseBool(Value(scheme, "native_appearance")),
                isBuiltIn, true, string.Empty);

            SkinDescriptor Invalid(string message) => new(
                id, Clean(name, id), Clean(author, "未知作者"),
                Clean(info, "自定义候选框皮肤"), directory, string.Empty,
                "0x1F2937", "0x111827", "0xFFFFFF", "0x2F6BFF",
                "0x8A94A3", "0xE6E9EF", false,
                isBuiltIn, false, message);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or
                                   InvalidDataException or ArgumentException)
        {
            return new SkinDescriptor(
                id, id, "未知作者", "无法读取此皮肤", directory, string.Empty,
                "0x1F2937", "0x111827", "0xFFFFFF", "0x2F6BFF",
                "0x8A94A3", "0xE6E9EF", false,
                isBuiltIn, false,
                ex is UnauthorizedAccessException ? "没有读取权限" : "配置文件损坏");
        }
    }

    private static void EnsureDirectoryTreeContainsNoReparsePoints(string root)
    {
        var pending = new Stack<string>();
        pending.Push(root);
        while (pending.Count > 0)
        {
            var directory = pending.Pop();
            if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("皮肤目录包含链接，已拒绝递归删除。");
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                var attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("皮肤目录包含链接，已拒绝递归删除。");
                if ((attributes & FileAttributes.Directory) != 0)
                    pending.Push(entry);
            }
        }
    }

    private static string? ResolveAsset(string root, string configuredPath)
    {
        try
        {
            var relative = configuredPath.Trim().Trim('"')
                .Replace('/', Path.DirectorySeparatorChar);
            var candidate = Path.GetFullPath(Path.Combine(root, relative));
            var normalizedRoot = Path.GetFullPath(root)
                .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            return candidate.StartsWith(
                       normalizedRoot, StringComparison.OrdinalIgnoreCase) &&
                   File.Exists(candidate)
                ? candidate : null;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or
                                   UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string? Value(
        IReadOnlyDictionary<string, string>? section,
        params string[] names)
    {
        if (section == null) return null;
        foreach (var name in names)
            if (section.TryGetValue(name, out var value) &&
                !string.IsNullOrWhiteSpace(value)) return value.Trim();
        return null;
    }

    private static string? FirstValue(string? value) =>
        value?.Split(',')[0].Trim();

    private static bool ParseBool(string? value)
    {
        var normalized = value?.Trim();
        return normalized == "1" ||
            string.Equals(normalized, "true", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(normalized, "yes", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(normalized, "on", StringComparison.OrdinalIgnoreCase);
    }

    private static string Clean(string value, string fallback)
    {
        var cleaned = value.Replace('\0', ' ').Replace('\r', ' ')
            .Replace('\n', ' ').Trim();
        return string.IsNullOrWhiteSpace(cleaned) ? fallback : cleaned;
    }

    private sealed class IniDocument
    {
        private readonly Dictionary<string, Dictionary<string, string>> sections_ =
            new(StringComparer.OrdinalIgnoreCase);

        internal IReadOnlyDictionary<string, string>? Section(string name) =>
            sections_.TryGetValue(name, out var section) ? section : null;

        internal static IniDocument Parse(string text)
        {
            var result = new IniDocument();
            Dictionary<string, string>? current = null;
            foreach (var rawLine in text.Replace("\0", string.Empty).Split('\n'))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] is ';' or '#') continue;
                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    var sectionName = line[1..^1].Trim();
                    if (!result.sections_.TryGetValue(sectionName, out current))
                    {
                        current = new Dictionary<string, string>(
                            StringComparer.OrdinalIgnoreCase);
                        result.sections_[sectionName] = current;
                    }
                    continue;
                }
                var separator = line.IndexOf('=');
                if (separator <= 0 || current == null) continue;
                current[line[..separator].Trim()] = line[(separator + 1)..].Trim();
            }
            return result;
        }
    }
}
