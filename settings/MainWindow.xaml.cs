using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using Microsoft.Win32;

namespace ShuruSettings;

public partial class MainWindow : Window
{
    private const string Clsid = "{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}";
    private static string UserDictionaryPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "FacaiPinyin", "data", "lexicon", "user_dict.txt");

    public MainWindow()
    {
        InitializeComponent();
        ApplySettings(SettingsStore.Load());
        RefreshStatus();
    }

    private void ApplySettings(AppSettings s)
    {
        EnglishDefaultBox.IsChecked = s.EnglishDefault;
        LearningBox.IsChecked = s.LearningEnabled;
        ContentLoggingBox.IsChecked = s.ContentLogging;
        FuzzyEnabledBox.IsChecked = s.FuzzyEnabled;
        FuzzyInitialsBox.IsChecked = s.FuzzyInitials;
        FuzzyFinalsBox.IsChecked = s.FuzzyFinals;
        FuzzyMissingVowelBox.IsChecked = s.FuzzyMissingVowel;
        XiaoheBox.IsChecked = s.ShuangpinXiaohe;
        QuanpinBox.IsChecked = !s.ShuangpinXiaohe;
        FullWidthBox.IsChecked = s.FullWidthPunctuation;
        HalfWidthBox.IsChecked = !s.FullWidthPunctuation;
        CandidateCountBox.Text = s.CandidateCount.ToString();
        CandidateFontSizeBox.Text = s.CandidateFontSize.ToString();
        UpdateFuzzyChildren();
    }

    private AppSettings ReadSettings()
    {
        if (!int.TryParse(CandidateCountBox.Text, out var count) || count is < 3 or > 9)
            throw new InvalidDataException("候选数量必须为 3–9。");
        if (!int.TryParse(CandidateFontSizeBox.Text, out var font) || font is < 14 or > 32)
            throw new InvalidDataException("候选字体必须为 14–32。");
        return new AppSettings(EnglishDefaultBox.IsChecked == true, LearningBox.IsChecked == true,
            ContentLoggingBox.IsChecked == true, FuzzyEnabledBox.IsChecked == true,
            FuzzyInitialsBox.IsChecked == true, FuzzyFinalsBox.IsChecked == true,
            FuzzyMissingVowelBox.IsChecked == true, XiaoheBox.IsChecked == true,
            FullWidthBox.IsChecked == true, count, font);
    }

    private void RefreshStatus()
    {
        var dll = FindRegisteredDll();
        var lexicon = ResolveLexiconDir(dll);
        LexiconPathBox.Text = lexicon;
        PackageText.Text = ReadPackageStatus(lexicon);
        var user = new FileInfo(UserDictionaryPath);
        UserDictionaryText.Text = user.Exists ? $"{UserDictionaryPath}\n{CountDataLines(UserDictionaryPath):N0} 条，{user.Length:N0} 字节" : $"{UserDictionaryPath}\n尚未创建";
        StatusText.Text = string.IsNullOrWhiteSpace(dll) ? "未检测到注册的输入法；设置仍保存到当前用户，下一实例会读取。" :
            File.Exists(dll) ? $"已注册：{dll}" : $"注册指向不存在的 DLL：{dll}";
    }

    private static string ReadPackageStatus(string dir)
    {
        var manifest = Path.Combine(dir, "manifest.json");
        if (!File.Exists(manifest)) return "ID: 未知　版本: 未知　校验: 缺少 manifest　条目数: 0";
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(manifest));
            var root = doc.RootElement;
            long entries = 0; var valid = true;
            foreach (var file in root.GetProperty("files").EnumerateArray())
            {
                entries += file.GetProperty("entries").GetInt64();
                var path = Path.Combine(dir, file.GetProperty("path").GetString() ?? "");
                valid &= File.Exists(path) && Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).Equals(file.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase);
            }
            return $"ID: {root.GetProperty("packageId").GetString()}　版本: {root.GetProperty("version").GetString()}　校验: {(valid ? "通过" : "失败")}　条目数: {entries:N0}";
        }
        catch (Exception ex) { return "manifest 无效：" + ex.Message; }
    }

    private static long CountDataLines(string path) => File.ReadLines(path).LongCount(x => !string.IsNullOrWhiteSpace(x) && !x.TrimStart().StartsWith('#') && !x.TrimStart().StartsWith(';'));
    private static string? FindRegisteredDll() { using var key = Registry.ClassesRoot.OpenSubKey($"CLSID\\{Clsid}\\InprocServer32"); return key?.GetValue(null) as string; }
    private static string ResolveLexiconDir(string? dll)
    {
        if (!string.IsNullOrWhiteSpace(dll)) { var p = Path.Combine(Path.GetDirectoryName(dll)!, "data", "lexicon"); if (Directory.Exists(p)) return p; }
        var dev = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "data", "lexicon"));
        return Directory.Exists(dev) ? dev : Path.Combine(AppContext.BaseDirectory, "data", "lexicon");
    }

    private void OpenFolder(string path)
    {
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
    }
    private void OpenLexicon_Click(object sender, RoutedEventArgs e) { if (Directory.Exists(LexiconPathBox.Text)) OpenFolder(LexiconPathBox.Text); else MessageBox.Show(this, "系统词库目录不存在。", "发财拼音"); }
    private void OpenUserDictionary_Click(object sender, RoutedEventArgs e) => OpenFolder(Path.GetDirectoryName(UserDictionaryPath)!);
    private void Refresh_Click(object sender, RoutedEventArgs e) => RefreshStatus();
    private void FuzzyEnabled_Changed(object sender, RoutedEventArgs e) => UpdateFuzzyChildren();
    private void UpdateFuzzyChildren() { if (FuzzyChildren != null) FuzzyChildren.IsEnabled = FuzzyEnabledBox.IsChecked == true; }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try { SettingsStore.Save(ReadSettings()); StatusText.Text = "设置已原子保存。请切换到其他输入法再切回；新实例保证生效。"; }
        catch (Exception ex) { MessageBox.Show(this, "保存失败：" + ex.Message, "发财拼音", MessageBoxButton.OK, MessageBoxImage.Error); }
    }

    private void ExportUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(UserDictionaryPath)) { MessageBox.Show(this, "尚无用户词。", "发财拼音"); return; }
        var dialog = new SaveFileDialog { Filter = "用户词文本|*.txt", FileName = "facai-user-dict.txt" };
        if (dialog.ShowDialog(this) == true) try { File.Copy(UserDictionaryPath, dialog.FileName, true); } catch (Exception ex) { ShowOperationError(ex); }
    }
    private void ImportUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "用户词文本|*.txt|所有文件|*.*" };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var lines = File.ReadAllLines(dialog.FileName); if (lines.Any(x => !string.IsNullOrWhiteSpace(x) && !x.TrimStart().StartsWith('#') && x.Split('\t').Length is < 3 or > 5)) throw new InvalidDataException("文件含非法用户词行。");
            AtomicReplace(dialog.FileName, UserDictionaryPath); RefreshStatus(); StatusText.Text = "用户词已导入；重新切换输入法后生效。";
        }
        catch (Exception ex) { ShowOperationError(ex); }
    }
    private void ClearUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "确定永久清空全部用户词？此操作不可撤销。", "危险操作确认", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes) return;
        try { Directory.CreateDirectory(Path.GetDirectoryName(UserDictionaryPath)!); AtomicWrite(Array.Empty<byte>(), UserDictionaryPath); RefreshStatus(); StatusText.Text = "用户词已清空；重新切换输入法后生效。"; } catch (Exception ex) { ShowOperationError(ex); }
    }
    private static void AtomicReplace(string source, string target) => AtomicWrite(File.ReadAllBytes(source), target);
    private static void AtomicWrite(byte[] content, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!); var temp = target + ".tmp-" + Guid.NewGuid().ToString("N");
        try { using (var f = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough)) { f.Write(content); f.Flush(true); } File.Move(temp, target, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
    private void ShowOperationError(Exception ex) => MessageBox.Show(this, "操作失败：" + ex.Message, "发财拼音", MessageBoxButton.OK, MessageBoxImage.Error);
    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
