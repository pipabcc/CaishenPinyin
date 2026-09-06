using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace ShuruSettings;

// OLE 数据对象与隐藏窗口都留在同一个 STA；编码、散列和入库不占用界面线程。
internal sealed class ClipboardCaptureWorker : IDisposable
{
    private const int ClipboardUpdateMessage = 0x031D;
    private readonly bool _listenToSystemClipboard;
    private readonly Action _onUpdated;
    private readonly Thread _thread;
    private readonly TaskCompletionSource<bool> _ready =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private Dispatcher? _dispatcher;
    private HwndSource? _window;
    private DispatcherTimer? _timer;
    private int _stopping;
    private bool _reading;
    private bool _readAgain;

    internal ClipboardCaptureWorker(Action onUpdated, bool listenToSystemClipboard = true)
    {
        _onUpdated = onUpdated;
        _listenToSystemClipboard = listenToSystemClipboard;
        _thread = new Thread(Run) { IsBackground = true, Name = "财神剪贴板采集" };
        _thread.SetApartmentState(ApartmentState.STA);
    }

    internal bool IsRunning => Volatile.Read(ref _stopping) == 0 && _thread.IsAlive;
    internal TimeSpan LastCaptureDuration { get; private set; }
    internal int CaptureThreadId => _thread.ManagedThreadId;

    internal bool Start()
    {
        _thread.Start();
        if (_ready.Task.Wait(TimeSpan.FromSeconds(2)) && _ready.Task.Result) return true;
        Dispose();
        return false;
    }

    internal Task<bool> CaptureAsync(Func<IDataObject?> readData)
    {
        var dispatcher = _dispatcher;
        if (!IsRunning || dispatcher == null || dispatcher.HasShutdownStarted)
            return Task.FromException<bool>(new ObjectDisposedException(nameof(ClipboardCaptureWorker)));
        return dispatcher.InvokeAsync(() => Capture(readData), DispatcherPriority.Background).Task;
    }

    private void Run()
    {
        try
        {
            _dispatcher = Dispatcher.CurrentDispatcher;
            if (Volatile.Read(ref _stopping) != 0) return;
            _timer = new DispatcherTimer(DispatcherPriority.Background, _dispatcher)
            {
                Interval = TimeSpan.FromMilliseconds(50)
            };
            _timer.Tick += ReadClipboard;
            if (_listenToSystemClipboard)
            {
                _window = new HwndSource(new HwndSourceParameters("CaishenClipboardListener")
                {
                    Width = 0, Height = 0, WindowStyle = 0
                });
                _window.AddHook(WindowProcedure);
                if (!AddClipboardFormatListener(_window.Handle))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            _ready.TrySetResult(true);
            if (Volatile.Read(ref _stopping) == 0) Dispatcher.Run();
        }
        catch (Exception error)
        {
            CrashLogger.Log("ClipboardCaptureWorker.Run", error);
        }
        finally
        {
            Interlocked.Exchange(ref _stopping, 1);
            _ready.TrySetResult(false);
            _timer?.Stop();
            if (_window != null)
            {
                if (_listenToSystemClipboard) RemoveClipboardFormatListener(_window.Handle);
                _window.RemoveHook(WindowProcedure);
                _window.Dispose();
            }
        }
    }

    private IntPtr WindowProcedure(
        IntPtr window, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message == ClipboardUpdateMessage && Volatile.Read(ref _stopping) == 0)
        {
            if (_reading) _readAgain = true;
            else
            {
                _timer!.Stop();
                _timer.Start();
            }
            handled = true;
        }
        return IntPtr.Zero;
    }

    private void ReadClipboard(object? sender, EventArgs args)
    {
        _timer!.Stop();
        if (Volatile.Read(ref _stopping) != 0) return;
        _reading = true;
        _readAgain = false;
        try
        {
            for (var attempt = 0; attempt < 3 && Volatile.Read(ref _stopping) == 0; ++attempt)
            {
                try
                {
                    Capture(Clipboard.GetDataObject);
                    return;
                }
                catch (COMException error)
                {
                    if (attempt == 2) CrashLogger.Log("ClipboardCaptureWorker.Read", error);
                    else Thread.Sleep(40);
                }
            }
        }
        catch (Exception error)
        {
            CrashLogger.Log("ClipboardCaptureWorker.Read", error);
        }
        finally
        {
            _reading = false;
            // OLE 读取可以重入窗口过程；读取期间的新通知需要补读最新内容。
            if (_readAgain && Volatile.Read(ref _stopping) == 0) _timer.Start();
        }
    }

    private bool Capture(Func<IDataObject?> readData)
    {
        if (Volatile.Read(ref _stopping) != 0 || !ClipboardStore.LoadConfig().Enabled)
            return false;
        var started = Stopwatch.GetTimestamp();
        try
        {
            var data = readData();
            if (data == null || Volatile.Read(ref _stopping) != 0) return false;
            var updated = ClipboardMonitor.ProcessDataObject(data);
            if (updated && Volatile.Read(ref _stopping) == 0) _onUpdated();
            return updated;
        }
        finally
        {
            LastCaptureDuration = Stopwatch.GetElapsedTime(started);
        }
    }

    public void Dispose()
    {
        Interlocked.Exchange(ref _stopping, 1);
        var dispatcher = _dispatcher;
        if (dispatcher != null && !dispatcher.HasShutdownStarted)
            dispatcher.BeginInvokeShutdown(DispatcherPriority.Send);
        if (_thread.IsAlive && Thread.CurrentThread != _thread &&
            !_thread.Join(TimeSpan.FromSeconds(2)))
            CrashLogger.Log("ClipboardCaptureWorker.Stop", "采集线程退出等待超时");
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AddClipboardFormatListener(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveClipboardFormatListener(IntPtr window);
}
