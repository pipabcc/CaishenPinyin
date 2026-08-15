using System;
using System.Collections.Specialized;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using System.Windows;
using System.Windows.Interop;

namespace ShuruSettings
{
    public static class ClipboardMonitor
    {
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AddClipboardFormatListener(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool RemoveClipboardFormatListener(IntPtr hwnd);

        private const int WM_CLIPBOARDUPDATE = 0x031D;
        private static HwndSource? _hwndSource;
        private static bool _isListening = false;
        private static int _updateQueued;
        private static int _updateRevision;
        private static readonly object ListenerLock = new object();

        public static event Action? ClipboardUpdated;

        public static bool StartListening()
        {
            lock (ListenerLock)
            {
                if (_isListening) return true;

                try
                {
                    var parameters = new HwndSourceParameters("CaishenClipboardListener")
                    {
                        Width = 0,
                        Height = 0,
                        PositionX = 0,
                        PositionY = 0,
                        WindowStyle = 0
                    };
                    _hwndSource = new HwndSource(parameters);
                    _hwndSource.AddHook(WndProc);

                    if (AddClipboardFormatListener(_hwndSource.Handle))
                    {
                        _isListening = true;
                        return true;
                    }
                    _hwndSource.RemoveHook(WndProc);
                    _hwndSource.Dispose();
                    _hwndSource = null;
                }
                catch (Exception ex)
                {
                    CrashLogger.Log("ClipboardMonitor.StartListening", ex);
                }
                return false;
            }
        }

        public static void StopListening()
        {
            lock (ListenerLock)
            {
                try
                {
                    if (_hwndSource != null && _hwndSource.Handle != IntPtr.Zero)
                    {
                        if (_isListening)
                            RemoveClipboardFormatListener(_hwndSource.Handle);
                        _hwndSource.RemoveHook(WndProc);
                        _hwndSource.Dispose();
                        _hwndSource = null;
                    }
                }
                catch (Exception ex)
                {
                    CrashLogger.Log("ClipboardMonitor.StopListening", ex);
                }
                finally
                {
                    _isListening = false;
                }
            }
        }

        private static IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == WM_CLIPBOARDUPDATE)
            {
                Interlocked.Increment(ref _updateRevision);
                QueueClipboardRead();
                handled = true;
            }
            return IntPtr.Zero;
        }

        private static void QueueClipboardRead()
        {
            if (Interlocked.CompareExchange(ref _updateQueued, 1, 0) != 0)
                return;

            // 延时合并同一次复制产生的重复通知；若等待或读取期间又有新通知，
            // 当前轮完成后会自动补读最新剪贴板，不会永久丢失最后一次复制。
            ThreadPool.QueueUserWorkItem(_ =>
            {
                try
                {
                    Thread.Sleep(50);
                    var dispatcher = Application.Current?.Dispatcher;
                    if (dispatcher == null)
                    {
                        CompleteClipboardRead(
                            Volatile.Read(ref _updateRevision));
                        return;
                    }
                    dispatcher.BeginInvoke(new Action(() =>
                    {
                        var observedRevision = Volatile.Read(
                            ref _updateRevision);
                        try { OnClipboardChangedSafely(); }
                        finally { CompleteClipboardRead(observedRevision); }
                    }));
                }
                catch (Exception ex)
                {
                    CompleteClipboardRead(
                        Volatile.Read(ref _updateRevision));
                    CrashLogger.Log("ClipboardMonitor.ThreadPool", ex);
                }
            });
        }

        private static void CompleteClipboardRead(int observedRevision)
        {
            Interlocked.Exchange(ref _updateQueued, 0);
            if (Volatile.Read(ref _updateRevision) != observedRevision)
                QueueClipboardRead();
        }

        private static void OnClipboardChangedSafely()
        {
            for (int retry = 0; retry < 3; ++retry)
            {
                try
                {
                    var config = ClipboardStore.LoadConfig();
                    if (!config.Enabled) return;

                    var dataObject = Clipboard.GetDataObject();
                    if (dataObject != null && ProcessDataObject(dataObject))
                        ClipboardUpdated?.Invoke();
                    break;
                }
                catch (COMException)
                {
                    Thread.Sleep(40);
                }
                catch (Exception ex)
                {
                    CrashLogger.Log("ClipboardMonitor.ReadClipboard", ex);
                    break;
                }
            }
        }

        internal static bool ProcessDataObject(IDataObject dataObject)
        {
            ArgumentNullException.ThrowIfNull(dataObject);
            if (ClipboardImageService.IsInternalPaste(dataObject)) return false;

            // 浏览器复制图片时通常同时携带文本或 HTML。图片格式优先，避免
            // 本应记录图片的操作被降级成链接文本。
            if (ClipboardImageService.TryReadImage(dataObject, out var pngBytes))
            {
                var path = ClipboardStore.SaveImageToDisk(pngBytes);
                if (string.IsNullOrEmpty(path)) return false;
                ClipboardStore.AddRecord(new ClipboardRecord
                {
                    Type = ClipboardItemType.Image,
                    Content = "image:" + Convert.ToHexString(
                        SHA256.HashData(pngBytes)),
                    DisplayTitle = $"[图片] {Path.GetFileName(path)}",
                    ImagePath = path,
                    CreatedTime = DateTime.Now
                });
                return true;
            }

            if (dataObject.GetDataPresent(DataFormats.UnicodeText, autoConvert: true) &&
                dataObject.GetData(DataFormats.UnicodeText, autoConvert: true) is
                    string text &&
                !string.IsNullOrWhiteSpace(text))
            {
                var title = text.Trim();
                if (title.Length > 80) title = title[..80] + "...";
                ClipboardStore.AddRecord(new ClipboardRecord
                {
                    Type = ClipboardItemType.Text,
                    Content = text,
                    DisplayTitle = title,
                    CreatedTime = DateTime.Now
                });
                return true;
            }

            if (!dataObject.GetDataPresent(DataFormats.FileDrop, autoConvert: true))
                return false;
            var files = ReadFileDropList(
                dataObject.GetData(DataFormats.FileDrop, autoConvert: true));
            if (files.Length == 0) return false;
            ClipboardStore.AddRecord(new ClipboardRecord
            {
                Type = ClipboardItemType.File,
                Content = string.Join("\n", files),
                DisplayTitle = files.Length == 1
                    ? files[0]
                    : $"{files[0]} 等 {files.Length} 个文件",
                CreatedTime = DateTime.Now
            });
            return true;
        }

        private static string[] ReadFileDropList(object? value)
        {
            if (value is string[] paths) return paths;
            if (value is not StringCollection collection) return Array.Empty<string>();
            var result = new string[collection.Count];
            collection.CopyTo(result, 0);
            return result;
        }
    }
}
