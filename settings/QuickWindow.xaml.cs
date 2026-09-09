using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace ShuruSettings
{
    public enum QuickWindowMode
    {
        Clipboard,
        CustomPhrases
    }

    public class QuickItemDisplay
    {
        public string Id { get; set; } = string.Empty;
        public string DisplayTitle { get; set; } = string.Empty;
        public string Content { get; set; } = string.Empty;
        public bool IsImage { get; set; } = false;
        public string ImagePath { get; set; } = string.Empty;
        public object? RawData { get; set; }
    }

    public partial class QuickWindow : Window
    {
        [DllImport("user32.dll")]
        private static extern IntPtr GetForegroundWindow();

        private readonly QuickWindowMode _mode;
        private readonly string? _directCommitToken;
        private readonly ObservableCollection<QuickItemDisplay> _items = new();
        private IntPtr _targetWindow;
        private CancellationTokenSource? _loadCancellation;
        private int _loadGeneration;
        private bool _isPasting;
        private bool _isDeleting;
        private bool _isClosing;
        private bool _isClosed;
        private CancellationTokenSource? _pasteCancellation;
        private bool _isShowingDialog;
        private bool _directCommitSessionFinished;

        public QuickWindow(
            QuickWindowMode mode = QuickWindowMode.Clipboard,
            string? directCommitToken = null,
            IntPtr targetWindow = default)
        {
            InitializeComponent();
            _mode = mode;
            RecordListBox.ItemsSource = _items;
            _directCommitToken =
                DirectTextCommitRequestStore.IsValidToken(directCommitToken)
                    ? directCommitToken!.ToLowerInvariant()
                    : null;

            // 记录唤起此快捷窗口前的活动窗口，以便选择后准确粘贴回原窗口
            _targetWindow = targetWindow != IntPtr.Zero
                ? targetWindow
                : GetForegroundWindow();

            if (_mode == QuickWindowMode.Clipboard)
            {
                TitleBlock.Text = "复制记录";
                SearchPlaceholder.Text = "搜索记录内容...";
            }
            else
            {
                TitleBlock.Text = "自定义短语";
                SearchPlaceholder.Text = "搜索短语内容或缩写...";
            }

            PositionWindowNearCursor();
        }

        [DllImport("user32.dll")]
        private static extern bool GetCursorPos(out POINT lpPoint);

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int X;
            public int Y;
        }

        private void PositionWindowNearCursor()
        {
            double left = SystemParameters.PrimaryScreenWidth / 2 - Width / 2;
            double top = SystemParameters.PrimaryScreenHeight / 2 - Height / 2;

            if (GetCursorPos(out POINT mouse))
            {
                left = mouse.X + 10;
                top = mouse.Y + 10;

                if (left + Width > SystemParameters.VirtualScreenWidth)
                {
                    left = mouse.X - Width - 10;
                }
                if (top + Height > SystemParameters.VirtualScreenHeight)
                {
                    top = mouse.Y - Height - 10;
                }
            }

            Left = Math.Max(10, left);
            Top = Math.Max(10, top);
        }

        private async void Window_Loaded(object sender, RoutedEventArgs e)
        {
            await LoadDataAsync();
            if (!_isClosing && !_isClosed) SearchBox.Focus();
        }

        private void Window_Deactivated(object sender, EventArgs e)
        {
            if (!_isPasting && !_isShowingDialog && !_isClosing && !_isClosed) Close();
        }

        private void TitleBar_MouseDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left)
            {
                DragMove();
            }
        }

        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            WindowState = WindowState.Minimized;
        }

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            Close();
        }

        private async Task LoadDataAsync(
            int debounceMilliseconds = 0, int preferredSelection = 0)
        {
            var cancellation = new CancellationTokenSource();
            var previous = Interlocked.Exchange(
                ref _loadCancellation, cancellation);
            previous?.Cancel();
            previous?.Dispose();
            var generation = Interlocked.Increment(ref _loadGeneration);
            var query = SearchBox.Text.Trim();

            try
            {
                if (debounceMilliseconds > 0)
                {
                    await Task.Delay(
                        debounceMilliseconds, cancellation.Token);
                }

                var items = await Task.Run(() =>
                {
                    cancellation.Token.ThrowIfCancellationRequested();
                    if (_mode == QuickWindowMode.Clipboard)
                    {
                        return ClipboardStore.QueryHistory(query, 500, 0)
                            .Select(item => new QuickItemDisplay
                            {
                                Id = item.Id,
                                DisplayTitle = item.DisplayTitle,
                                Content = item.Content,
                                IsImage = item.IsImage,
                                ImagePath = item.ImagePath,
                                RawData = item
                            })
                            .ToList();
                    }

                    IEnumerable<CustomPhrase> phrases =
                        CustomPhraseStore.Load();
                    if (!string.IsNullOrEmpty(query))
                    {
                        phrases = phrases.Where(phrase =>
                            phrase.Code.Contains(
                                query, StringComparison.OrdinalIgnoreCase) ||
                            phrase.Phrase.Contains(
                                query, StringComparison.OrdinalIgnoreCase));
                    }
                    return phrases.Take(500)
                        .Select(phrase => new QuickItemDisplay
                        {
                            Id = $"{phrase.Code}_{phrase.Phrase}",
                            DisplayTitle =
                                $"{phrase.Code} → {phrase.Phrase}",
                            Content = phrase.Phrase,
                            RawData = phrase
                        })
                        .ToList();
                }, cancellation.Token);

                if (generation != _loadGeneration || cancellation.IsCancellationRequested)
                    return;
                _items.Clear();
                foreach (var item in items)
                {
                    _items.Add(item);
                }
                RecordListBox.SelectedIndex = _items.Count == 0
                    ? -1 : Math.Clamp(preferredSelection, 0, _items.Count - 1);
                EmptyHint.Visibility = _items.Count == 0
                    ? Visibility.Visible
                    : Visibility.Collapsed;
            }
            catch (OperationCanceledException)
            {
                // 新查询已经替代当前查询。
            }
            catch (Exception ex)
            {
                CrashLogger.Log("QuickWindow.LoadData", ex);
                if (generation == _loadGeneration)
                {
                    _items.Clear();
                    EmptyHint.Visibility = Visibility.Visible;
                }
            }
        }

        private async void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            SearchPlaceholder.Visibility = string.IsNullOrEmpty(SearchBox.Text) ? Visibility.Visible : Visibility.Collapsed;
            await LoadDataAsync(150);
        }

        private void SearchBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key is Key.Down or Key.Up)
            {
                RecordListBox.SelectedIndex = e.Key == Key.Down
                    ? 0 : _items.Count - 1;
                FocusSelectedRecord();
                e.Handled = true;
            }
            else if (e.Key == Key.Enter)
            {
                if (RecordListBox.Items.Count > 0)
                {
                    var item = RecordListBox.Items[0] as QuickItemDisplay;
                    if (item != null) PasteItem(item);
                }
                e.Handled = true;
            }
            else if (e.Key == Key.Escape)
            {
                Close();
                e.Handled = true;
            }
        }

        private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape)
            {
                Close();
                e.Handled = true;
            }
        }

        private async void RecordListBox_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter)
            {
                e.Handled = true;
                if (RecordListBox.SelectedItem is QuickItemDisplay item)
                {
                    PasteItem(item);
                }
            }
            else if (e.Key == Key.Delete)
            {
                // 先截断路由，再等待数据库；同一次 Delete 不能被其它处理器重复执行。
                e.Handled = true;
                if (RecordListBox.SelectedItem is QuickItemDisplay item)
                {
                    await DeleteRecordAsync(item);
                }
            }
            else if (e.Key is Key.Down or Key.Up)
            {
                e.Handled = true;
                if (_items.Count == 0) return;
                var step = e.Key == Key.Down ? 1 : -1;
                RecordListBox.SelectedIndex = Math.Clamp(
                    RecordListBox.SelectedIndex + step, 0, _items.Count - 1);
                FocusSelectedRecord();
            }
        }

        private void FocusSelectedRecord()
        {
            if (RecordListBox.SelectedItem is not QuickItemDisplay item)
            {
                RecordListBox.Focus();
                return;
            }
            RecordListBox.ScrollIntoView(item);
            RecordListBox.UpdateLayout();
            if (RecordListBox.ItemContainerGenerator.ContainerFromItem(item)
                is ListBoxItem container)
                container.Focus();
        }

        private void Item_Click(object sender, RoutedEventArgs e)
        {
            e.Handled = true;
            if (sender is FrameworkElement elem && elem.DataContext is QuickItemDisplay item)
            {
                PasteItem(item);
            }
        }

        private async void DeleteItem_Click(object sender, RoutedEventArgs e)
        {
            e.Handled = true;
            if (sender is FrameworkElement elem && elem.DataContext is QuickItemDisplay item)
            {
                await DeleteRecordAsync(item);
            }
        }

        private async Task DeleteRecordAsync(QuickItemDisplay item)
        {
            if (_isDeleting || _isPasting || _isClosed) return;
            var index = _items.IndexOf(item);
            if (index < 0) return;
            _isDeleting = true;
            try
            {
                if (_mode == QuickWindowMode.Clipboard)
                {
                    var deleted = await Task.Run(() =>
                        ClipboardStore.DeleteRecords(new[] { item.Id }));
                    if (_isClosed) return;
                    if (deleted == 0)
                    {
                        ShowTransientTitle("删除失败，请刷新后重试");
                        return;
                    }
                }
                else
                {
                    if (item.RawData is not CustomPhrase phrase) return;
                    _isShowingDialog = true;
                    bool confirmed;
                    try
                    {
                        confirmed = ConfirmDialog.Show(this, "确认删除",
                            $"确定要删除自定义短语「{item.DisplayTitle}」吗？", "确认删除");
                    }
                    finally
                    {
                        _isShowingDialog = false;
                    }
                    if (!confirmed) return;
                    await Task.Run(() =>
                    {
                        var phrases = CustomPhraseStore.Load().ToList();
                        phrases.RemoveAll(p => p.Code == phrase.Code && p.Phrase == phrase.Phrase);
                        CustomPhraseStore.Save(phrases);
                    });
                }

                // 新查询会撤销删除前仍在运行的搜索，防止旧结果把已删除记录重新放回列表。
                if (!_isClosed)
                {
                    await LoadDataAsync(preferredSelection: index);
                    if (!_isClosed) FocusSelectedRecord();
                }
            }
            catch (Exception ex)
            {
                CrashLogger.Log("QuickWindow.DeleteRecord", ex);
                if (!_isClosed) ShowTransientTitle("删除失败，请稍后重试");
            }
            finally
            {
                _isDeleting = false;
            }
        }

        private async void PasteItem(QuickItemDisplay item)
        {
            if (_isPasting || _isDeleting || _isClosed || !_items.Contains(item)) return;
            if (!item.IsImage && _directCommitToken != null && _directCommitSessionFinished)
            {
                ShowTransientTitle("上屏会话已结束，请重新打开记录窗口");
                return;
            }
            _isPasting = true;
            using var cancellation = new CancellationTokenSource();
            _pasteCancellation = cancellation;
            try
            {
                // 正文来自被点击记录的快照。文本直接交回原 TSF 会话，不依赖剪贴板可写。
                bool canTryDirect = !item.IsImage &&
                    _directCommitToken != null &&
                    !_directCommitSessionFinished;

                if (canTryDirect)
                {
                    Hide();
                    var directResult = await PasteDirectTextAsync(item.Content, cancellation.Token);
                    if (directResult == DirectTextCommitResult.Success)
                    {
                        Close();
                        return;
                    }
                    Show();
                    ShowTransientTitle(DescribeDirectFailure(directResult));
                    return;
                }
                // 统一的剪贴板还原粘贴（文本 / 图片）：
                if (item.IsImage)
                {
                    if (!File.Exists(item.ImagePath))
                        throw new FileNotFoundException(
                            "剪贴板图片文件不存在", item.ImagePath);
                    await ClipboardImageService.SetClipboardImageAsync(
                        item.ImagePath);
                }
                else
                {
                    await ClipboardImageService.SetClipboardTextAsync(
                        item.Content, cancellation.Token);
                }

                // 剪贴板准备期间窗口保持可见，允许 Esc 取消；准备成功后才交还焦点。
                cancellation.Token.ThrowIfCancellationRequested();
                Hide();
                if (!await ClipboardPasteService.PasteCurrentClipboardAsync(
                        _targetWindow, cancellation.Token))
                {
                    Show();
                    ShowTransientTitle("粘贴失败：目标窗口未响应");
                    return;
                }
                Close();
            }
            catch (OperationCanceledException) when (_isClosed)
            {
                // 窗口关闭后取消尚未提交的操作。
            }
            catch (Exception ex)
            {
                CrashLogger.Log("QuickWindow.PasteItem", ex);
                if (!_isClosed)
                {
                    if (!IsVisible) Show();
                    ShowTransientTitle(DescribeFailure(ex));
                }
            }
            finally
            {
                _isPasting = false;
                _pasteCancellation = null;
            }
        }

        private static string DescribeFailure(Exception ex)
        {
            if (ex is COMException) return "复制失败：剪贴板被其它程序占用";
            var message = ex.Message;
            return message.Length > 60 ? message[..60] : message;
        }

        private async Task<DirectTextCommitResult> PasteDirectTextAsync(
            string text, CancellationToken cancellationToken)
        {
            if (!await ClipboardPasteService.ActivateTargetAsync(
                    _targetWindow, cancellationToken: cancellationToken).ConfigureAwait(true))
            {
                _directCommitSessionFinished = true;
                DirectTextCommitRequestStore.CancelSession(
                    _directCommitToken!);
                return DirectTextCommitResult.TargetUnavailable;
            }
            var result = await DirectTextCommitRequestStore.PublishAndWaitAsync(
                _directCommitToken!, text, cancellationToken).ConfigureAwait(true);
            _directCommitSessionFinished = true;
            if (result == DirectTextCommitResult.Timeout)
            {
                DirectTextCommitRequestStore.CancelSession(
                    _directCommitToken!);
            }
            return result;
        }

        private static string DescribeDirectFailure(
            DirectTextCommitResult result) => result switch
        {
            DirectTextCommitResult.TargetUnavailable => "上屏失败：目标窗口不可用",
            DirectTextCommitResult.ContextChanged => "上屏失败：输入焦点已改变",
            DirectTextCommitResult.SensitiveContext => "上屏已拒绝：当前为敏感输入框",
            DirectTextCommitResult.RequestExpired => "上屏超时：目标窗口未确认",
            DirectTextCommitResult.InvalidRequest => "上屏失败：请求无效",
            DirectTextCommitResult.CommitFailed => "上屏失败：目标输入框拒绝写入",
            DirectTextCommitResult.Timeout => "上屏超时：目标窗口未响应",
            _ => "上屏失败"
        };

        private void ShowTransientTitle(string message)
        {
            var previous = TitleBlock.Text;
            TitleBlock.Text = message;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
            timer.Tick += (s, e) =>
            {
                TitleBlock.Text = previous;
                timer.Stop();
            };
            timer.Start();
        }

        protected override void OnClosing(CancelEventArgs e)
        {
            // Close 自身会引发 Deactivated，必须在原生窗口销毁前阻止再次进入 Close。
            _isClosing = true;
            base.OnClosing(e);
            if (e.Cancel) _isClosing = false;
        }

        protected override void OnClosed(EventArgs e)
        {
            _isClosed = true;
            _pasteCancellation?.Cancel();
            if (_directCommitToken != null && !_directCommitSessionFinished)
            {
                try
                {
                    DirectTextCommitRequestStore.CancelSession(
                        _directCommitToken);
                }
                catch (Exception ex)
                {
                    CrashLogger.Log("DirectTextCommit.WindowClosed", ex);
                }
            }
            _loadCancellation?.Cancel();
            _loadCancellation?.Dispose();
            _loadCancellation = null;
            base.OnClosed(e);
        }
    }
}
