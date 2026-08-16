using System.Buffers.Binary;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ShuruSettings;

public static class SsfConverter
{
    private const long MaxSourceBytes = 128L * 1024 * 1024;
    private const long MaxExtractedBytes = 256L * 1024 * 1024;
    private const int MaxArchiveEntries = 512;
    private static readonly byte[] SogouSsfV3Magic = { 0x53, 0x6B, 0x69, 0x6E };
    private static readonly byte[] SsfAesKey =
    {
        0x52, 0x36, 0x46, 0x1A, 0xD3, 0x85, 0x03, 0x66,
        0x90, 0x45, 0x16, 0x28, 0x79, 0x03, 0x36, 0x23,
        0xDD, 0xBE, 0x6F, 0x03, 0xFF, 0x04, 0xE3, 0xCA,
        0xD5, 0x7F, 0xFC, 0xA3, 0x50, 0xE4, 0x9E, 0xD9
    };
    private static readonly byte[] SsfAesIv =
    {
        0xE0, 0x7A, 0xAD, 0x35, 0xE0, 0x90, 0xAA, 0x03,
        0x8A, 0x51, 0xFD, 0x05, 0xDF, 0x8C, 0x5D, 0x0F
    };

    public static string ConvertAndInstall(string sourceFilePath, string outputDirectory)
    {
        var source = Path.GetFullPath(sourceFilePath);
        var output = Path.GetFullPath(outputDirectory);
        if (!File.Exists(source)) throw new FileNotFoundException("皮肤文件不存在", source);
        if (new FileInfo(source).Length > MaxSourceBytes)
            throw new InvalidDataException("皮肤包超过 128 MB 安全限制。");

        var outputParent = Path.GetDirectoryName(output) ??
            throw new InvalidDataException("皮肤安装目录无效。");
        Directory.CreateDirectory(outputParent);
        var workRoot = Path.Combine(outputParent, ".skin-import-" + Guid.NewGuid().ToString("N"));
        var unpacked = Path.Combine(workRoot, "unpacked");
        var normalized = Path.Combine(workRoot, "normalized");
        Directory.CreateDirectory(unpacked);
        Directory.CreateDirectory(normalized);

        try
        {
            var fileBytes = File.ReadAllBytes(source);
            if (IsSogouSsf(fileBytes)) ExtractSogouSsf(fileBytes, unpacked);
            else if (IsZip(fileBytes)) ExtractZip(fileBytes, unpacked);
            else throw new InvalidDataException("不支持的皮肤文件格式，仅支持 .ssf 或标准 .zip 皮肤包。");

            var contentRoot = LocateContentRoot(unpacked);
            CopyDirectory(contentRoot, normalized);
            NormalizeSkin(normalized, Path.GetFileNameWithoutExtension(source));
            ReplaceDirectory(normalized, output);
            return Path.GetFileName(output);
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    public static bool NormalizeInstalledSkin(string skinDirectory, string fallbackName)
    {
        var source = Path.GetFullPath(skinDirectory);
        if (!Directory.Exists(source)) return false;
        var iniPath = Path.Combine(source, "skin.ini");
        if (!File.Exists(iniPath) || IsNormalizedConfiguration(iniPath)) return false;

        var parent = Path.GetDirectoryName(source) ??
            throw new InvalidDataException("皮肤目录无效。");
        var workRoot = Path.Combine(parent, ".skin-migrate-" + Guid.NewGuid().ToString("N"));
        var staged = Path.Combine(workRoot, "normalized");
        try
        {
            CopyDirectory(source, staged);
            NormalizeSkin(staged, fallbackName);
            ReplaceDirectory(staged, source);
            return true;
        }
        finally
        {
            TryDeleteDirectory(workRoot);
        }
    }

    private static bool IsSogouSsf(byte[] data) =>
        data.AsSpan().StartsWith(SogouSsfV3Magic);

    private static bool IsZip(byte[] data) => data.Length >= 4 &&
        data[0] == 0x50 && data[1] == 0x4B && data[2] == 0x03 && data[3] == 0x04;

    private static void ExtractZip(byte[] data, string outputDirectory)
    {
        using var stream = new MemoryStream(data, writable: false);
        using var archive = new ZipArchive(stream, ZipArchiveMode.Read);
        if (archive.Entries.Count > MaxArchiveEntries)
            throw new InvalidDataException("皮肤包文件数量超过安全限制。");

        long extractedBytes = 0;
        var root = EnsureTrailingSeparator(Path.GetFullPath(outputDirectory));
        foreach (var entry in archive.Entries)
        {
            extractedBytes = checked(extractedBytes + entry.Length);
            if (extractedBytes > MaxExtractedBytes)
                throw new InvalidDataException("皮肤包解压后超过 256 MB 安全限制。");

            var destination = Path.GetFullPath(Path.Combine(outputDirectory,
                entry.FullName.Replace('/', Path.DirectorySeparatorChar)));
            if (!destination.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("皮肤包包含越界路径。");
            if (string.IsNullOrEmpty(entry.Name))
            {
                Directory.CreateDirectory(destination);
                continue;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            using var input = entry.Open();
            using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            CopyWithLimit(input, output, entry.Length);
        }
    }

    private static void ExtractSogouSsf(byte[] ssfData, string outputDirectory)
    {
        if (ssfData.Length <= 24) throw new InvalidDataException("SSF 文件损坏或过短。");
        var cipherLength = (ssfData.Length - 8) / 16 * 16;
        if (cipherLength <= 0) throw new InvalidDataException("SSF 加密数据不完整。");

        byte[] decrypted;
        using (var aes = Aes.Create())
        {
            aes.Key = SsfAesKey;
            aes.IV = SsfAesIv;
            aes.Mode = CipherMode.CBC;
            aes.Padding = PaddingMode.None;
            using var decryptor = aes.CreateDecryptor();
            decrypted = decryptor.TransformFinalBlock(ssfData, 8, cipherLength);
        }
        if (decrypted.Length < 8) throw new InvalidDataException("解密后的 SSF 数据损坏。");

        byte[] archiveBytes;
        using (var input = new MemoryStream(decrypted, 4, decrypted.Length - 4, writable: false))
        using (var zlib = new ZLibStream(input, CompressionMode.Decompress))
        using (var output = new MemoryStream())
        {
            CopyWithLimit(zlib, output, MaxExtractedBytes);
            archiveBytes = output.ToArray();
        }
        if (archiveBytes.Length < 12) throw new InvalidDataException("SSF 内部归档损坏。");

        var offsetsSize = ReadUInt32LittleEndian(archiveBytes, 4);
        if (offsetsSize == 0 || offsetsSize % 4 != 0 || offsetsSize > archiveBytes.Length - 8)
            throw new InvalidDataException("SSF 文件索引损坏。");
        var entryCount = checked((int)(offsetsSize / 4));
        if (entryCount > MaxArchiveEntries)
            throw new InvalidDataException("SSF 文件数量超过安全限制。");

        long extractedBytes = 0;
        for (var index = 0; index < entryCount; ++index)
        {
            var pointer = checked((int)ReadUInt32LittleEndian(archiveBytes, 8 + index * 4));
            var nameLength = checked((int)ReadUInt32LittleEndian(archiveBytes, pointer));
            pointer = checked(pointer + 4);
            EnsureRange(archiveBytes, pointer, nameLength);
            var fileName = Path.GetFileName(Encoding.Unicode.GetString(archiveBytes, pointer, nameLength)
                .TrimEnd('\0'));
            pointer = checked(pointer + nameLength);
            var contentLength = checked((int)ReadUInt32LittleEndian(archiveBytes, pointer));
            pointer = checked(pointer + 4);
            EnsureRange(archiveBytes, pointer, contentLength);
            extractedBytes = checked(extractedBytes + contentLength);
            if (extractedBytes > MaxExtractedBytes)
                throw new InvalidDataException("SSF 解包后超过 256 MB 安全限制。");
            if (string.IsNullOrWhiteSpace(fileName)) continue;
            File.WriteAllBytes(Path.Combine(outputDirectory, fileName),
                archiveBytes.AsSpan(pointer, contentLength).ToArray());
        }
    }

    private static string LocateContentRoot(string unpacked)
    {
        var iniFiles = Directory.GetFiles(unpacked, "skin.ini", SearchOption.AllDirectories);
        if (iniFiles.Length == 0) return unpacked;
        return Path.GetDirectoryName(iniFiles
            .OrderBy(path => path.Count(character => character is '\\' or '/'))
            .First())!;
    }

    private static void NormalizeSkin(string skinDirectory, string fallbackName)
    {
        var originalIniPath = Path.Combine(skinDirectory, "skin.ini");
        IniDocument? original = null;
        if (File.Exists(originalIniPath))
        {
            var originalBytes = File.ReadAllBytes(originalIniPath);
            File.WriteAllBytes(Path.Combine(skinDirectory, "source_skin.ini"), originalBytes);
            original = IniDocument.Parse(DecodeText(originalBytes));
        }

        var scheme = original?.Section("Scheme_H1");
        var display = original?.Section("Display");
        var general = original?.Section("General");
        var isSogouLayout = scheme != null &&
            (scheme.ContainsKey("pic") || scheme.ContainsKey("pinyin_marge") ||
             scheme.ContainsKey("zhongwen_marge"));

        var backgroundName = FirstValue(GetValue(scheme, "pic", "bg_image", "background_image"));
        var backgroundPath = ResolveAsset(skinDirectory, backgroundName) ??
            FindFallbackBackground(skinDirectory);
        if (backgroundPath == null)
            throw new InvalidDataException("皮肤包没有可用的候选框背景图片。");

        var animation = ApngDecoder.Decode(backgroundPath,
            Path.Combine(skinDirectory, "frames"));
        var backgroundRelative = animation.Count > 1
            ? Path.Combine("frames", "frame_000.png")
            : Path.GetRelativePath(skinDirectory, backgroundPath);

        var pinyinMargin = NormalizeIntList(GetValue(scheme, "pinyin_margin", "pinyin_marge"),
            "10,6,16,16", 4);
        var candidateMargin = NormalizeIntList(GetValue(scheme, "candidate_margin", "zhongwen_marge"),
            "6,10,16,16", 4);
        var horizontal = NormalizeIntList(GetValue(scheme, "layout_horizontal"), "0,16,16", 3);
        var vertical = NormalizeIntList(GetValue(scheme, "layout_vertical"), "0,16,16", 3);

        var name = GetValue(general, "skin_name", "name") ?? fallbackName;
        var author = GetValue(general, "skin_author", "author") ?? "转换导入";
        var info = GetValue(general, "skin_info", "info", "description") ?? $"导入的自定义皮肤【{fallbackName}】";
        var fontFamily = GetValue(display, "font_ch", "font_family") ?? "Microsoft YaHei UI";
        var fontSize = ParseBoundedInt(GetValue(display, "font_size"), 18, 10, 48);
        var pinyinColor = NormalizeColor(GetValue(display, "pinyin_color"), "0x1F2937", isSogouLayout);
        var candidateColor = NormalizeColor(GetValue(display, "zhongwen_color", "candidate_color"), "0x111827", isSogouLayout);
        var selectedColor = NormalizeColor(GetValue(display, "zhongwen_first_color", "highlight_color"),
            candidateColor, isSogouLayout);

        var builder = new StringBuilder();
        builder.AppendLine("[General]");
        builder.AppendLine("format=caishen-skin-v1");
        builder.AppendLine($"name={CleanIniValue(name)}");
        builder.AppendLine($"author={CleanIniValue(author)}");
        builder.AppendLine($"info={CleanIniValue(info)}");
        builder.AppendLine("version=1.0");
        builder.AppendLine();
        builder.AppendLine("[Display]");
        builder.AppendLine($"font_family={CleanIniValue(fontFamily)}");
        builder.AppendLine($"font_size={fontSize}");
        builder.AppendLine($"pinyin_color={pinyinColor}");
        builder.AppendLine($"candidate_color={candidateColor}");
        builder.AppendLine($"highlight_color={selectedColor}");
        builder.AppendLine("highlight_bg_color=0x2F6BFF");
        builder.AppendLine($"index_color={candidateColor}");
        builder.AppendLine("status_text_color=0x8A94A3");
        builder.AppendLine("separator_color=0xE6E9EF");
        builder.AppendLine();
        builder.AppendLine("[Scheme_H1]");
        builder.AppendLine($"bg_image={backgroundRelative.Replace('/', '\\')}");
        builder.AppendLine($"layout_horizontal={horizontal}");
        builder.AppendLine($"layout_vertical={vertical}");
        builder.AppendLine($"pinyin_margin={pinyinMargin}");
        builder.AppendLine($"candidate_margin={candidateMargin}");
        builder.AppendLine($"native_appearance={(isSogouLayout ? 1 : 0)}");
        builder.AppendLine($"show_separator={(isSogouLayout && !HasValue(scheme, "separator") ? 0 : 1)}");
        builder.AppendLine($"has_shadow={(isSogouLayout ? 0 : 1)}");
        builder.AppendLine($"corner_radius={(isSogouLayout ? 0 : 8)}");

        if (animation.Count > 1)
        {
            builder.AppendLine();
            builder.AppendLine("[Animation]");
            builder.AppendLine($"frame_count={animation.Count}");
            for (var index = 0; index < animation.Count; ++index)
            {
                builder.AppendLine($"frame_{index}=frames\\frame_{index:D3}.png");
                builder.AppendLine($"delay_{index}={animation[index].DelayMilliseconds}");
            }
        }

        File.WriteAllText(originalIniPath, builder.ToString(), new UTF8Encoding(false));
    }

    private static string DecodeText(byte[] bytes)
    {
        if (bytes.Length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
            return Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2);
        if (bytes.Length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
            return Encoding.BigEndianUnicode.GetString(bytes, 2, bytes.Length - 2);
        try { return new UTF8Encoding(false, true).GetString(bytes); }
        catch (DecoderFallbackException)
        {
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            return Encoding.GetEncoding("GB18030").GetString(bytes);
        }
    }

    private static bool IsNormalizedConfiguration(string iniPath)
    {
        var document = IniDocument.Parse(DecodeText(File.ReadAllBytes(iniPath)));
        var general = document.Section("General");
        var scheme = document.Section("Scheme_H1");
        return string.Equals(GetValue(general, "format"), "caishen-skin-v1",
                   StringComparison.OrdinalIgnoreCase) ||
               (scheme != null && scheme.ContainsKey("native_appearance") &&
                scheme.ContainsKey("bg_image") && !scheme.ContainsKey("pic"));
    }

    private static string? ResolveAsset(string root, string? configuredName)
    {
        if (string.IsNullOrWhiteSpace(configuredName)) return null;
        var relative = configuredName.Trim().Trim('"').Replace('/', Path.DirectorySeparatorChar);
        var candidate = Path.GetFullPath(Path.Combine(root, relative));
        if (!candidate.StartsWith(EnsureTrailingSeparator(Path.GetFullPath(root)), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("皮肤配置引用了越界素材路径。");
        if (File.Exists(candidate)) return candidate;
        var directory = Path.GetDirectoryName(candidate);
        if (directory == null || !Directory.Exists(directory)) return null;
        var name = Path.GetFileName(candidate);
        return Directory.EnumerateFiles(directory)
            .FirstOrDefault(path => string.Equals(Path.GetFileName(path), name, StringComparison.OrdinalIgnoreCase));
    }

    private static string? FindFallbackBackground(string root)
    {
        var preferred = new[] { "skin1.png", "horizontal_cand.png", "cand_bg.png", "cd_bg.png", "cand.png", "background.png", "skin_bg.png" };
        foreach (var name in preferred)
        {
            var found = Directory.EnumerateFiles(root, "*.png", SearchOption.TopDirectoryOnly)
                .FirstOrDefault(path => string.Equals(Path.GetFileName(path), name, StringComparison.OrdinalIgnoreCase));
            if (found != null) return found;
        }
        return Directory.EnumerateFiles(root, "*.png", SearchOption.TopDirectoryOnly)
            .Where(path => !string.Equals(Path.GetFileName(path), "bar.png", StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(path => new FileInfo(path).Length)
            .FirstOrDefault();
    }

    private static void ReplaceDirectory(string stagedDirectory, string outputDirectory)
    {
        var backup = outputDirectory + ".backup-" + Guid.NewGuid().ToString("N");
        var hadExisting = Directory.Exists(outputDirectory);
        try
        {
            if (hadExisting) Directory.Move(outputDirectory, backup);
            Directory.Move(stagedDirectory, outputDirectory);
            if (hadExisting) TryDeleteDirectory(backup);
        }
        catch
        {
            if (!Directory.Exists(outputDirectory) && Directory.Exists(backup))
                Directory.Move(backup, outputDirectory);
            throw;
        }
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, directory)));
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            var target = Path.Combine(destination, Path.GetRelativePath(source, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target, overwrite: false);
        }
    }

    private static void CopyWithLimit(Stream input, Stream output, long expectedOrMaximum)
    {
        var maximum = Math.Min(MaxExtractedBytes, Math.Max(0, expectedOrMaximum));
        var buffer = new byte[81920];
        long total = 0;
        while (true)
        {
            var read = input.Read(buffer, 0, buffer.Length);
            if (read == 0) break;
            total = checked(total + read);
            if (total > maximum) throw new InvalidDataException("皮肤素材超过安全限制。");
            output.Write(buffer, 0, read);
        }
    }

    private static uint ReadUInt32LittleEndian(byte[] data, int offset)
    {
        EnsureRange(data, offset, sizeof(uint));
        return BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset, sizeof(uint)));
    }

    private static void EnsureRange(byte[] data, int offset, int length)
    {
        if (offset < 0 || length < 0 || offset > data.Length - length)
            throw new InvalidDataException("皮肤包包含损坏的长度或偏移。");
    }

    private static string EnsureTrailingSeparator(string path) =>
        path.EndsWith(Path.DirectorySeparatorChar) ? path : path + Path.DirectorySeparatorChar;

    private static void TryDeleteDirectory(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static string? GetValue(Dictionary<string, string>? section, params string[] names)
    {
        if (section == null) return null;
        foreach (var name in names)
            if (section.TryGetValue(name, out var value) && !string.IsNullOrWhiteSpace(value)) return value.Trim();
        return null;
    }

    private static bool HasValue(Dictionary<string, string>? section, string name) =>
        section != null && section.TryGetValue(name, out var value) && !string.IsNullOrWhiteSpace(value);

    private static string? FirstValue(string? value) => value?.Split(',')[0].Trim();

    private static string NormalizeIntList(string? value, string fallback, int count)
    {
        var parts = value?.Split(',').Select(part => int.TryParse(part.Trim(), out var parsed) ? parsed : -1).ToArray();
        return parts is { Length: var length } && length >= count && parts.Take(count).All(number => number >= 0)
            ? string.Join(',', parts.Take(count))
            : fallback;
    }

    private static int ParseBoundedInt(string? value, int fallback, int minimum, int maximum) =>
        int.TryParse(value, out var parsed) ? Math.Clamp(parsed, minimum, maximum) : fallback;

    private static string NormalizeColor(string? value, string fallback, bool sogouBgr)
    {
        if (string.IsNullOrWhiteSpace(value)) return fallback;
        var text = value.Trim();
        try
        {
            var number = text.StartsWith("#", StringComparison.Ordinal) ?
                Convert.ToUInt32(text[1..], 16) :
                text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ?
                    Convert.ToUInt32(text[2..], 16) : Convert.ToUInt32(text, 10);
            number &= 0xFFFFFF;
            if (sogouBgr)
                number = ((number & 0xFF) << 16) | (number & 0x00FF00) | ((number >> 16) & 0xFF);
            return $"0x{number:X6}";
        }
        catch (FormatException) { return fallback; }
        catch (OverflowException) { return fallback; }
    }

    private static string CleanIniValue(string value) =>
        value.Replace("\r", " ").Replace("\n", " ").Trim();

    private sealed class IniDocument
    {
        private readonly Dictionary<string, Dictionary<string, string>> sections_ =
            new(StringComparer.OrdinalIgnoreCase);

        public Dictionary<string, string>? Section(string name) =>
            sections_.TryGetValue(name, out var section) ? section : null;

        public static IniDocument Parse(string text)
        {
            var document = new IniDocument();
            Dictionary<string, string>? current = null;
            foreach (var rawLine in text.Replace("\0", string.Empty).Split('\n'))
            {
                var line = rawLine.Trim();
                if (line.Length == 0 || line[0] is ';' or '#') continue;
                if (line.StartsWith('[') && line.EndsWith(']'))
                {
                    var name = line[1..^1].Trim();
                    if (!document.sections_.TryGetValue(name, out current))
                    {
                        current = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                        document.sections_[name] = current;
                    }
                    continue;
                }
                var separator = line.IndexOf('=');
                if (separator <= 0 || current == null) continue;
                current[line[..separator].Trim()] = line[(separator + 1)..].Trim();
            }
            return document;
        }
    }

    public sealed record AnimationFrameInfo(int DelayMilliseconds);

    private static class ApngDecoder
    {
        private static readonly byte[] PngSignature = { 137, 80, 78, 71, 13, 10, 26, 10 };
        private const int MaxDimension = 8192;
        private const int MaxFrames = 240;
        private const long MaxDecodedBytes = 512L * 1024 * 1024;

        public static IReadOnlyList<AnimationFrameInfo> Decode(string pngPath, string framesDirectory)
        {
            var bytes = File.ReadAllBytes(pngPath);
            if (!bytes.AsSpan().StartsWith(PngSignature)) return new[] { new AnimationFrameInfo(100) };
            var chunks = ReadChunks(bytes);
            var header = chunks.FirstOrDefault(chunk => chunk.Type == "IHDR") ??
                throw new InvalidDataException("PNG 缺少 IHDR 数据块。");
            var canvasWidth = ReadBigEndianInt(header.Data, 0);
            var canvasHeight = ReadBigEndianInt(header.Data, 4);
            ValidateDimensions(canvasWidth, canvasHeight);
            if (!chunks.Any(chunk => chunk.Type == "acTL"))
                return new[] { new AnimationFrameInfo(100) };

            var sharedChunks = chunks.TakeWhile(chunk => chunk.Type is not "IDAT" and not "fdAT")
                .Where(chunk => chunk.Type is not "IHDR" and not "acTL" and not "fcTL")
                .ToList();
            var frames = CollectFrames(chunks);
            if (frames.Count is 0 or > MaxFrames)
                throw new InvalidDataException("APNG 动画帧数量无效或超过安全限制。");
            var decodedBytes = checked((long)canvasWidth * canvasHeight * 4 * frames.Count);
            if (decodedBytes > MaxDecodedBytes)
                throw new InvalidDataException("APNG 解码后超过 512 MB 安全限制。");

            Directory.CreateDirectory(framesDirectory);
            var canvas = new byte[checked(canvasWidth * canvasHeight * 4)];
            var result = new List<AnimationFrameInfo>(frames.Count);
            for (var index = 0; index < frames.Count; ++index)
            {
                var frame = frames[index];
                ValidateFrame(frame.Control, canvasWidth, canvasHeight);
                var framePng = BuildFramePng(header.Data, sharedChunks, frame);
                var sourcePixels = DecodePixels(framePng, frame.Control.Width, frame.Control.Height);
                var previous = frame.Control.DisposeOperation == 2 ? canvas.ToArray() : null;
                Composite(canvas, canvasWidth, sourcePixels, frame.Control);
                WritePng(Path.Combine(framesDirectory, $"frame_{index:D3}.png"), canvas, canvasWidth, canvasHeight);
                result.Add(new AnimationFrameInfo(frame.Control.DelayMilliseconds));
                if (frame.Control.DisposeOperation == 1) ClearFrameArea(canvas, canvasWidth, frame.Control);
                else if (frame.Control.DisposeOperation == 2 && previous != null) canvas = previous;
            }
            return result;
        }

        private static List<PngChunk> ReadChunks(byte[] bytes)
        {
            var chunks = new List<PngChunk>();
            var offset = PngSignature.Length;
            while (offset < bytes.Length)
            {
                if (offset > bytes.Length - 12) throw new InvalidDataException("PNG 数据块被截断。");
                var length = ReadBigEndianInt(bytes, offset);
                if (length < 0 || length > bytes.Length - offset - 12)
                    throw new InvalidDataException("PNG 数据块长度无效。");
                var type = Encoding.ASCII.GetString(bytes, offset + 4, 4);
                var data = bytes.AsSpan(offset + 8, length).ToArray();
                chunks.Add(new PngChunk(type, data));
                offset = checked(offset + length + 12);
                if (type == "IEND") break;
            }
            return chunks;
        }

        private static List<RawFrame> CollectFrames(IEnumerable<PngChunk> chunks)
        {
            var frames = new List<RawFrame>();
            RawFrame? current = null;
            foreach (var chunk in chunks)
            {
                if (chunk.Type == "fcTL")
                {
                    if (current != null && current.ImageData.Count > 0) frames.Add(current);
                    current = new RawFrame(ParseControl(chunk.Data));
                }
                else if (chunk.Type == "IDAT" && current != null)
                {
                    current.ImageData.Add(chunk.Data);
                }
                else if (chunk.Type == "fdAT" && current != null)
                {
                    if (chunk.Data.Length < 4) throw new InvalidDataException("APNG fdAT 数据块损坏。");
                    current.ImageData.Add(chunk.Data.AsSpan(4).ToArray());
                }
            }
            if (current != null && current.ImageData.Count > 0) frames.Add(current);
            return frames;
        }

        private static FrameControl ParseControl(byte[] data)
        {
            if (data.Length != 26) throw new InvalidDataException("APNG fcTL 数据块损坏。");
            var denominator = BinaryPrimitives.ReadUInt16BigEndian(data.AsSpan(22, 2));
            if (denominator == 0) denominator = 100;
            var numerator = BinaryPrimitives.ReadUInt16BigEndian(data.AsSpan(20, 2));
            var milliseconds = (int)Math.Round(numerator * 1000.0 / denominator);
            return new FrameControl(
                ReadBigEndianInt(data, 4), ReadBigEndianInt(data, 8),
                ReadBigEndianInt(data, 12), ReadBigEndianInt(data, 16),
                Math.Clamp(milliseconds == 0 ? 10 : milliseconds, 10, 10_000),
                data[24], data[25]);
        }

        private static byte[] BuildFramePng(byte[] canvasHeader, IReadOnlyList<PngChunk> shared, RawFrame frame)
        {
            var frameHeader = canvasHeader.ToArray();
            BinaryPrimitives.WriteInt32BigEndian(frameHeader.AsSpan(0, 4), frame.Control.Width);
            BinaryPrimitives.WriteInt32BigEndian(frameHeader.AsSpan(4, 4), frame.Control.Height);
            using var stream = new MemoryStream();
            stream.Write(PngSignature);
            WriteChunk(stream, "IHDR", frameHeader);
            foreach (var chunk in shared) WriteChunk(stream, chunk.Type, chunk.Data);
            foreach (var imageData in frame.ImageData) WriteChunk(stream, "IDAT", imageData);
            WriteChunk(stream, "IEND", Array.Empty<byte>());
            return stream.ToArray();
        }

        private static byte[] DecodePixels(byte[] png, int width, int height)
        {
            using var stream = new MemoryStream(png, writable: false);
            var decoder = new PngBitmapDecoder(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            BitmapSource source = decoder.Frames[0];
            if (source.Format != PixelFormats.Bgra32)
                source = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
            var pixels = new byte[checked(width * height * 4)];
            source.CopyPixels(pixels, width * 4, 0);
            return pixels;
        }

        private static void Composite(byte[] canvas, int canvasWidth, byte[] source, FrameControl control)
        {
            for (var y = 0; y < control.Height; ++y)
            for (var x = 0; x < control.Width; ++x)
            {
                var sourceIndex = (y * control.Width + x) * 4;
                var destinationIndex = ((control.Y + y) * canvasWidth + control.X + x) * 4;
                if (control.BlendOperation == 0)
                {
                    Buffer.BlockCopy(source, sourceIndex, canvas, destinationIndex, 4);
                    continue;
                }
                var sourceAlpha = source[sourceIndex + 3];
                var destinationAlpha = canvas[destinationIndex + 3];
                var outputAlpha = sourceAlpha + (destinationAlpha * (255 - sourceAlpha) + 127) / 255;
                for (var channel = 0; channel < 3; ++channel)
                {
                    var numerator = source[sourceIndex + channel] * sourceAlpha * 255 +
                        canvas[destinationIndex + channel] * destinationAlpha * (255 - sourceAlpha);
                    canvas[destinationIndex + channel] = outputAlpha == 0 ? (byte)0 :
                        (byte)Math.Clamp((numerator + outputAlpha * 127) / (outputAlpha * 255), 0, 255);
                }
                canvas[destinationIndex + 3] = (byte)outputAlpha;
            }
        }

        private static void ClearFrameArea(byte[] canvas, int canvasWidth, FrameControl control)
        {
            for (var y = 0; y < control.Height; ++y)
                Array.Clear(canvas, ((control.Y + y) * canvasWidth + control.X) * 4, control.Width * 4);
        }

        private static void WritePng(string path, byte[] pixels, int width, int height)
        {
            var bitmap = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgra32,
                palette: null, pixels, width * 4);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
            encoder.Save(stream);
        }

        private static void WriteChunk(Stream stream, string type, byte[] data)
        {
            Span<byte> length = stackalloc byte[4];
            BinaryPrimitives.WriteInt32BigEndian(length, data.Length);
            stream.Write(length);
            var typeBytes = Encoding.ASCII.GetBytes(type);
            stream.Write(typeBytes);
            stream.Write(data);
            var crcInput = new byte[typeBytes.Length + data.Length];
            Buffer.BlockCopy(typeBytes, 0, crcInput, 0, typeBytes.Length);
            Buffer.BlockCopy(data, 0, crcInput, typeBytes.Length, data.Length);
            Span<byte> crc = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(crc, Crc32(crcInput));
            stream.Write(crc);
        }

        private static uint Crc32(byte[] data)
        {
            uint crc = 0xFFFFFFFF;
            foreach (var value in data)
            {
                crc ^= value;
                for (var bit = 0; bit < 8; ++bit)
                    crc = (crc & 1) != 0 ? 0xEDB88320U ^ (crc >> 1) : crc >> 1;
            }
            return ~crc;
        }

        private static int ReadBigEndianInt(byte[] data, int offset)
        {
            if (offset < 0 || offset > data.Length - 4) throw new InvalidDataException("PNG 整数被截断。");
            return BinaryPrimitives.ReadInt32BigEndian(data.AsSpan(offset, 4));
        }

        private static void ValidateDimensions(int width, int height)
        {
            if (width <= 0 || height <= 0 || width > MaxDimension || height > MaxDimension ||
                (long)width * height * 4 > MaxDecodedBytes)
                throw new InvalidDataException("PNG 图片尺寸无效或超过安全限制。");
        }

        private static void ValidateFrame(FrameControl frame, int canvasWidth, int canvasHeight)
        {
            ValidateDimensions(frame.Width, frame.Height);
            if (frame.X < 0 || frame.Y < 0 || frame.X > canvasWidth - frame.Width ||
                frame.Y > canvasHeight - frame.Height || frame.DisposeOperation > 2 || frame.BlendOperation > 1)
                throw new InvalidDataException("APNG 动画帧位置或操作类型无效。");
        }

        private sealed record PngChunk(string Type, byte[] Data);
        private sealed record FrameControl(int Width, int Height, int X, int Y,
            int DelayMilliseconds, byte DisposeOperation, byte BlendOperation);
        private sealed class RawFrame(FrameControl control)
        {
            public FrameControl Control { get; } = control;
            public List<byte[]> ImageData { get; } = new();
        }
    }
}
