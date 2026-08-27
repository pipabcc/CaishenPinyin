using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;

namespace ShuruSettings;

public partial class App : Application
{
    private const string ClipboardMonitorMutexName =
        "Local\\CaishenPinyinClipboardMonitorV2";

    private Mutex? clipboardMonitorMutex_;
    private bool ownsClipboardMonitor_;
    private UiRequestWatcher? uiRequestWatcher_;

    public App()
    {
        DispatcherUnhandledException += (s, args) =>
        {
            CrashLogger.Log("DispatcherUnhandledException", args.Exception);
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            CrashLogger.Log("AppDomain.UnhandledException", args.ExceptionObject);
        };
        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            CrashLogger.Log("TaskScheduler.UnobservedTaskException", args.Exception);
            args.SetObserved();
        };
    }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        // 沙箱宿主（开始菜单搜索框）自己改不了用户数据目录的 DACL，由这里
        // 与 IME 宿主共同兜底，确保皮肤、设置与统计在沙箱中同样可读。
        AppContainerAccess.EnsureUserData();

        TextPasteRequestStore.CleanupExpired();
        DirectTextCommitRequestStore.CleanupExpired();

        var normalizeSkin = ArgumentValue(e.Args, "-normalize-skin");
        if (!string.IsNullOrWhiteSpace(normalizeSkin))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var exitCode = 1;
            try
            {
                if (!string.Equals(Path.GetFileName(normalizeSkin), normalizeSkin,
                        StringComparison.Ordinal))
                    throw new InvalidDataException("皮肤标识无效。");
                var directory = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "CaishenPinyin", "skins", normalizeSkin);
                SsfConverter.NormalizeInstalledSkin(directory, normalizeSkin);
                exitCode = 0;
            }
            catch (Exception ex)
            {
                CrashLogger.Log("SsfConverter.NormalizeInstalledSkin", ex);
            }
            Shutdown(exitCode);
            return;
        }

        var pasteTextRequest = ArgumentValue(e.Args, "-paste-text-request");
        if (!string.IsNullOrWhiteSpace(pasteTextRequest))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var target = ParseWindowHandle(
                ArgumentValue(e.Args, "-target-hwnd"));
            _ = RunPasteTextRequestAndExitAsync(pasteTextRequest, target);
            return;
        }

        var pasteRecordId = ArgumentValue(e.Args, "-paste-record");
        if (!string.IsNullOrWhiteSpace(pasteRecordId))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            var target = ParseWindowHandle(
                ArgumentValue(e.Args, "-target-hwnd"));
            _ = RunPasteRecordAndExitAsync(pasteRecordId, target);
            return;
        }

        if (e.Args.Contains("-self-test"))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = RunUiSelfTestAndExitAsync();
            return;
        }

        if (e.Args.Contains("-clipboard-monitor"))
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            StartClipboardMonitorHost();
            return;
        }

        EnsureClipboardMonitorProcess();

        if (e.Args.Contains("-quick") || e.Args.Contains("-v") || e.Args.Contains("-vv"))
        {
            QuickWindowMode mode = QuickWindowMode.Clipboard;
            if (e.Args.Contains("phrases") || e.Args.Contains("-vv"))
            {
                mode = QuickWindowMode.CustomPhrases;
            }

            var directCommitToken = ArgumentValue(
                e.Args, "-direct-commit-token");
            if (!string.IsNullOrWhiteSpace(directCommitToken) &&
                !DirectTextCommitRequestStore.IsValidToken(directCommitToken))
            {
                CrashLogger.Log(
                    "DirectTextCommit.Startup", "invalid direct commit token");
                directCommitToken = null;
            }
            var target = ParseWindowHandle(
                ArgumentValue(e.Args, "-target-hwnd"));
            var quickWin = new QuickWindow(
                mode, directCommitToken, target);
            quickWin.Show();
            return;
        }

        var mainWin = new MainWindow();
        mainWin.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        uiRequestWatcher_?.Dispose();
        uiRequestWatcher_ = null;
        if (ownsClipboardMonitor_)
        {
            try
            {
                ClipboardMonitor.StopListening();
                clipboardMonitorMutex_?.ReleaseMutex();
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardMonitor.StopListening", ex);
            }
        }
        clipboardMonitorMutex_?.Dispose();
        clipboardMonitorMutex_ = null;
        base.OnExit(e);
    }

    private void StartClipboardMonitorHost()
    {
        try
        {
            clipboardMonitorMutex_ = new Mutex(
                initiallyOwned: true,
                ClipboardMonitorMutexName,
                out var createdNew);
            if (!createdNew)
            {
                clipboardMonitorMutex_.Dispose();
                clipboardMonitorMutex_ = null;
                Shutdown(0);
                return;
            }
            ownsClipboardMonitor_ = true;
            if (!ClipboardMonitor.StartListening())
                throw new InvalidOperationException("无法注册系统剪贴板监听器");
            // 沙箱宿主打不开设置窗口，由这个常驻进程代为响应其请求文件。
            uiRequestWatcher_ = new UiRequestWatcher();
            uiRequestWatcher_.Start();
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardMonitor.Host", ex);
            Shutdown(1);
        }
    }

    private static void EnsureClipboardMonitorProcess()
    {
        try
        {
            using var existing = Mutex.OpenExisting(ClipboardMonitorMutexName);
            return;
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            // 尚无监听进程，继续启动。
        }
        catch (UnauthorizedAccessException)
        {
            // 另一完整性级别的监听器已经存在，不重复启动。
            return;
        }

        try
        {
            var executable = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(executable)) return;
            Process.Start(new ProcessStartInfo
            {
                FileName = executable,
                Arguments = "-clipboard-monitor",
                WorkingDirectory = AppContext.BaseDirectory,
                UseShellExecute = false,
                CreateNoWindow = true
            });
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardMonitor.Launch", ex);
        }
    }

    private async Task RunPasteRecordAndExitAsync(
        string recordId,
        IntPtr targetWindow)
    {
        var exitCode = 1;
        try
        {
            if (await ClipboardPasteService.PasteRecordAsync(
                    recordId, targetWindow))
            {
                exitCode = 0;
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardPasteService.PasteRecord", ex);
        }
        Shutdown(exitCode);
    }

    private async Task RunPasteTextRequestAndExitAsync(
        string requestToken,
        IntPtr targetWindow)
    {
        var exitCode = 1;
        try
        {
            if (await ClipboardPasteService.PasteTextRequestAsync(
                    requestToken, targetWindow))
            {
                exitCode = 0;
            }
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ClipboardPasteService.PasteTextRequest", ex);
        }
        Shutdown(exitCode);
    }

    private async Task RunUiSelfTestAndExitAsync()
    {
        var exitCode = 1;
        MainWindow? window = null;
        try
        {
            if (!ClipboardMonitor.StartListening())
                throw new InvalidOperationException("剪贴板监听器注册失败");
            ClipboardMonitor.StopListening();
            window = new MainWindow();
            window.Show();
            await window.RunClipboardPageSmokeTestAsync();
            exitCode = 0;
        }
        catch (Exception ex)
        {
            CrashLogger.Log("SettingsUi.SelfTest", ex);
        }
        finally
        {
            window?.Close();
        }
        Shutdown(exitCode);
    }

    private static string? ArgumentValue(string[] arguments, string name)
    {
        for (var index = 0; index + 1 < arguments.Length; ++index)
        {
            if (string.Equals(arguments[index], name,
                    StringComparison.OrdinalIgnoreCase))
            {
                return arguments[index + 1];
            }
        }
        return null;
    }

    private static IntPtr ParseWindowHandle(string? value) =>
        long.TryParse(value, out var rawValue)
            ? new IntPtr(rawValue)
            : IntPtr.Zero;
}
