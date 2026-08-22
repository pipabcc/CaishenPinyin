using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;

namespace ShuruSettings;

// 开始菜单搜索框等 AppContainer 宿主被系统禁止启动包外进程，右键「输入法设置」
// 无法直接拉起本程序。沙箱侧改为把意图写成请求文件（见 ime_ui_logic.h 的
// WriteSettingsUiRequest），由常驻的剪贴板监听进程在这里代为打开窗口。
//
// 请求目录对所有沙箱应用可写，因此这里对文件名、头部与命令三重校验，且只接受
// 固定关键字——请求内容任何时候都不会被当作命令行参数使用。
internal sealed class UiRequestWatcher : IDisposable
{
    private const string Header = "CAISHEN_UI_REQUEST_V1";
    private const long MaximumRequestBytes = 4L * 1024;

    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    private FileSystemWatcher? watcher_;

    internal static string RequestDirectory =>
        Path.Combine(AppContainerAccess.UserDataRoot, "ui_requests");

    internal void Start()
    {
        try
        {
            Directory.CreateDirectory(RequestDirectory);
            CleanupExpired();
            watcher_ = new FileSystemWatcher(RequestDirectory, "*.txt")
            {
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite,
                IncludeSubdirectories = false,
            };
            watcher_.Created += OnRequestObserved;
            watcher_.Renamed += OnRequestObserved;
            watcher_.EnableRaisingEvents = true;
            // 监听器就绪前落地的请求不会触发事件，补扫一次。
            DrainPending();
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Start", ex);
        }
    }

    public void Dispose()
    {
        var watcher = watcher_;
        watcher_ = null;
        if (watcher == null) return;
        try
        {
            watcher.EnableRaisingEvents = false;
            watcher.Created -= OnRequestObserved;
            watcher.Renamed -= OnRequestObserved;
            watcher.Dispose();
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Dispose", ex);
        }
    }

    // 沙箱侧用 MoveFileEx 改名落地，事件类型取决于时序；一律重扫目录，
    // 顺带兜住 Filter 对改名事件匹配旧名/新名的差异。
    private void OnRequestObserved(object sender, FileSystemEventArgs e) =>
        DrainPending();

    private void DrainPending()
    {
        string[] files;
        try
        {
            if (!Directory.Exists(RequestDirectory)) return;
            files = Directory.GetFiles(
                RequestDirectory, "*.txt", SearchOption.TopDirectoryOnly);
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Enumerate", ex);
            return;
        }

        foreach (var path in files)
        {
            var command = ReadAndDelete(path);
            if (command == null) continue;
            var dispatcher = Application.Current?.Dispatcher;
            if (dispatcher == null) continue;
            dispatcher.BeginInvoke(new Action(() => Execute(command)));
        }
    }

    private static string? ReadAndDelete(string path)
    {
        // 只认 32 位十六进制文件名，挡住沙箱侧写入的任意名字。
        var name = Path.GetFileNameWithoutExtension(path);
        if (name.Length != 32 || !name.All(Uri.IsHexDigit)) return null;
        try
        {
            if (new FileInfo(path).Length > MaximumRequestBytes) return null;
            var lines = File.ReadAllLines(path, StrictUtf8);
            if (lines.Length < 2) return null;
            if (!string.Equals(lines[0], Header, StringComparison.Ordinal))
                return null;
            return NormalizeCommand(lines[1]);
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Read", ex);
            return null;
        }
        finally
        {
            try { File.Delete(path); }
            catch (Exception ex) { CrashLogger.Log("UiRequestWatcher.Delete", ex); }
        }
    }

    private static string? NormalizeCommand(string value) => value.Trim() switch
    {
        "settings" => "settings",
        "clipboard" => "clipboard",
        "phrases" => "phrases",
        _ => null,
    };

    private static void Execute(string command)
    {
        try
        {
            switch (command)
            {
                case "clipboard":
                    ShowQuickWindow(QuickWindowMode.Clipboard);
                    break;
                case "phrases":
                    ShowQuickWindow(QuickWindowMode.CustomPhrases);
                    break;
                default:
                    ShowMainWindow();
                    break;
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Execute", ex);
        }
    }

    private static void ShowMainWindow()
    {
        var existing = Application.Current?.Windows
            .OfType<MainWindow>().FirstOrDefault();
        if (existing != null)
        {
            BringToFront(existing);
            return;
        }
        var window = new MainWindow();
        window.Show();
        BringToFront(window);
    }

    private static void ShowQuickWindow(QuickWindowMode mode)
    {
        var window = new QuickWindow(mode);
        window.Show();
        BringToFront(window);
    }

    private static void BringToFront(Window window)
    {
        if (window.WindowState == WindowState.Minimized)
            window.WindowState = WindowState.Normal;
        window.Activate();
        // 请求来自另一个进程，前台权限不在本进程手上，靠 Topmost 抖动置顶。
        window.Topmost = true;
        window.Topmost = false;
        window.Focus();
    }

    private static void CleanupExpired()
    {
        try
        {
            if (!Directory.Exists(RequestDirectory)) return;
            var cutoff = DateTime.UtcNow.AddDays(-1);
            foreach (var path in Directory.EnumerateFiles(
                         RequestDirectory, "*", SearchOption.TopDirectoryOnly))
            {
                if (File.GetLastWriteTimeUtc(path) < cutoff) File.Delete(path);
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("UiRequestWatcher.Cleanup", ex);
        }
    }
}
