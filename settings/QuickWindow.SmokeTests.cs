using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace ShuruSettings;

public partial class QuickWindow
{
    internal async Task RunClipboardInteractionSmokeTestAsync()
    {
        RequireIsolatedSmokeHistory();
        var prefix = "快捷窗口删除测试" + Guid.NewGuid().ToString("N");
        var ids = Enumerable.Range(0, 3).Select(i => prefix + i).ToArray();
        try
        {
            for (var index = 0; index < ids.Length; ++index)
                ClipboardStore.AddRecord(new ClipboardRecord
                {
                    Id = ids[index], Content = prefix + index, DisplayTitle = prefix + index,
                    CreatedTime = DateTime.Now.AddMinutes(-index)
                });
            SearchBox.Text = prefix;
            await LoadDataAsync();
            SmokeRequire(_items.Count == 3, "快捷窗口测试记录未加载");
            SearchBox.Focus();
            var deleteInSearch = RaiseSmokeKey(SearchBox, Key.Delete, previewOnly: true);
            SmokeRequire(!deleteInSearch.Handled && !_isDeleting,
                "搜索框 Delete 被当成删除记录");

            RaiseSmokeKey(SearchBox, Key.Down);
            RaiseSmokeKey(RecordListBox, Key.Down);
            SmokeRequire(RecordListBox.SelectedIndex == 1 &&
                RecordListBox.IsKeyboardFocusWithin, "上下键没有移动到第二条记录");
            RaiseSmokeKey(RecordListBox, Key.Up);
            SmokeRequire(RecordListBox.SelectedIndex == 0, "上方向键未移动选择");

            var second = _items[1];
            RecordListBox.ScrollIntoView(second);
            RecordListBox.UpdateLayout();
            var container = RecordListBox.ItemContainerGenerator.ContainerFromItem(second);
            var deleteButton = FindSmokeButton(container, "DeleteBtn");
            var click = new RoutedEventArgs(Button.ClickEvent);
            deleteButton.RaiseEvent(click);
            SmokeRequire(click.Handled, "删除按钮没有截断点击事件");
            await WaitForSmokeConditionAsync(() => !_isDeleting);
            SmokeRequire(!_isPasting && IsVisible && _items.Count == 2 &&
                ((QuickItemDisplay)RecordListBox.SelectedItem).Id == ids[2] &&
                RecordListBox.IsKeyboardFocusWithin,
                "鼠标删除后未留在窗口并选中下一条");

            var keyboardDelete = RaiseSmokeKey(RecordListBox, Key.Delete);
            SmokeRequire(keyboardDelete.Handled, "Delete 未在异步删除前截断事件");
            await WaitForSmokeConditionAsync(() => !_isDeleting);
            SmokeRequire(_items.Count == 1 && RecordListBox.SelectedIndex == 0,
                "删除末条后未选中上一条");
            RaiseSmokeKey(RecordListBox, Key.Delete);
            await WaitForSmokeConditionAsync(() => !_isDeleting);
            SmokeRequire(_items.Count == 0 && !_isPasting && IsVisible &&
                EmptyHint.Visibility == Visibility.Visible, "连续删除后空窗口未保留");
        }
        finally { ClipboardStore.DeleteRecords(ids); }
    }

    internal static async Task RunDirectTextCommitSmokeTestAsync(IntPtr targetWindow)
    {
        RequireIsolatedSmokeHistory();
        var token = Guid.NewGuid().ToString("N");
        var prefix = "快捷窗口直接上屏测试" + token;
        var ids = new[] { prefix + "1", prefix + "2" };
        var expected = string.Concat(Enumerable.Repeat("第二条记录\r\n末尾完整🙂\n", 1000));
        var previousDirectory = Environment.GetEnvironmentVariable("CAISHEN_DIRECT_COMMIT_REQUEST_DIR");
        var directory = Path.Combine(Path.GetTempPath(), "caishen-quick-direct-" + token);
        Directory.CreateDirectory(directory);
        Environment.SetEnvironmentVariable("CAISHEN_DIRECT_COMMIT_REQUEST_DIR", directory);
        var window = new QuickWindow(QuickWindowMode.Clipboard, token, targetWindow);
        var clipboardHeld = false;
        try
        {
            ClipboardStore.AddRecord(new ClipboardRecord
            {
                Id = ids[0], Content = prefix + "第一条", DisplayTitle = prefix + "第一条"
            });
            ClipboardStore.AddRecord(new ClipboardRecord
            {
                Id = ids[1], Content = expected, DisplayTitle = prefix + "第二条",
                CreatedTime = DateTime.Now.AddMinutes(-1)
            });
            window.Show();
            window.SearchBox.Text = prefix;
            await window.LoadDataAsync();
            SmokeRequire(window._items.Count == 2 && window._items[1].Content == expected,
                "第二条长文本测试数据未加载");
            window.RecordListBox.UpdateLayout();
            var container = window.RecordListBox.ItemContainerGenerator.ContainerFromIndex(1);
            var commitButton = FindSmokeButton(container, "CommitBtn");

            // 已被其它进程占用时直接沿用占用状态；测试只读序号，不改变用户剪贴板。
            clipboardHeld = SmokeOpenClipboard(IntPtr.Zero);
            var sequence = SmokeClipboardSequence();
            commitButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            var request = DirectTextCommitRequestStore.RequestPath(token);
            await WaitForSmokeConditionAsync(() => File.Exists(request));
            SmokeRequire(File.ReadAllText(request) == DirectTextCommitRequestStore.RequestHeader + expected,
                "点击第二条没有提交完整的多行正文快照");
            var result = DirectTextCommitRequestStore.ResultPath(token);
            File.WriteAllText(result + ".tmp", DirectTextCommitRequestStore.ResultHeader + "success\n",
                new UTF8Encoding(false));
            File.Move(result + ".tmp", result);
            await WaitForSmokeConditionAsync(() => !window._isPasting);
            SmokeRequire(window._isClosed && SmokeClipboardSequence() == sequence,
                "文本直接上屏仍依赖剪贴板写入，或成功后没有关闭窗口");
        }
        finally
        {
            if (clipboardHeld) SmokeCloseClipboard();
            if (!window._isClosed) window.Close();
            await WaitForSmokeConditionAsync(() => !window._isPasting);
            ClipboardStore.DeleteRecords(ids);
            Environment.SetEnvironmentVariable("CAISHEN_DIRECT_COMMIT_REQUEST_DIR", previousDirectory);
            Directory.Delete(directory, recursive: true);
        }
    }

    internal static async Task RunClipboardCancellationSmokeTestAsync(IntPtr targetWindow)
    {
        RequireIsolatedSmokeHistory();
        var id = "快捷窗口取消测试" + Guid.NewGuid().ToString("N");
        var window = new QuickWindow(targetWindow: targetWindow);
        var clipboardHeld = false;
        try
        {
            ClipboardStore.AddRecord(new ClipboardRecord
            {
                Id = id, Content = id, DisplayTitle = id
            });
            window.Show();
            window.SearchBox.Text = id;
            await window.LoadDataAsync();
            window.RecordListBox.UpdateLayout();
            var container = window.RecordListBox.ItemContainerGenerator.ContainerFromIndex(0);
            var commitButton = FindSmokeButton(container, "CommitBtn");
            clipboardHeld = SmokeOpenClipboard(IntPtr.Zero);
            SmokeRequire(clipboardHeld, "无法建立剪贴板占用，取消测试未执行");
            var sequence = SmokeClipboardSequence();
            commitButton.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            SmokeRequire(window.IsVisible && window._isPasting,
                "等待剪贴板期间窗口消失，无法取消");
            await Task.Delay(75);
            RaiseSmokeKey(window.SearchBox, Key.Escape);
            await WaitForSmokeConditionAsync(() => !window._isPasting);
            SmokeRequire(window._isClosed && SmokeClipboardSequence() == sequence,
                "关闭窗口后仍写入剪贴板或等待重试");
        }
        finally
        {
            if (clipboardHeld) SmokeCloseClipboard();
            if (!window._isClosed) window.Close();
            await WaitForSmokeConditionAsync(() => !window._isPasting);
            ClipboardStore.DeleteRecord(id);
        }
    }

    private static KeyEventArgs RaiseSmokeKey(
        FrameworkElement target, Key key, bool previewOnly = false)
    {
        var source = PresentationSource.FromVisual(target) ??
            throw new InvalidOperationException("快捷窗口尚未创建呈现源");
        var args = new KeyEventArgs(Keyboard.PrimaryDevice, source, Environment.TickCount, key)
        {
            RoutedEvent = Keyboard.PreviewKeyDownEvent
        };
        target.RaiseEvent(args);
        if (!args.Handled && !previewOnly)
        {
            args.RoutedEvent = Keyboard.KeyDownEvent;
            target.RaiseEvent(args);
        }
        return args;
    }

    private static Button FindSmokeButton(DependencyObject? parent, string name)
    {
        Button? Find(DependencyObject? element)
        {
            if (element is Button button && button.Name == name) return button;
            if (element == null) return null;
            for (var index = 0; index < VisualTreeHelper.GetChildrenCount(element); ++index)
            {
                var found = Find(VisualTreeHelper.GetChild(element, index));
                if (found != null) return found;
            }
            return null;
        }
        return Find(parent) ?? throw new InvalidOperationException("未找到按钮 " + name);
    }

    private static async Task WaitForSmokeConditionAsync(Func<bool> condition)
    {
        var stopwatch = Stopwatch.StartNew();
        while (!condition() && stopwatch.Elapsed < TimeSpan.FromSeconds(4))
            await Task.Delay(10);
        SmokeRequire(condition(), "快捷窗口操作未在限定时间内完成");
    }

    private static void RequireIsolatedSmokeHistory() => SmokeRequire(
        !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("CAISHEN_CLIPBOARD_DATA_DIR")),
        "快捷窗口测试必须使用隔离的复制记录目录");

    private static void SmokeRequire(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    [DllImport("user32.dll", EntryPoint = "OpenClipboard")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SmokeOpenClipboard(IntPtr owner);
    [DllImport("user32.dll", EntryPoint = "CloseClipboard")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SmokeCloseClipboard();
    [DllImport("user32.dll", EntryPoint = "GetClipboardSequenceNumber")]
    private static extern uint SmokeClipboardSequence();
}
