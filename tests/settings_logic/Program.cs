using ShuruSettings;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

if (args.Contains("--corrupt-migration"))
    return RunCorruptMigrationTest();

return RunMainTests();

static int RunMainTests()
{
    var root = Path.Combine(
        Path.GetTempPath(), "caishen-settings-test-" + Guid.NewGuid().ToString("N"));
    var clipboardDirectory = Path.Combine(root, "clipboard");
    Directory.CreateDirectory(clipboardDirectory);
    Environment.SetEnvironmentVariable(
        "CAISHEN_CLIPBOARD_DATA_DIR", clipboardDirectory);

    try
    {
        TestSettingsAndCustomPhrases(root);
        PrepareLegacyClipboardHistory(clipboardDirectory);
        TestClipboardMigrationAndCrud(clipboardDirectory);
        TestClipboardConcurrencyAndPerformance();
        TestClipboardImageNormalizationAndCapture();
        TestTextPasteRequests(root);
        TestCorruptMigrationInChildProcess(root);
        Console.WriteLine("settings_logic: OK");
        return 0;
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine(ex);
        return 1;
    }
    finally
    {
        Environment.SetEnvironmentVariable("CAISHEN_CLIPBOARD_DATA_DIR", null);
        try { Directory.Delete(root, recursive: true); }
        catch (Exception ex) { Console.Error.WriteLine("cleanup: " + ex.Message); }
    }
}

static void TestSettingsAndCustomPhrases(string root)
{
    var settingsPath = Path.Combine(root, "settings.ini");
    var defaults = SettingsStore.Load(settingsPath);
    Require(defaults.LearningEnabled && !defaults.ContentLogging &&
            defaults.FuzzyEnabled && defaults.CandidateCount == 9,
        "设置默认值错误");

    File.WriteAllText(settingsPath,
        "CandidateCount=99\nCandidateFontSize=no\nContentLogging=maybe\nFuzzyEnabled=0\n");
    var fallback = SettingsStore.Load(settingsPath);
    Require(fallback.CandidateCount == 9 && fallback.CandidateFontSize == 19 &&
            !fallback.ContentLogging && !fallback.FuzzyEnabled,
        "非法设置回退错误");

    var expected = new AppSettings(
        true, false, true, true, false, true, false, true, false,
        5, 24, "加油拼音");
    SettingsStore.Save(expected, settingsPath);
    Require(SettingsStore.Load(settingsPath) == expected,
        "设置保存后不一致");
    Require(Directory.GetFiles(root, "*.tmp-*").Length == 0,
        "设置原子写入遗留临时文件");
    Require(SettingsStore.NormalizeDisplayName("  加油  ") == "加油" &&
            SettingsStore.NormalizeDisplayName("\r\n") is null &&
            SettingsStore.NormalizeDisplayName(new string('a', 25)) is null,
        "输入法名称规范化错误");

    var phrasePath = Path.Combine(root, "custom_phrases.txt");
    var phrases = new[]
    {
        new CustomPhrase("SDS", "深度思考", 1),
        new CustomPhrase("sds", "认真思考", 2),
        new CustomPhrase("ss", "延长思考", 9)
    };
    CustomPhraseStore.Save(phrases, phrasePath);
    var loaded = CustomPhraseStore.Load(phrasePath);
    Require(loaded.Count == 3 &&
            loaded[0] == phrases[0] with { Code = "sds" } &&
            loaded[2].Position == 9,
        "自定义短语保存或规范化错误");

    File.AppendAllText(phrasePath, "bad-code!\t忽略\t1\n");
    Require(CustomPhraseStore.Load(phrasePath).Count == 3,
        "短语加载未忽略非法行");
    ExpectException<InvalidDataException>(() =>
        CustomPhraseStore.Import(phrasePath));
    ExpectException<InvalidDataException>(() =>
        CustomPhraseStore.Save(new[]
        {
            new CustomPhrase("dup", "重复", 1),
            new CustomPhrase("DUP", "重复", 2)
        }, phrasePath));
}

static void PrepareLegacyClipboardHistory(string clipboardDirectory)
{
    using var extensionDocument = JsonDocument.Parse("true");
    var legacy = new List<ClipboardRecord>
    {
        new()
        {
            Id = "legacy-first",
            Type = ClipboardItemType.Text,
            Content = "第一行\r\n第二行",
            DisplayTitle = "多行文本",
            CreatedTime = new DateTime(
                2026, 8, 15, 12, 0, 0, DateTimeKind.Local),
            AdditionalData = new Dictionary<string, JsonElement>
            {
                ["future_flag"] = extensionDocument.RootElement.Clone()
            }
        }
    };
    File.WriteAllText(
        Path.Combine(clipboardDirectory, "history.json"),
        JsonSerializer.Serialize(legacy));
}

static void TestClipboardMigrationAndCrud(string clipboardDirectory)
{
    ClipboardStore.SaveConfig(new ClipboardConfig
    {
        Enabled = true,
        MaxRecords = 20_000
    });

    var history = ClipboardStore.LoadHistory();
    Require(history.Count == 1 && history[0].Id == "legacy-first" &&
            history[0].Content == "第一行\r\n第二行" &&
            history[0].AdditionalData?.ContainsKey("future_flag") == true,
        "旧 JSON 迁移不完整");
    Require(File.Exists(Path.Combine(clipboardDirectory, "history.db")),
        "SQLite 数据库未创建");
    Require(!File.Exists(Path.Combine(clipboardDirectory, "history.json")) &&
            File.Exists(Path.Combine(
                clipboardDirectory, "history.json.migrated.bak")),
        "迁移后的 JSON 未按约定保留备份");

    ClipboardStore.AddRecord(new ClipboardRecord
    {
        Id = "second",
        Content = "待修改",
        DisplayTitle = "待修改",
        CreatedTime = DateTime.Now
    });
    ClipboardStore.UpdateRecordContent("second", "修改后");
    Require(ClipboardStore.FindRecord("second")?.Content == "修改后",
        "SQLite 记录修改失败");
    ClipboardStore.DeleteRecord("second");
    Require(ClipboardStore.FindRecord("second") is null,
        "SQLite 记录删除失败");

    history[0].Content = "不得污染数据库";
    Require(ClipboardStore.FindRecord("legacy-first")?.Content !=
            "不得污染数据库",
        "查询结果修改污染了持久化数据");

    ClipboardStore.AddRecord(new ClipboardRecord
    {
        Id = "batch-a",
        Content = "批量删除甲",
        DisplayTitle = "批量删除甲"
    });
    ClipboardStore.AddRecord(new ClipboardRecord
    {
        Id = "batch-b",
        Content = "批量删除乙",
        DisplayTitle = "批量删除乙"
    });
    Require(ClipboardStore.DeleteRecords(
                new[] { "batch-a", "batch-b", "batch-a" }) == 2 &&
            ClipboardStore.FindRecord("batch-a") is null &&
            ClipboardStore.FindRecord("batch-b") is null,
        "SQLite 批量删除失败");
    Require(new ClipboardRecord { Type = ClipboardItemType.Text }.TypeDisplayName == "文本" &&
            new ClipboardRecord { Type = ClipboardItemType.Image }.TypeDisplayName == "图片" &&
            new ClipboardRecord { Type = ClipboardItemType.File }.TypeDisplayName == "文件",
        "剪贴板类型中文名称错误");
    Require(new ClipboardRecord { Type = ClipboardItemType.Text }.OpenOrEditLabel == "编辑" &&
            new ClipboardRecord { Type = ClipboardItemType.Image }.OpenOrEditLabel == "打开" &&
            new ClipboardRecord { Type = ClipboardItemType.File }.OpenOrEditLabel == "打开",
        "剪贴板打开或编辑按钮名称错误");
}

static void TestClipboardConcurrencyAndPerformance()
{
    var records = Enumerable.Range(0, 10_000)
        .Select(index => new ClipboardRecord
        {
            Id = $"bulk-{index}",
            Content = $"性能目标{index}",
            DisplayTitle = $"性能目标{index}",
            CreatedTime = DateTime.Now.AddMilliseconds(index)
        })
        .ToList();

    var writeWatch = Stopwatch.StartNew();
    ClipboardStore.SaveHistory(records);
    writeWatch.Stop();
    Require(ClipboardStore.CountHistory() == 10_000,
        "一万条记录批量写入不完整");
    Require(writeWatch.Elapsed < TimeSpan.FromSeconds(15),
        $"一万条记录事务写入过慢：{writeWatch.Elapsed}");

    var searchWatch = Stopwatch.StartNew();
    var found = ClipboardStore.QueryHistory("目标9876", 20, 0);
    searchWatch.Stop();
    Require(found.Count == 1 && found[0].Id == "bulk-9876",
        "SQLite FTS 搜索结果错误");
    Require(searchWatch.Elapsed < TimeSpan.FromSeconds(2),
        $"一万条记录搜索过慢：{searchWatch.Elapsed}");

    Parallel.For(0, 24, index => ClipboardStore.AddRecord(new ClipboardRecord
    {
        Id = $"parallel-{index}",
        Content = $"并发记录-{index}",
        DisplayTitle = $"并发记录-{index}",
        CreatedTime = DateTime.Now.AddSeconds(index)
    }));
    Require(ClipboardStore.CountHistory() == 10_024,
        "并发写入丢失记录");
    Require(ClipboardStore.QueryHistory("并发记录", 100, 0)
            .Select(record => record.Id).Distinct().Count() == 24,
        "并发写入产生重复或缺失记录");
}

static void TestClipboardImageNormalizationAndCapture()
{
    var transparentAlphaPng = EncodePng(new byte[]
    {
        12, 34, 56, 0,
        78, 90, 123, 0
    }, width: 2, height: 1);
    var repaired = ClipboardImageService.NormalizePngAlpha(
        transparentAlphaPng);
    Require(repaired.AlphaRepaired,
        "全零 Alpha 图片没有被识别");
    var repairedPixels = DecodeBgra(repaired.PngBytes);
    Require(repairedPixels[3] == 255 && repairedPixels[7] == 255,
        "全零 Alpha 图片没有恢复为不透明");

    var partialPng = EncodePng(new byte[]
    {
        12, 34, 56, 128,
        78, 90, 123, 0
    }, width: 2, height: 1);
    var partial = ClipboardImageService.NormalizePngAlpha(partialPng);
    Require(!partial.AlphaRepaired &&
            partial.PngBytes.SequenceEqual(partialPng),
        "正常半透明 PNG 被错误改写");

    var beforeTextCapture = ClipboardStore.CountHistory();
    var textData = new DataObject();
    textData.SetData(DataFormats.UnicodeText, "监听测试内容");
    Require(ClipboardMonitor.ProcessDataObject(textData),
        "文本剪贴板数据未被监听器接收");
    Require(ClipboardStore.CountHistory() == beforeTextCapture + 1 &&
            ClipboardStore.QueryHistory("监听测试", 10, 0).Count == 1,
        "文本剪贴板数据未写入 SQLite");

    var internalData = new DataObject();
    internalData.SetData(ClipboardImageService.InternalPasteFormat, "1");
    internalData.SetData(DataFormats.UnicodeText, "不得重复记录");
    Require(!ClipboardMonitor.ProcessDataObject(internalData) &&
            ClipboardStore.CountHistory() == beforeTextCapture + 1,
        "输入法自身粘贴被重复记录");

    var imageData = new DataObject();
    imageData.SetData(
        ClipboardImageService.NativePngFormat,
        new MemoryStream(transparentAlphaPng, writable: false));
    Require(ClipboardMonitor.ProcessDataObject(imageData),
        "PNG 剪贴板数据未被监听器接收");
    var imageRecord = ClipboardStore.QueryHistory("[图片]", 10, 0)
        .FirstOrDefault(record => record.IsImage);
    Require(imageRecord != null && File.Exists(imageRecord.ImagePath),
        "PNG 剪贴板记录或图片文件缺失");
    var storedPixels = DecodeBgra(File.ReadAllBytes(imageRecord!.ImagePath));
    Require(storedPixels[3] == 255 && storedPixels[7] == 255,
        "监听器保存的 PNG 仍然全透明");
}

static void TestTextPasteRequests(string root)
{
    var requestDirectory = Path.Combine(root, "paste-requests");
    Environment.SetEnvironmentVariable(
        "CAISHEN_PASTE_REQUEST_DIR", requestDirectory);
    try
    {
        Directory.CreateDirectory(requestDirectory);
        var token = new string('a', 32);
        var path = Path.Combine(requestDirectory, token + ".txt");
        var expected = "中文第一行\r\n第二行" + new string('长', 180_000);
        File.WriteAllText(
            path,
            TextPasteRequestStore.Header + expected,
            new System.Text.UTF8Encoding(false));
        Require(TextPasteRequestStore.ReadAndDelete(token) == expected,
            "大文本请求内容读取不完整");
        Require(!File.Exists(path), "大文本请求使用后未删除");
        ExpectException<InvalidDataException>(() =>
            TextPasteRequestStore.ReadAndDelete("../invalid"));
    }
    finally
    {
        Environment.SetEnvironmentVariable(
            "CAISHEN_PASTE_REQUEST_DIR", null);
    }
}

static void TestCorruptMigrationInChildProcess(string root)
{
    var childDirectory = Path.Combine(root, "corrupt-clipboard");
    Directory.CreateDirectory(childDirectory);
    var assembly = Assembly.GetExecutingAssembly().Location;
    var startInfo = new ProcessStartInfo
    {
        FileName = "dotnet",
        Arguments = $"\"{assembly}\" --corrupt-migration",
        UseShellExecute = false,
        CreateNoWindow = true
    };
    startInfo.Environment["CAISHEN_CLIPBOARD_DATA_DIR"] = childDirectory;
    using var process = Process.Start(startInfo) ??
        throw new InvalidOperationException("无法启动损坏迁移子测试");
    Require(process.WaitForExit(20_000), "损坏迁移子测试超时");
    Require(process.ExitCode == 0,
        $"损坏迁移子测试失败：{process.ExitCode}");
}

static int RunCorruptMigrationTest()
{
    var directory = Environment.GetEnvironmentVariable(
        "CAISHEN_CLIPBOARD_DATA_DIR");
    if (string.IsNullOrWhiteSpace(directory)) return 20;
    Directory.CreateDirectory(directory);
    var historyPath = Path.Combine(directory, "history.json");
    File.WriteAllText(historyPath, "{");

    if (ClipboardStore.LoadHistory().Count != 0) return 21;
    ClipboardStore.AddRecord(new ClipboardRecord
    {
        Id = "after-corruption",
        Content = "数据库仍可使用",
        DisplayTitle = "数据库仍可使用"
    });
    if (ClipboardStore.FindRecord("after-corruption") is null) return 22;
    if (File.ReadAllText(historyPath) != "{") return 23;
    if (File.Exists(historyPath + ".migrated.bak")) return 24;
    return 0;
}

static byte[] EncodePng(byte[] bgraPixels, int width, int height)
{
    var bitmap = BitmapSource.Create(
        width, height, 96, 96, PixelFormats.Bgra32,
        palette: null, bgraPixels, width * 4);
    var encoder = new PngBitmapEncoder();
    encoder.Frames.Add(BitmapFrame.Create(bitmap));
    using var stream = new MemoryStream();
    encoder.Save(stream);
    return stream.ToArray();
}

static byte[] DecodeBgra(byte[] png)
{
    using var stream = new MemoryStream(png, writable: false);
    var decoder = BitmapDecoder.Create(
        stream, BitmapCreateOptions.PreservePixelFormat,
        BitmapCacheOption.OnLoad);
    var frame = decoder.Frames[0];
    BitmapSource bgra = frame.Format == PixelFormats.Bgra32
        ? frame
        : new FormatConvertedBitmap(
            frame, PixelFormats.Bgra32, null, 0);
    var pixels = new byte[bgra.PixelWidth * bgra.PixelHeight * 4];
    bgra.CopyPixels(pixels, bgra.PixelWidth * 4, 0);
    return pixels;
}

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static void ExpectException<TException>(Action action)
    where TException : Exception
{
    try
    {
        action();
    }
    catch (TException)
    {
        return;
    }
    throw new InvalidOperationException(
        $"期望抛出 {typeof(TException).Name}");
}
