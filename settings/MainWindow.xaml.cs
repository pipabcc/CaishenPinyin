using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using Microsoft.Win32;

namespace ShuruSettings;

public partial class MainWindow : Window
{
    private const string Clsid = "{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}";
    private const int OperationCancelled = 1223;
    private static string UserDictionaryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CaishenPinyin", "data", "lexicon", "user_dict.txt");

    private readonly List<CustomPhrase> phrases_ = new();
    private CancellationTokenSource? clipboardQueryCancellation_;
    private int clipboardQueryGeneration_;
    private string? registeredDllPath_;

    public MainWindow()
    {
        InitializeComponent();
        try
        {
            var iconUri = new Uri("pack://application:,,,/app.ico", UriKind.RelativeOrAbsolute);
            Icon = new System.Windows.Media.Imaging.BitmapImage(iconUri);
        }
        catch { }
        ApplySettings(SettingsStore.Load());
        phrases_.AddRange(CustomPhraseStore.Load());
        RefreshPhraseGrid();
        RefreshStatus();
    }

    private void Window_Loaded(object sender, RoutedEventArgs e) =>
        NavigationList.Focus();

    private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        // 允许在非交互输入控件区域拖拽移动无标题栏窗口
        if (StatusText.IsMouseOver) return;
        if (e.LeftButton == MouseButtonState.Pressed &&
            e.OriginalSource is not TextBox &&
            e.OriginalSource is not Button &&
            e.OriginalSource is not ComboBox &&
            e.OriginalSource is not CheckBox &&
            e.OriginalSource is not RadioButton &&
            e.OriginalSource is not ListBoxItem &&
            e.OriginalSource is not System.Windows.Controls.Primitives.Thumb)
        {
            try
            {
                DragMove();
            }
            catch
            {
                // 忽略非主窗口激活状态下的拖拽异常
            }
        }
    }

    private async void NavigationList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (GeneralPage is null || InputPage is null || PhrasePage is null || ClipboardPage is null || DictionaryPage is null)
            return;

        var pages = new FrameworkElement[] { GeneralPage, InputPage, PhrasePage, ClipboardPage, DictionaryPage };
        for (var index = 0; index < pages.Length; ++index)
        {
            pages[index].Visibility = NavigationList.SelectedIndex == index
                ? Visibility.Visible
                : Visibility.Collapsed;
        }

        if (NavigationList.SelectedIndex == 3)
        {
            await RefreshClipboardGridAsync();
        }
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
        DisplayNameBox.Text = settings.DisplayName;
        if (PhraseDirectWindowBox != null) PhraseDirectWindowBox.IsChecked = settings.VvModeOpenWindow;
        if (ClipboardDirectWindowBox != null) ClipboardDirectWindowBox.IsChecked = settings.VModeOpenWindow;
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

    private AppSettings ReadSettings()
    {
        var displayName = SettingsStore.NormalizeDisplayName(DisplayNameBox.Text) ??
            throw new InvalidDataException("输入法列表名称须为 1 至 24 个可见字符。");
        return new AppSettings(
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
            ReadComboValue(CandidateFontSizeBox, "候选字体大小"),
            displayName,
            ClipboardDirectWindowBox?.IsChecked == true,
            PhraseDirectWindowBox?.IsChecked == true);
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var previous = SettingsStore.Load();
            var updated = ReadSettings().Validated();
            var registeredDll = FindRegisteredDll();
            SettingsStore.Save(updated);
            if (!string.Equals(previous.DisplayName, updated.DisplayName, StringComparison.Ordinal))
            {
                try
                {
                    ReregisterInputMethod(registeredDll);
                }
                catch (Win32Exception ex) when (ex.NativeErrorCode == OperationCancelled)
                {
                    SettingsStore.Save(previous);
                    ApplySettings(previous);
                    throw new InvalidOperationException("已取消管理员授权，输入法名称未更改。", ex);
                }
                catch (Exception registrationError)
                {
                    SettingsStore.Save(previous);
                    ApplySettings(previous);
                    try { ReregisterInputMethod(registeredDll); }
                    catch { /* 配置已经恢复；注册回滚错误由下面的完整提示说明。 */ }
                    throw new InvalidOperationException(
                        "输入法重新注册失败，设置已恢复。请确认安装文件完整并允许管理员授权。",
                        registrationError);
                }
                StatusText.Text = "设置与输入法名称已保存。切换到其他输入法后再切回即可刷新显示。";
            }
            else
            {
                StatusText.Text = "设置已保存。切换到其他输入法后再切回即可生效。";
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "保存失败：" + ex.Message, "财神输入法",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private static void ReregisterInputMethod(string? dll)
    {
        if (string.IsNullOrWhiteSpace(dll) || !File.Exists(dll))
            throw new FileNotFoundException("未找到已注册的 ShuruIme.dll。", dll);

        var executable = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System), "regsvr32.exe");
        var startInfo = new ProcessStartInfo(executable)
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden
        };
        startInfo.ArgumentList.Add("/s");
        startInfo.ArgumentList.Add(dll);
        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException("无法启动输入法注册程序。");
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidOperationException($"输入法注册程序返回错误码 {process.ExitCode}。");
    }

    private void FuzzyEnabled_Changed(object sender, RoutedEventArgs e) => UpdateFuzzyChildren();

    private void UpdateFuzzyChildren()
    {
        if (FuzzyChildren is not null)
            FuzzyChildren.IsEnabled = FuzzyEnabledBox.IsChecked == true;
    }

    private void PhraseSearchBox_TextChanged(object sender, TextChangedEventArgs e) =>
        RefreshPhraseGrid();

    private void PhraseSearchButton_Click(object sender, RoutedEventArgs e)
    {
        PhraseSearchBox.Focus();
        RefreshPhraseGrid();
    }

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
        if (!ConfirmDialog.Show(
                this,
                "删除自定义短语",
                $"确定删除“{selected.Phrase}”？",
                "确认删除"))
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
        if (!ConfirmDialog.Show(
                this,
                "清空自定义短语",
                "确定永久清空全部自定义短语？此操作不可撤销。",
                "确认清空"))
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
        registeredDllPath_ = dll;
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

    private void StatusText_MouseLeftButtonUp(
        object sender, MouseButtonEventArgs e)
    {
        e.Handled = true;
        var directory = string.IsNullOrWhiteSpace(registeredDllPath_)
            ? null
            : Path.GetDirectoryName(registeredDllPath_);
        if (string.IsNullOrWhiteSpace(directory) || !Directory.Exists(directory))
        {
            ShowOperationError(new DirectoryNotFoundException(
                "未找到当前输入法安装文件夹。"));
            return;
        }
        OpenFolder(directory);
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
            foreach (var file in root.GetProperty("files").EnumerateArray())
            {
                var path = Path.Combine(directory, file.GetProperty("path").GetString() ?? string.Empty);
                if (File.Exists(path)) entries += file.GetProperty("entries").GetInt64();
            }
            return $"版本 {root.GetProperty("version").GetString()} · {entries:N0} 条";
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
            "CaishenPinyin", "data", "lexicon");
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
        else MessageBox.Show(this, "系统词库目录不存在。", "财神输入法");
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
            FileName = "caishen-user-dict.txt"
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
        if (!ConfirmDialog.Show(
                this,
                "清空自动学习词",
                "确定永久清空全部自动学习词？此操作不可撤销。",
                "确认清空"))
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

    private bool _isUpdatingClipboardUi = false;
    private async Task RefreshClipboardGridAsync(int debounceMilliseconds = 0)
    {
        if (ClipboardGrid is null || ClipboardEnabledBox is null || ClipboardSearchBox is null || ClipboardCountText is null)
            return;

        var cancellation = new CancellationTokenSource();
        var previous = Interlocked.Exchange(
            ref clipboardQueryCancellation_, cancellation);
        previous?.Cancel();
        previous?.Dispose();
        var generation = Interlocked.Increment(ref clipboardQueryGeneration_);
        var query = ClipboardSearchBox.Text?.Trim() ?? string.Empty;

        try
        {
            if (debounceMilliseconds > 0)
            {
                await Task.Delay(debounceMilliseconds, cancellation.Token);
            }

            var result = await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                var config = ClipboardStore.LoadConfig();
                var count = ClipboardStore.CountHistory(query);
                var rows = count == 0
                    ? new List<ClipboardRecord>()
                    : ClipboardStore.QueryHistory(query, 5000, 0);
                return (config, count, rows);
            }, cancellation.Token);

            if (generation != clipboardQueryGeneration_ ||
                cancellation.IsCancellationRequested)
            {
                return;
            }

            _isUpdatingClipboardUi = true;
            ClipboardEnabledBox.IsChecked = result.config.Enabled;
            ClipboardGrid.ItemsSource = result.rows;
            ClipboardCountText.Text = result.count > result.rows.Count
                ? $"共 {result.count:N0} 条记录，当前显示前 {result.rows.Count:N0} 条"
                : $"共 {result.count:N0} 条记录";
        }
        catch (OperationCanceledException)
        {
            // 新搜索已经替代当前搜索。
        }
        catch (Exception ex)
        {
            CrashLogger.Log("MainWindow.RefreshClipboardGrid", ex);
            if (StatusText is not null)
                StatusText.Text = "剪贴板记录加载失败，请查看错误日志。";
        }
        finally
        {
            if (generation == clipboardQueryGeneration_)
                _isUpdatingClipboardUi = false;
        }
    }

    private void ClipboardEnabledBox_Changed(object sender, RoutedEventArgs e)
    {
        if (_isUpdatingClipboardUi || ClipboardEnabledBox is null || StatusText is null) return;

        try
        {
            var config = ClipboardStore.LoadConfig();
            config.Enabled = ClipboardEnabledBox.IsChecked == true;
            ClipboardStore.SaveConfig(config);
            StatusText.Text = config.Enabled ? "剪贴板记录功能已开启。" : "剪贴板记录功能已关闭。";
        }
        catch (Exception ex)
        {
            CrashLogger.Log("MainWindow.SaveClipboardConfig", ex);
            ShowOperationError(ex);
        }
    }

    private void VModeDirectWindowBox_Changed(object sender, RoutedEventArgs e)
    {
        try
        {
            var isClipboard = ReferenceEquals(sender, ClipboardDirectWindowBox);
            var isPhrase = ReferenceEquals(sender, PhraseDirectWindowBox);
            var isChecked = (sender as CheckBox)?.IsChecked == true;

            var current = SettingsStore.Load();
            AppSettings updated;
            if (isClipboard)
            {
                updated = current with { VModeOpenWindow = isChecked };
                StatusText.Text = isChecked ? "剪贴板（按v）已设置为直接打开独立搜索窗口。" : "剪贴板（按v）已设置为内嵌候选窗展示。";
            }
            else if (isPhrase)
            {
                updated = current with { VvModeOpenWindow = isChecked };
                StatusText.Text = isChecked ? "自定义短语（按vv）已设置为直接打开独立搜索窗口。" : "自定义短语（按vv）已设置为内嵌候选窗展示。";
            }
            else
            {
                return;
            }

            SettingsStore.Save(updated);
        }
        catch (Exception ex)
        {
            CrashLogger.Log("MainWindow.SaveVModeOpenWindow", ex);
        }
    }

    private async void ClipboardSearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        await RefreshClipboardGridAsync(150);
    }

    private async void ClipboardSearchButton_Click(object sender, RoutedEventArgs e)
    {
        ClipboardSearchBox.Focus();
        await RefreshClipboardGridAsync();
    }

    private void RowCopy_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement elem && elem.DataContext is ClipboardRecord record)
        {
            try
            {
                if (record.IsImage)
                {
                    if (!File.Exists(record.ImagePath))
                        throw new FileNotFoundException(
                            "剪贴板图片文件不存在", record.ImagePath);
                    ClipboardImageService.SetClipboardImage(record.ImagePath);
                }
                else
                    ClipboardImageService.SetClipboardText(record.Content);
                StatusText.Text = "已将记录复制到系统剪贴板。";
            }
            catch (Exception ex)
            {
                ShowOperationError(ex);
            }
        }
    }

    private async void RowOpenOrEdit_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement elem && elem.DataContext is ClipboardRecord record)
        {
            if (record.Type != ClipboardItemType.Text)
            {
                try
                {
                    OpenClipboardRecord(record);
                    StatusText.Text = record.Type == ClipboardItemType.Image
                        ? "已打开剪贴板图片。"
                        : "已打开剪贴板文件。";
                }
                catch (Exception ex)
                {
                    ShowOperationError(ex);
                }
                return;
            }

            var dlg = new EditClipboardDialog(record.Content) { Owner = this };
            if (dlg.ShowDialog() == true)
            {
                ClipboardStore.UpdateRecordContent(record.Id, dlg.EditedContent);
                await RefreshClipboardGridAsync();
                StatusText.Text = "剪贴板记录已修改并保存。";
            }
        }
    }

    private static void OpenClipboardRecord(ClipboardRecord record)
    {
        var target = record.Type == ClipboardItemType.Image
            ? record.ImagePath
            : record.Content
                .Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)
                .Select(path => path.Trim())
                .FirstOrDefault(path => File.Exists(path) || Directory.Exists(path));
        if (string.IsNullOrWhiteSpace(target) ||
            (!File.Exists(target) && !Directory.Exists(target)))
        {
            throw new FileNotFoundException(
                "剪贴板记录对应的图片或文件不存在。", target);
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = target,
            UseShellExecute = true
        });
    }

    private async void RowDelete_Click(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement elem && elem.DataContext is ClipboardRecord record)
        {
            ClipboardStore.DeleteRecord(record.Id);
            await RefreshClipboardGridAsync();
            StatusText.Text = "已删除该条剪贴板记录。";
        }
    }

    private async void DeleteClipboardItem_Click(object sender, RoutedEventArgs e)
    {
        var selected = ClipboardGrid.SelectedItems
            .OfType<ClipboardRecord>()
            .ToList();
        if (selected.Count == 0)
        {
            StatusText.Text = "请先选择要删除的记录。";
            return;
        }

        if (!ConfirmDialog.Show(
                this,
                "删除剪贴板记录",
                $"确定永久删除选中的 {selected.Count:N0} 条记录？此操作不可撤销。",
                "确认删除"))
            return;

        var deleted = ClipboardStore.DeleteRecords(
            selected.Select(record => record.Id));
        await RefreshClipboardGridAsync();
        StatusText.Text = $"已删除 {deleted:N0} 条剪贴板记录。";
    }

    private async void ClearClipboard_Click(object sender, RoutedEventArgs e)
    {
        if (!ConfirmDialog.Show(
                this,
                "清空剪贴板",
                "确定永久清空全部剪贴板历史记录？此操作不可撤销。",
                "确认清空"))
            return;

        try
        {
            ClipboardStore.ClearHistory();
            await RefreshClipboardGridAsync();
            StatusText.Text = "剪贴板记录已全部清空。";
        }
        catch (Exception ex) { ShowOperationError(ex); }
    }

    private void ShowOperationError(Exception ex) => MessageBox.Show(
        this, "操作失败：" + ex.Message, "财神输入法",
        MessageBoxButton.OK, MessageBoxImage.Error);

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    internal async Task RunClipboardPageSmokeTestAsync()
    {
        if (TryFindResource("ModernTextButtonStyle") is null)
            throw new InvalidOperationException("缺少 ModernTextButtonStyle");
        if (PhraseSearchButton is null)
            throw new InvalidOperationException("自定义短语搜索图标未加载");
        if (StatusText.Cursor != Cursors.Hand)
            throw new InvalidOperationException("安装路径未提供可点击反馈");
        NavigationList.SelectedIndex = 3;
        await RefreshClipboardGridAsync();
        if (ClipboardGrid.ItemsSource is null)
            throw new InvalidOperationException("剪贴板页面未完成数据绑定");
        if (ClipboardGrid.SelectionMode != DataGridSelectionMode.Extended)
            throw new InvalidOperationException("剪贴板列表未启用扩展多选");
        if (new ClipboardRecord { Type = ClipboardItemType.Image }.TypeDisplayName != "图片")
            throw new InvalidOperationException("剪贴板类型未本地化");
        if (ClipboardGrid.Columns.Count != 3)
            throw new InvalidOperationException("剪贴板列表仍包含独立操作列");
        if (new ClipboardRecord { Type = ClipboardItemType.Text }.OpenOrEditLabel != "编辑" ||
            new ClipboardRecord { Type = ClipboardItemType.Image }.OpenOrEditLabel != "打开" ||
            new ClipboardRecord { Type = ClipboardItemType.File }.OpenOrEditLabel != "打开")
            throw new InvalidOperationException("剪贴板行操作名称不正确");
    }

    protected override void OnClosed(EventArgs e)
    {
        clipboardQueryCancellation_?.Cancel();
        clipboardQueryCancellation_?.Dispose();
        clipboardQueryCancellation_ = null;
        base.OnClosed(e);
    }
}
