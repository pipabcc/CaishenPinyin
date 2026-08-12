using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;

namespace ShuruSettings;

public partial class MainWindow : Window
{
    private const string Clsid = "{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}";
    private static string UserDictionaryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "FacaiPinyin", "data", "lexicon", "user_dict.txt");

    private readonly List<CustomPhrase> phrases_ = new();

    public MainWindow()
    {
        InitializeComponent();
        ApplySettings(SettingsStore.Load());
        phrases_.AddRange(CustomPhraseStore.Load());
        RefreshPhraseGrid();
        RefreshStatus();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e) =>
        NavigationList.Focus();

    private void NavigationList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (GeneralPage is null || InputPage is null || PhrasePage is null || DictionaryPage is null)
            return;
        var pages = new FrameworkElement[] { GeneralPage, InputPage, PhrasePage, DictionaryPage };
        for (var index = 0; index < pages.Length; ++index)
            pages[index].Visibility = NavigationList.SelectedIndex == index
                ? Visibility.Visible
                : Visibility.Collapsed;
    }

    private void ApplySettings(AppSettings settings)
    {
        EnglishDefaultBox.IsChecked = settings.EnglishDefault;
        LearningBox.IsChecked = settings.LearningEnabled;
        ContentLoggingBox.IsChecked = settings.ContentLogging;
        FuzzyEnabledBox.IsChecked = settings.FuzzyEnabled;
        FuzzyInitialsBox.IsChecked = settings.FuzzyInitials;
        FuzzyFinalsBox.IsChecked = settings.FuzzyFinals;
        FuzzyMissingVowelBox.IsChecked = settings.FuzzyMissingVowel;
        ShuangpinBox.IsChecked = settings.ShuangpinXiaohe;
        QuanpinBox.IsChecked = !settings.ShuangpinXiaohe;
        FullWidthBox.IsChecked = settings.FullWidthPunctuation;
        HalfWidthBox.IsChecked = !settings.FullWidthPunctuation;
        SelectComboValue(CandidateCountBox, settings.CandidateCount);
        SelectComboValue(CandidateFontSizeBox, settings.CandidateFontSize);
        UpdateFuzzyChildren();
    }

    private static void SelectComboValue(ComboBox box, int value)
    {
        box.SelectedItem = box.Items.OfType<ComboBoxItem>()
            .FirstOrDefault(item => item.Content?.ToString() == value.ToString());
    }

    private static int ReadComboValue(ComboBox box, string fieldName)
    {
        var text = (box.SelectedItem as ComboBoxItem)?.Content?.ToString();
        return int.TryParse(text, out var value)
            ? value
            : throw new InvalidDataException($"请选择{fieldName}。");
    }

    private AppSettings ReadSettings() => new(
        EnglishDefaultBox.IsChecked == true,
        LearningBox.IsChecked == true,
        ContentLoggingBox.IsChecked == true,
        FuzzyEnabledBox.IsChecked == true,
        FuzzyInitialsBox.IsChecked == true,
        FuzzyFinalsBox.IsChecked == true,
        FuzzyMissingVowelBox.IsChecked == true,
        ShuangpinBox.IsChecked == true,
        FullWidthBox.IsChecked == true,
        ReadComboValue(CandidateCountBox, "每页候选数量"),
        ReadComboValue(CandidateFontSizeBox, "候选字体大小"));

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SettingsStore.Save(ReadSettings());
            StatusText.Text = "设置已保存。切换到其他输入法后再切回即可生效。";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "保存失败：" + ex.Message, "发财拼音",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void FuzzyEnabled_Changed(object sender, RoutedEventArgs e) => UpdateFuzzyChildren();

    private void UpdateFuzzyChildren()
    {
        if (FuzzyChildren is not null)
            FuzzyChildren.IsEnabled = FuzzyEnabledBox.IsChecked == true;
    }

    private void PhraseSearchBox_TextChanged(object sender, TextChangedEventArgs e) =>
        RefreshPhraseGrid();

    private void RefreshPhraseGrid()
    {
        if (PhraseGrid is null) return;
        var query = PhraseSearchBox?.Text.Trim() ?? string.Empty;
        var visible = phrases_
            .Where(item => query.Length == 0 ||
                item.Code.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                item.Phrase.Contains(query, StringComparison.CurrentCultureIgnoreCase))
            .ToList();
        PhraseGrid.ItemsSource = visible;
        PhraseCountText.Text = query.Length == 0
            ? $"共 {phrases_.Count:N0} 条短语"
            : $"找到 {visible.Count:N0} 条，共 {phrases_.Count:N0} 条";
    }

    private void AddPhrase_Click(object sender, RoutedEventArgs e)
    {
        var editor = new PhraseEditorWindow { Owner = this };
        if (editor.ShowDialog() == true && editor.Result is not null)
            CommitPhrase(editor.Result, null);
    }

    private void EditPhrase_Click(object sender, RoutedEventArgs e) => EditSelectedPhrase();

    private void PhraseGrid_MouseDoubleClick(object sender, MouseButtonEventArgs e) =>
        EditSelectedPhrase();

    private void EditSelectedPhrase()
    {
        if (PhraseGrid.SelectedItem is not CustomPhrase selected)
        {
            StatusText.Text = "请先选择一条要编辑的自定义短语。";
            return;
        }
        var editor = new PhraseEditorWindow(selected) { Owner = this };
        if (editor.ShowDialog() == true && editor.Result is not null)
            CommitPhrase(editor.Result, selected);
    }

    private void CommitPhrase(CustomPhrase updated, CustomPhrase? original)
    {
        try
        {
            var duplicate = phrases_.Any(item => item != original &&
                item.Code.Equals(updated.Code, StringComparison.OrdinalIgnoreCase) &&
                item.Phrase.Equals(updated.Phrase, StringComparison.Ordinal));
            if (duplicate) throw new InvalidDataException("相同输入码和短语已经存在。");

            if (original is null) phrases_.Add(updated);
            else phrases_[phrases_.IndexOf(original)] = updated;
            SavePhrases("自定义短语已保存。切换输入法后生效。");
        }
        catch (Exception ex)
        {
            ShowOperationError(ex);
        }
    }

    private void DeletePhrase_Click(object sender, RoutedEventArgs e)
    {
        if (PhraseGrid.SelectedItem is not CustomPhrase selected)
        {
            StatusText.Text = "请先选择一条要删除的自定义短语。";
            return;
        }
        if (MessageBox.Show(this, $"确定删除“{selected.Phrase}”？", "删除自定义短语",
                MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        phrases_.Remove(selected);
        SavePhrases("自定义短语已删除。切换输入法后生效。");
    }

    private void ImportPhrases_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "自定义短语文本|*.txt|所有文件|*.*",
            Title = "导入自定义短语"
        };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var imported = CustomPhraseStore.Import(dialog.FileName);
            foreach (var phrase in imported)
            {
                var existing = phrases_.FindIndex(item =>
                    item.Code.Equals(phrase.Code, StringComparison.OrdinalIgnoreCase) &&
                    item.Phrase.Equals(phrase.Phrase, StringComparison.Ordinal));
                if (existing >= 0) phrases_[existing] = phrase;
                else phrases_.Add(phrase);
            }
            SavePhrases($"已导入并合并 {imported.Count:N0} 条自定义短语。切换输入法后生效。");
        }
        catch (Exception ex)
        {
            ShowOperationError(ex);
        }
    }

    private void ExportPhrases_Click(object sender, RoutedEventArgs e)
    {
        if (phrases_.Count == 0)
        {
            StatusText.Text = "当前没有可导出的自定义短语。";
            return;
        }
        var dialog = new SaveFileDialog
        {
            Filter = "自定义短语文本|*.txt",
            FileName = "facai-custom-phrases.txt",
            Title = "导出自定义短语"
        };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            CustomPhraseStore.Save(phrases_, dialog.FileName);
            StatusText.Text = $"已导出 {phrases_.Count:N0} 条自定义短语。";
        }
        catch (Exception ex)
        {
            ShowOperationError(ex);
        }
    }

    private void ClearPhrases_Click(object sender, RoutedEventArgs e)
    {
        if (phrases_.Count == 0) return;
        if (MessageBox.Show(this, "确定清空全部自定义短语？此操作不可撤销。", "清空自定义短语",
                MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        phrases_.Clear();
        SavePhrases("自定义短语已清空。切换输入法后生效。");
    }

    private void SavePhrases(string status)
    {
        try
        {
            CustomPhraseStore.Save(phrases_);
            RefreshPhraseGrid();
            StatusText.Text = status;
        }
        catch (Exception ex)
        {
            phrases_.Clear();
            phrases_.AddRange(CustomPhraseStore.Load());
            RefreshPhraseGrid();
            ShowOperationError(ex);
        }
    }

    private void RefreshStatus()
    {
        var dll = FindRegisteredDll();
        var lexicon = ResolveLexiconDirectory(dll);
        LexiconPathBox.Text = lexicon;
        PackageText.Text = ReadPackageStatus(lexicon);
        var user = new FileInfo(UserDictionaryPath);
        UserDictionaryText.Text = user.Exists
            ? $"{CountDataLines(UserDictionaryPath):N0} 条自动学习词 · {user.Length:N0} 字节\n{UserDictionaryPath}"
            : $"尚未产生自动学习词\n{UserDictionaryPath}";
        StatusText.Text = string.IsNullOrWhiteSpace(dll)
            ? "未检测到已注册的输入法，设置仍会保存到当前用户。"
            : File.Exists(dll)
                ? $"输入法已安装：{dll}"
                : $"注册指向不存在的文件：{dll}";
    }

    private static string ReadPackageStatus(string directory)
    {
        var manifest = Path.Combine(directory, "manifest.json");
        if (!File.Exists(manifest)) return "未找到有效的系统词库清单。";
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifest));
            var root = document.RootElement;
            long entries = 0;
            var valid = true;
            foreach (var file in root.GetProperty("files").EnumerateArray())
            {
                entries += file.GetProperty("entries").GetInt64();
                var path = Path.Combine(directory, file.GetProperty("path").GetString() ?? string.Empty);
                valid &= File.Exists(path) && Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)))
                    .Equals(file.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase);
            }
            return $"版本 {root.GetProperty("version").GetString()} · {entries:N0} 条 · 校验{(valid ? "通过" : "失败")}";
        }
        catch (Exception ex)
        {
            return "词库清单无效：" + ex.Message;
        }
    }

    private static long CountDataLines(string path) => File.ReadLines(path).LongCount(line =>
        !string.IsNullOrWhiteSpace(line) && !line.TrimStart().StartsWith('#') &&
        !line.TrimStart().StartsWith(';'));

    private static string? FindRegisteredDll()
    {
        using var key = Registry.ClassesRoot.OpenSubKey($"CLSID\\{Clsid}\\InprocServer32");
        return key?.GetValue(null) as string;
    }

    private static string ResolveLexiconDirectory(string? dll)
    {
        var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "FacaiPinyin", "data", "lexicon");
        try
        {
            var pointer = Path.Combine(root, "current");
            if (File.Exists(pointer))
            {
                var version = File.ReadAllText(pointer).Trim();
                if (version.Length > 0 && version.All(ch => char.IsLetterOrDigit(ch) || ch is '.' or '_' or '-'))
                {
                    var deployed = Path.Combine(root, "versions", version);
                    if (File.Exists(Path.Combine(deployed, "base_dict.txt"))) return deployed;
                }
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }

        if (!string.IsNullOrWhiteSpace(dll))
        {
            var adjacent = Path.Combine(Path.GetDirectoryName(dll)!, "data", "lexicon");
            if (Directory.Exists(adjacent)) return adjacent;
        }
        var development = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory, "..", "..", "..", "..", "data", "lexicon"));
        return Directory.Exists(development)
            ? development
            : Path.Combine(AppContext.BaseDirectory, "data", "lexicon");
    }

    private void OpenLexicon_Click(object sender, RoutedEventArgs e)
    {
        if (Directory.Exists(LexiconPathBox.Text)) OpenFolder(LexiconPathBox.Text);
        else MessageBox.Show(this, "系统词库目录不存在。", "发财拼音");
    }

    private void OpenUserDictionary_Click(object sender, RoutedEventArgs e) =>
        OpenFolder(Path.GetDirectoryName(UserDictionaryPath)!);

    private void Refresh_Click(object sender, RoutedEventArgs e) => RefreshStatus();

    private void ExportUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(UserDictionaryPath))
        {
            StatusText.Text = "当前没有可导出的自动学习词。";
            return;
        }
        var dialog = new SaveFileDialog
        {
            Filter = "用户词文本|*.txt",
            FileName = "facai-user-dict.txt"
        };
        if (dialog.ShowDialog(this) == true)
        {
            try
            {
                File.Copy(UserDictionaryPath, dialog.FileName, true);
                StatusText.Text = "自动学习词已导出。";
            }
            catch (Exception ex) { ShowOperationError(ex); }
        }
    }

    private void ImportUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Filter = "用户词文本|*.txt|所有文件|*.*" };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var lines = File.ReadAllLines(dialog.FileName);
            if (lines.Any(line => !string.IsNullOrWhiteSpace(line) &&
                !line.TrimStart().StartsWith('#') && line.Split('\t').Length is < 3 or > 5))
                throw new InvalidDataException("文件含有格式错误的用户词行。");
            AtomicWrite(File.ReadAllBytes(dialog.FileName), UserDictionaryPath);
            RefreshStatus();
            StatusText.Text = "自动学习词已导入。切换输入法后生效。";
        }
        catch (Exception ex) { ShowOperationError(ex); }
    }

    private void ClearUserDictionary_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "确定永久清空全部自动学习词？此操作不可撤销。", "清空自动学习词",
                MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        try
        {
            AtomicWrite(Array.Empty<byte>(), UserDictionaryPath);
            RefreshStatus();
            StatusText.Text = "自动学习词已清空。切换输入法后生效。";
        }
        catch (Exception ex) { ShowOperationError(ex); }
    }

    private static void AtomicWrite(byte[] content, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var temporary = target + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            using (var stream = new FileStream(
                       temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None,
                       4096, FileOptions.WriteThrough))
            {
                stream.Write(content);
                stream.Flush(true);
            }
            File.Move(temporary, target, true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    private static void OpenFolder(string path)
    {
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"")
        {
            UseShellExecute = true
        });
    }

    private void ShowOperationError(Exception ex) => MessageBox.Show(
        this, "操作失败：" + ex.Message, "发财拼音",
        MessageBoxButton.OK, MessageBoxImage.Error);

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
