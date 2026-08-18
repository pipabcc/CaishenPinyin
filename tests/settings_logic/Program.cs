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
using WinForms = System.Windows.Forms;

if (args.Contains("--corrupt-migration"))
    return RunCorruptMigrationTest();
if (args.Length == 2 && args[0] == "--installed-image-paste")
    return RunInstalledImagePasteTestOnStaThread(args[1]);

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
        TestSkinCatalog(root);
        TestSsfConversion(root);
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

static int RunInstalledImagePasteTest(string helperPath)
{
    var helper = Path.GetFullPath(helperPath);
    if (!File.Exists(helper))
    {
        Console.Error.WriteLine($"安装版粘贴助手不存在：{helper}");
        return 1;
    }

    var root = Path.Combine(
        Path.GetTempPath(), "caishen-image-paste-test-" +
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    Environment.SetEnvironmentVariable("CAISHEN_CLIPBOARD_DATA_DIR", root);
    WinForms.Form? window = null;
    try
    {
        var imagePath = Path.Combine(root, "paste-source.png");
        File.WriteAllBytes(imagePath, EncodePng(
        [
            32, 64, 224, 255, 224, 64, 32, 255,
            64, 192, 96, 255, 240, 192, 32, 255
        ], width: 2, height: 2));
        var recordId = "installed-image-" + Guid.NewGuid().ToString("N");
        ClipboardStore.AddRecord(new ClipboardRecord
        {
            Id = recordId,
            Type = ClipboardItemType.Image,
            Content = "[图片]",
            DisplayTitle = "[图片]",
            ImagePath = imagePath,
            CreatedTime = DateTime.Now
        });
        Require(ClipboardStore.FindRecord(recordId)?.IsImage == true,
            "端到端测试图片记录未写入数据库");

        var editor = new PasteTrackingRichTextBox
        {
            Dock = WinForms.DockStyle.Fill,
            DetectUrls = false
        };
        window = new WinForms.Form
        {
            Text = "财神输入法图片粘贴测试",
            Width = 460,
            Height = 280,
            StartPosition = WinForms.FormStartPosition.CenterScreen,
            ShowInTaskbar = false,
            TopMost = true
        };
        window.Controls.Add(editor);
        window.Show();
        window.BringToFront();
        editor.Focus();
        PumpWindowsForms(TimeSpan.FromMilliseconds(150));

        var handle = editor.Handle;
        Require(handle != IntPtr.Zero, "端到端测试编辑窗口句柄无效");
        RunImagePasteHelper(helper, recordId, handle, root);
        PumpWindowsForms(TimeSpan.FromMilliseconds(300));

        var clipboardFormats = System.Windows.Clipboard.GetDataObject()?
            .GetFormats(autoConvert: true) ?? Array.Empty<string>();
        Require(editor.PasteMessageCount > 0 || editor.PasteKeyCount > 0,
            "安装版助手未向目标编辑控件发送粘贴按键；" +
            $"剪贴板格式：{string.Join(',', clipboardFormats)}");
        Require(editor.Rtf?.Contains("\\pict", StringComparison.Ordinal) == true,
            "目标编辑控件收到粘贴命令但未插入图片；" +
            $"WM_PASTE={editor.PasteMessageCount}，Ctrl+V={editor.PasteKeyCount}，" +
            $"剪贴板格式：{string.Join(',', clipboardFormats)}");
        Require(!editor.Text.Contains(imagePath, StringComparison.OrdinalIgnoreCase),
            "图片被错误地作为文件路径插入编辑框");

        window.Controls.Clear();
        editor.Dispose();
        var browserLikeEditor = new PasteTrackingCustomControl
        {
            Dock = WinForms.DockStyle.Fill
        };
        window.Controls.Add(browserLikeEditor);
        browserLikeEditor.Focus();
        PumpWindowsForms(TimeSpan.FromMilliseconds(150));

        RunImagePasteHelper(
            helper, recordId, browserLikeEditor.Handle, root);
        PumpWindowsForms(TimeSpan.FromMilliseconds(300));
        Require(browserLikeEditor.PasteKeyCount > 0,
            "非标准编辑控件未收到 Ctrl+V，SendInput 分支未覆盖");
        Require(browserLikeEditor.LastExtraInfo ==
                ClipboardPasteInputProtocol.Marker,
            $"合成粘贴键缺少 CPIM 标记：0x{browserLikeEditor.LastExtraInfo:X}");
        Require(browserLikeEditor.ReceivedImage,
            "非标准编辑控件收到 Ctrl+V 后未读取到剪贴板图片");
        Console.WriteLine("installed_image_paste: OK");
        return 0;
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine(ex);
        return 1;
    }
    finally
    {
        window?.Close();
        Environment.SetEnvironmentVariable("CAISHEN_CLIPBOARD_DATA_DIR", null);
        try { Directory.Delete(root, recursive: true); }
        catch (Exception ex) { Console.Error.WriteLine("cleanup: " + ex.Message); }
    }
}

static void RunImagePasteHelper(
    string helper,
    string recordId,
    IntPtr targetHandle,
    string dataRoot)
{
    Require(targetHandle != IntPtr.Zero, "图片粘贴测试目标窗口句柄无效");
    var startInfo = new ProcessStartInfo
    {
        FileName = helper,
        Arguments = $"-paste-record {recordId} -target-hwnd {targetHandle.ToInt64()}",
        WorkingDirectory = Path.GetDirectoryName(helper),
        UseShellExecute = false,
        CreateNoWindow = true
    };
    startInfo.Environment["CAISHEN_CLIPBOARD_DATA_DIR"] = dataRoot;
    using var pasteProcess = Process.Start(startInfo) ??
        throw new InvalidOperationException("无法启动安装版图片粘贴助手");
    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
    while (!pasteProcess.HasExited && DateTime.UtcNow < deadline)
        PumpWindowsForms(TimeSpan.FromMilliseconds(20));
    if (!pasteProcess.HasExited)
    {
        pasteProcess.Kill(entireProcessTree: true);
        throw new TimeoutException("安装版图片粘贴助手执行超时");
    }
    Require(pasteProcess.ExitCode == 0,
        $"安装版图片粘贴助手退出码错误：{pasteProcess.ExitCode}");
}

static int RunInstalledImagePasteTestOnStaThread(string helperPath)
{
    var exitCode = 1;
    var thread = new Thread(() =>
    {
        exitCode = RunInstalledImagePasteTest(helperPath);
    });
    thread.SetApartmentState(ApartmentState.STA);
    thread.Start();
    thread.Join();
    return exitCode;
}

static void PumpWindowsForms(TimeSpan duration)
{
    var deadline = DateTime.UtcNow + duration;
    while (DateTime.UtcNow < deadline)
    {
        WinForms.Application.DoEvents();
        Thread.Sleep(5);
    }
}

static void TestSsfConversion(string root)
{
    var repositoryRoot = Path.GetFullPath(Path.Combine(
        AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
    var source = Path.Combine(repositoryRoot, "1.ssf");
    if (!File.Exists(source))
    {
        Console.WriteLine("settings_logic: 跳过 SSF 样例测试（未找到 1.ssf）");
        return;
    }

    var target = Path.Combine(root, "skins", "ssf-sample");
    Require(SsfConverter.ConvertAndInstall(source, target) == "ssf-sample",
        "SSF 转换返回了错误的皮肤标识");
    var ini = File.ReadAllText(Path.Combine(target, "skin.ini"));
    Require(ini.Contains("bg_image=frames\\frame_000.png") &&
            ini.Contains("layout_horizontal=0,95,182") &&
            ini.Contains("layout_vertical=0,121,5") &&
            ini.Contains("pinyin_margin=60,2,35,33") &&
            ini.Contains("candidate_margin=10,8,32,134") &&
            ini.Contains("native_appearance=1") &&
            ini.Contains("frame_count=6") &&
            ini.Contains("delay_0=80"),
        "SSF 配置未按 Scheme_H1 规范化");
    Require(Directory.GetFiles(Path.Combine(target, "frames"), "frame_*.png").Length == 6,
        "SSF APNG 没有完整导出 6 帧");

    using var frame = new FileStream(Path.Combine(target, "frames", "frame_000.png"),
        FileMode.Open, FileAccess.Read, FileShare.Read);
    var decoder = new PngBitmapDecoder(frame, BitmapCreateOptions.PreservePixelFormat,
        BitmapCacheOption.OnLoad);
    Require(decoder.Frames[0].PixelWidth == 285 && decoder.Frames[0].PixelHeight == 131,
        "SSF 候选背景错误地选择了状态栏素材");
    var normalizedBefore = File.ReadAllText(Path.Combine(target, "skin.ini"));
    Require(!SsfConverter.NormalizeInstalledSkin(target, "ssf-sample") &&
            File.ReadAllText(Path.Combine(target, "skin.ini")) == normalizedBefore,
        "已规范化皮肤没有保持幂等");

    var wsscSource = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CaishenPinyin", "skins", "wssc");
    if (Directory.Exists(wsscSource))
    {
        var wsscTarget = Path.Combine(root, "skins", "wssc-test");
        CopyTestDirectory(wsscSource, wsscTarget);
        var framesDir = Path.Combine(wsscTarget, "frames");
        if (Directory.Exists(framesDir)) Directory.Delete(framesDir, true);
        // 模拟未提取动画的旧版 skin.ini
        var sourceIni = Path.Combine(wsscTarget, "source_skin.ini");
        if (File.Exists(sourceIni))
            File.Copy(sourceIni, Path.Combine(wsscTarget, "skin.ini"), true);

        Require(SsfConverter.NormalizeInstalledSkin(wsscTarget, "wssc-test"),
            "挂件型皮肤 wssc 应当成功规范化并补充提取动画");
        var wsscIni = File.ReadAllText(Path.Combine(wsscTarget, "skin.ini"));
        Require(wsscIni.Contains("format_version=2") &&
                wsscIni.Contains("bg_image=skin1.png") &&
                wsscIni.Contains("native_min_width=507") &&
                wsscIni.Contains("native_min_height=175") &&
                wsscIni.Contains("pinyin_margin=110,2,66,12") &&
                wsscIni.Contains("candidate_margin=2,20,66,1") &&
                wsscIni.Contains("[AnimationOverlays]") &&
                wsscIni.Contains("count=1") &&
                wsscIni.Contains("[AnimationOverlay0]") &&
                wsscIni.Contains("frame_count=23") &&
                wsscIni.Contains("horizontal_anchor=end") &&
                wsscIni.Contains("vertical_anchor=end") &&
                wsscIni.Contains("margin_right=15") &&
                wsscIni.Contains("margin_bottom=9") &&
                wsscIni.Contains("frame_0=overlay_frames\\0\\frame_000.png"),
            "挂件型皮肤 wssc 未按原始 SSF 布局输出独立动画挂件");
        var wsscFrames = Path.Combine(wsscTarget, "overlay_frames", "0");
        Require(Directory.GetFiles(wsscFrames, "frame_*.png").Length == 23,
            "挂件型皮肤 wssc 未能完整保存 23 帧独立挂件动画");
        using (var overlayFrame = new FileStream(
            Path.Combine(wsscFrames, "frame_000.png"),
            FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            var overlayDecoder = new PngBitmapDecoder(
                overlayFrame, BitmapCreateOptions.PreservePixelFormat,
                BitmapCacheOption.OnLoad);
            Require(overlayDecoder.Frames[0].PixelWidth == 150 &&
                    overlayDecoder.Frames[0].PixelHeight == 150,
                "wssc 错误地选择了 300x300 的未显示挂件资源");
        }
        Require(DecodeBgra(File.ReadAllBytes(
                    Path.Combine(wsscFrames, "frame_000.png")))
                .SequenceEqual(DecodeBgra(File.ReadAllBytes(
                    Path.Combine(wsscTarget, "oh_custom11.png")))),
            "wssc 输出帧不是 SSF 明确启用的 oh_custom11 挂件");

        var wsscBefore = File.ReadAllText(Path.Combine(wsscTarget, "skin.ini"));
        Require(!SsfConverter.NormalizeInstalledSkin(wsscTarget, "wssc-test") &&
                File.ReadAllText(Path.Combine(wsscTarget, "skin.ini")) == wsscBefore,
            "已规范化 wssc 皮肤未保持幂等");
    }
}

static void CopyTestDirectory(string source, string destination)
{
    Directory.CreateDirectory(destination);
    foreach (var dir in Directory.GetDirectories(source, "*", SearchOption.AllDirectories))
        Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, dir)));
    foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
        File.Copy(file, Path.Combine(destination, Path.GetRelativePath(source, file)), true);
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
    Require(fallback.CandidateCount == 9 &&
            fallback.CandidateFontSizeMode == SettingsStore.FollowSkinFontSizeMode &&
            !fallback.ContentLogging && !fallback.FuzzyEnabled,
        "非法设置回退错误");

    File.WriteAllText(settingsPath,
        "CandidateCount=11\nCandidateFontFamily=DengXian\nCandidateFontSize=24\n");
    var migrated = SettingsStore.Load(settingsPath);
    Require(migrated.CandidateCount == 11 &&
            migrated.CandidateFontFamily == "DengXian" &&
            migrated.CandidateFontSizeMode == "large",
        "旧版候选字号迁移错误");

    var expected = new AppSettings(
        EnglishDefault: true,
        LearningEnabled: false,
        ContentLogging: true,
        FuzzyEnabled: true,
        FuzzyInitials: false,
        FuzzyFinals: true,
        FuzzyMissingVowel: false,
        ShuangpinXiaohe: true,
        FullWidthPunctuation: false,
        CandidateCount: 5,
        CandidateFontFamily: "Microsoft YaHei UI",
        CandidateFontSizeMode: "extra_large",
        DisplayName: "加油拼音");
    SettingsStore.Save(expected, settingsPath);
    Require(SettingsStore.Load(settingsPath) == expected,
        "设置保存后不一致");
    Require(Directory.GetFiles(root, "*.tmp-*").Length == 0,
        "设置原子写入遗留临时文件");
    Require(SettingsStore.NormalizeDisplayName("  加油  ") == "加油" &&
            SettingsStore.NormalizeDisplayName("\r\n") is null &&
            SettingsStore.NormalizeDisplayName(new string('a', 25)) is null,
        "输入法名称规范化错误");
    Require(SettingsStore.CandidateFontSizeModeFromLegacy(16) == "small" &&
            SettingsStore.CandidateFontSizeModeFromLegacy(19) == "standard" &&
            SettingsStore.CandidateFontSizeModeFromLegacy(24) == "large" &&
            SettingsStore.CandidateFontSizeModeFromLegacy(30) == "extra_large" &&
            SettingsStore.LegacyCandidateFontSize("extra_large") == 26 &&
            new AppSettings(CandidateCount: 10).Validated().CandidateCount == 10,
        "候选设置映射错误");

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

    var pasteData = ClipboardImageService.CreateClipboardImageDataObject(
        partialPng);
    Require(pasteData.GetDataPresent(
                ClipboardImageService.InternalPasteFormat, autoConvert: false) &&
            pasteData.GetDataPresent(
                ClipboardImageService.NativePngFormat, autoConvert: false) &&
            pasteData.GetDataPresent(DataFormats.Bitmap, autoConvert: false) &&
            !pasteData.GetDataPresent(DataFormats.FileDrop, autoConvert: false) &&
            !pasteData.GetDataPresent(DataFormats.FileDrop, autoConvert: true),
        "图片粘贴对象的格式集合不正确或仍包含文件拖放格式");

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

static void TestSkinCatalog(string root)
{
    var builtInRoot = Path.Combine(root, "built-in-skins");
    var userRoot = Path.Combine(root, "user-skins");
    Directory.CreateDirectory(builtInRoot);
    Directory.CreateDirectory(userRoot);
    var preview = EncodePng([20, 40, 60, 255], width: 1, height: 1);

    static void WriteSkin(
        string parent,
        string id,
        string name,
        byte[] png,
        bool valid = true)
    {
        var directory = Path.Combine(parent, id);
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "skin.ini"), valid
            ? $"[General]\nname={name}\nauthor=测试作者\ninfo=测试皮肤\n" +
              "[Display]\npinyin_color=0x112233\n" +
              $"[Scheme_H1]\nbg_image=cand_bg.png\n" +
              $"native_appearance={(id == "imported" ? 1 : 0)}\n"
            : $"[General]\nname={name}\n[Scheme_H1]\n");
        if (valid)
            File.WriteAllBytes(Path.Combine(directory, "cand_bg.png"), png);
    }

    WriteSkin(builtInRoot, SkinCatalog.DefaultSkinId, "内置默认", preview);
    WriteSkin(builtInRoot, "official", "官方皮肤", preview);
    WriteSkin(userRoot, SkinCatalog.DefaultSkinId, "同名用户副本", preview);
    WriteSkin(userRoot, "imported", "导入皮肤", preview);
    WriteSkin(userRoot, "broken", "损坏皮肤", preview, valid: false);
    Directory.CreateDirectory(Path.Combine(userRoot, "missing-config"));

    var firstLoad = SkinCatalog.Load(builtInRoot, userRoot);
    var duplicate = firstLoad.Single(item =>
        item.Id == SkinCatalog.DefaultSkinId);
    Require(duplicate.IsBuiltIn && duplicate.Name == "内置默认",
        "同名用户皮肤覆盖了内置皮肤");
    Require(firstLoad.Any(item =>
            item.Id == "imported" && !item.IsBuiltIn && item.IsAvailable),
        "导入皮肤未进入皮肤管理目录");
    var imported = firstLoad.Single(item => item.Id == "imported");
    var importedCard = new SkinCardViewModel(imported, isCurrent: false);
    Require(imported.UsesNativeLayout && importedCard.CanDelete &&
            importedCard.PreviewWidth == 1 && importedCard.PreviewHeight == 1,
        "原生导入皮肤预览未保持素材原始尺寸");
    var builtInCard = new SkinCardViewModel(duplicate, isCurrent: true);
    Require(!builtInCard.CanDelete && builtInCard.PreviewWidth == 420 &&
            builtInCard.PreviewHeight == 82,
        "内置皮肤删除权限或预览逻辑错误");
    Require(firstLoad.Any(item =>
            item.Id == "broken" && !item.IsAvailable &&
            !string.IsNullOrWhiteSpace(item.ValidationError)),
        "损坏皮肤未以不可用状态保留");
    Require(firstLoad.Any(item =>
            item.Id == "missing-config" && !item.IsAvailable),
        "缺失配置文件的皮肤未以不可用状态保留");
    Require(SkinCatalog.ResolveSelectedId("broken", firstLoad) ==
            SkinCatalog.DefaultSkinId &&
            SkinCatalog.ResolveSelectedId("missing", firstLoad) ==
            SkinCatalog.DefaultSkinId,
        "失效的当前皮肤没有回退到默认皮肤");

    var secondLoad = SkinCatalog.Load(builtInRoot, userRoot);
    Require(secondLoad.Any(item => item.Id == "imported" && item.IsAvailable),
        "重新加载后导入皮肤从管理目录消失");

    Directory.CreateDirectory(Path.Combine(userRoot, "sample-2"));
    var uniqueId = SkinCatalog.CreateUniqueUserSkinId(
        Path.Combine(root, "sample.zip"), ["sample"], userRoot);
    Require(uniqueId == "sample-3",
        $"导入皮肤冲突名称生成错误：{uniqueId}");

    var outsideRoot = Path.Combine(root, "outside-skin");
    WriteSkin(root, "outside-skin", "越界皮肤", preview);
    var outsideDescriptor = imported with
    {
        Id = "outside-skin",
        DirectoryPath = outsideRoot
    };
    ExpectException<InvalidDataException>(() =>
        SkinCatalog.DeleteUserSkin(outsideDescriptor, userRoot));
    Require(Directory.Exists(outsideRoot), "越界皮肤目录被错误删除");
    ExpectException<InvalidOperationException>(() =>
        SkinCatalog.DeleteUserSkin(duplicate, userRoot));
    Require(Directory.Exists(duplicate.DirectoryPath), "内置皮肤目录被错误删除");

    SkinCatalog.DeleteUserSkin(imported, userRoot);
    Require(!Directory.Exists(imported.DirectoryPath) &&
            !SkinCatalog.Load(builtInRoot, userRoot).Any(item =>
                item.Id == imported.Id),
        "导入皮肤目录未被删除或目录未刷新");
    Require(SkinCatalog.ResolveSelectedId(
                imported.Id, SkinCatalog.Load(builtInRoot, userRoot)) ==
            SkinCatalog.DefaultSkinId,
        "删除当前导入皮肤后未回退默认皮肤");
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

sealed class PasteTrackingRichTextBox : WinForms.RichTextBox
{
    private const int PasteMessage = 0x0302;
    private const int KeyDownMessage = 0x0100;
    private const int VirtualKeyV = 0x56;

    internal int PasteMessageCount { get; private set; }
    internal int PasteKeyCount { get; private set; }

    protected override void WndProc(ref WinForms.Message message)
    {
        if (message.Msg == PasteMessage) ++PasteMessageCount;
        if (message.Msg == KeyDownMessage &&
            message.WParam.ToInt32() == VirtualKeyV)
        {
            ++PasteKeyCount;
        }
        base.WndProc(ref message);
    }
}

sealed class PasteTrackingCustomControl : WinForms.Control
{
    private const int KeyDownMessage = 0x0100;
    private const int VirtualKeyV = 0x56;

    internal int PasteKeyCount { get; private set; }
    internal ulong LastExtraInfo { get; private set; }
    internal bool ReceivedImage { get; private set; }

    internal PasteTrackingCustomControl()
    {
        SetStyle(WinForms.ControlStyles.Selectable, true);
        TabStop = true;
    }

    protected override void WndProc(ref WinForms.Message message)
    {
        if (message.Msg == KeyDownMessage &&
            message.WParam.ToInt32() == VirtualKeyV)
        {
            ++PasteKeyCount;
            LastExtraInfo = GetMessageExtraInfo().ToUInt64();
            ReceivedImage = System.Windows.Clipboard.ContainsImage();
            return;
        }
        base.WndProc(ref message);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern UIntPtr GetMessageExtraInfo();
}
