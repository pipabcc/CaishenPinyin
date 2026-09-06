using System;
using System.Collections.Specialized;
using System.IO;
using System.Security.Cryptography;
using System.Windows;

namespace ShuruSettings
{
    public static class ClipboardMonitor
    {
        private static volatile ClipboardCaptureWorker? _worker;
        private static readonly object ListenerLock = new object();

        public static event Action? ClipboardUpdated;

        public static bool StartListening()
        {
            lock (ListenerLock)
            {
                if (_worker?.IsRunning == true) return true;

                try
                {
                    _worker?.Dispose();
                    ClipboardCaptureWorker? worker = null;
                    worker = new ClipboardCaptureWorker(() => PublishUpdated(worker));
                    if (!worker.Start()) return false;
                    _worker = worker;
                    return true;
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
            ClipboardCaptureWorker? worker;
            lock (ListenerLock)
            {
                worker = _worker;
                _worker = null;
            }
            worker?.Dispose();
        }

        private static void PublishUpdated(ClipboardCaptureWorker? worker)
        {
            void Notify()
            {
                if (worker?.IsRunning == true && ReferenceEquals(worker, _worker))
                    ClipboardUpdated?.Invoke();
            }
            var dispatcher = Application.Current?.Dispatcher;
            if (dispatcher == null) Notify();
            else if (!dispatcher.HasShutdownStarted) dispatcher.BeginInvoke((Action)Notify);
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
