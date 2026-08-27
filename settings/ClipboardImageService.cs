using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ShuruSettings;

internal readonly record struct NormalizedClipboardImage(
    byte[] PngBytes,
    bool AlphaRepaired);

internal static class ClipboardImageService
{
    internal const string NativePngFormat = "PNG";
    internal const string InternalPasteFormat =
        "CaishenPinyin.InternalClipboardPaste";

    private const int ClipboardCannotOpenHResult = unchecked((int)0x800401D0);
    private const long MaximumDecodedPixels = 100_000_000;
    // 剪贴板是全系统单例资源，其它进程（含本程序自己的监视器在哈希/入库
    // 大图片时）可能短暂持有；总重试窗口约 2.5 秒，覆盖常见竞争。
    // 总重试窗口约 7 秒：覆盖常见竞争以及 GameViewer 之类会长时间
    // 持有剪贴板的第三方工具的间歇性占用。
    private static readonly int[] ClipboardRetryDelaysMilliseconds =
        [50, 100, 200, 300, 500, 500, 750, 750, 1000, 1000, 1000, 1000];

    internal static bool IsInternalPaste(IDataObject dataObject) =>
        dataObject.GetDataPresent(InternalPasteFormat, autoConvert: false);

    internal static bool TryReadImage(
        IDataObject dataObject,
        out byte[] normalizedPng)
    {
        normalizedPng = Array.Empty<byte>();

        if (dataObject.GetDataPresent(NativePngFormat, autoConvert: false))
        {
            var payload = ReadBinaryPayload(
                dataObject.GetData(NativePngFormat, autoConvert: false));
            if (payload is { Length: > 0 })
            {
                try
                {
                    normalizedPng = NormalizePngAlpha(payload).PngBytes;
                    return true;
                }
                catch (Exception ex)
                {
                    CrashLogger.Log("ClipboardImageService.NativePng", ex);
                }
            }
        }

        if (!dataObject.GetDataPresent(DataFormats.Bitmap, autoConvert: true))
            return false;
        if (dataObject.GetData(DataFormats.Bitmap, autoConvert: true) is not
            BitmapSource bitmap)
        {
            return false;
        }

        normalizedPng = EncodePng(NormalizeBitmapAlpha(bitmap));
        return normalizedPng.Length > 0;
    }

    internal static NormalizedClipboardImage NormalizePngAlpha(byte[] pngBytes)
    {
        ArgumentNullException.ThrowIfNull(pngBytes);
        if (pngBytes.Length == 0)
            throw new InvalidDataException("PNG 图片为空");

        var bitmap = DecodeBitmap(pngBytes);
        var normalized = NormalizeBitmapAlpha(bitmap, out var alphaRepaired);
        return alphaRepaired
            ? new NormalizedClipboardImage(EncodePng(normalized), true)
            : new NormalizedClipboardImage((byte[])pngBytes.Clone(), false);
    }

    internal static void SetClipboardImage(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath) || !File.Exists(imagePath))
            throw new FileNotFoundException("剪贴板图片文件不存在", imagePath);

        var fullPath = Path.GetFullPath(imagePath);
        var original = File.ReadAllBytes(fullPath);
        var normalized = NormalizePngAlpha(original);
        if (normalized.AlphaRepaired)
            RepairImageFileAtomically(fullPath, normalized.PngBytes);

        SetClipboardDataObject(CreateClipboardImageDataObject(normalized.PngBytes));
    }

    internal static DataObject CreateClipboardImageDataObject(byte[] pngBytes)
    {
        ArgumentNullException.ThrowIfNull(pngBytes);
        if (pngBytes.Length == 0)
            throw new InvalidDataException("PNG 图片为空");

        var bitmap = DecodeBitmap(pngBytes);
        var dataObject = new DataObject();
        dataObject.SetData(InternalPasteFormat, "1");
        dataObject.SetData(
            NativePngFormat,
            new MemoryStream((byte[])pngBytes.Clone(), writable: false));
        dataObject.SetImage(bitmap);
        return dataObject;
    }

    internal static void SetClipboardText(string text)
    {
        var dataObject = new DataObject();
        dataObject.SetData(InternalPasteFormat, "1");
        dataObject.SetData(DataFormats.UnicodeText, text ?? string.Empty);

        SetClipboardDataObject(dataObject);
    }

    // OLE 剪贴板要求 STA；用专用 STA 线程执行带重试的写入，让调用方
    // （QuickWindow 的 UI 线程）在长达数秒的重试窗口内保持响应。
    internal static Task SetClipboardTextAsync(string text)
    {
        var dataObject = new DataObject();
        dataObject.SetData(InternalPasteFormat, "1");
        dataObject.SetData(DataFormats.UnicodeText, text ?? string.Empty);
        return SetClipboardDataObjectAsync(dataObject);
    }

    internal static Task SetClipboardImageAsync(string imagePath)
    {
        if (string.IsNullOrWhiteSpace(imagePath) || !File.Exists(imagePath))
            throw new FileNotFoundException("剪贴板图片文件不存在", imagePath);

        var fullPath = Path.GetFullPath(imagePath);
        var original = File.ReadAllBytes(fullPath);
        var normalized = NormalizePngAlpha(original);
        if (normalized.AlphaRepaired)
            RepairImageFileAtomically(fullPath, normalized.PngBytes);
        return SetClipboardDataObjectAsync(
            CreateClipboardImageDataObject(normalized.PngBytes));
    }

    internal static Task SetClipboardDataObjectAsync(DataObject dataObject)
    {
        var completion = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                SetClipboardDataObject(dataObject);
                completion.TrySetResult(true);
            }
            catch (Exception ex)
            {
                completion.TrySetException(ex);
            }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        return completion.Task;
    }

    private static void SetClipboardDataObject(DataObject dataObject)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                Clipboard.SetDataObject(dataObject, copy: true);
                return;
            }
            catch (COMException ex) when (
                ex.HResult == ClipboardCannotOpenHResult)
            {
                if (attempt >= ClipboardRetryDelaysMilliseconds.Length)
                {
                    // 竞争持续超过整个重试窗口：记录当前占用者，供定位
                    // 是哪个进程长期锁死剪贴板。
                    CrashLogger.Log(
                        "ClipboardImageService.SetClipboardDataObject",
                        new InvalidOperationException(
                            "clipboard locked after retries; holder=" +
                            DescribeClipboardHolder(), ex));
                    throw;
                }
                Thread.Sleep(ClipboardRetryDelaysMilliseconds[attempt]);
            }
        }
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetOpenClipboardWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr hWnd, out uint processId);

    internal static string DescribeClipboardHolder()
    {
        try
        {
            var window = GetOpenClipboardWindow();
            if (window == IntPtr.Zero) return "none";
            GetWindowThreadProcessId(window, out var processId);
            var name = "<unknown>";
            try
            {
                name = Process.GetProcessById((int)processId).ProcessName;
            }
            catch
            {
                // 进程可能恰好退出；保留 pid 即可。
            }
            return $"window=0x{window.ToInt64():X}; pid={processId}; process={name}";
        }
        catch (Exception ex)
        {
            return "diagnostic-failed: " + ex.Message;
        }
    }

    private static BitmapSource NormalizeBitmapAlpha(BitmapSource bitmap) =>
        NormalizeBitmapAlpha(bitmap, out _);

    private static BitmapSource NormalizeBitmapAlpha(
        BitmapSource bitmap,
        out bool alphaRepaired)
    {
        ArgumentNullException.ThrowIfNull(bitmap);
        ValidateDimensions(bitmap.PixelWidth, bitmap.PixelHeight);

        BitmapSource bgra = bitmap.Format == PixelFormats.Bgra32
            ? bitmap
            : new FormatConvertedBitmap(
                bitmap, PixelFormats.Bgra32, destinationPalette: null, alphaThreshold: 0);
        var stride = checked(bgra.PixelWidth * 4);
        var pixels = new byte[checked(stride * bgra.PixelHeight)];
        bgra.CopyPixels(pixels, stride, 0);

        var anyAlpha = false;
        var anyRgb = false;
        for (var offset = 0; offset < pixels.Length; offset += 4)
        {
            anyAlpha |= pixels[offset + 3] != 0;
            anyRgb |= pixels[offset] != 0 ||
                      pixels[offset + 1] != 0 ||
                      pixels[offset + 2] != 0;
        }

        alphaRepaired = !anyAlpha && anyRgb;
        if (!alphaRepaired)
        {
            if (bgra.CanFreeze) bgra.Freeze();
            return bgra;
        }

        for (var offset = 3; offset < pixels.Length; offset += 4)
            pixels[offset] = byte.MaxValue;
        var repaired = BitmapSource.Create(
            bgra.PixelWidth,
            bgra.PixelHeight,
            PositiveDpi(bgra.DpiX),
            PositiveDpi(bgra.DpiY),
            PixelFormats.Bgra32,
            palette: null,
            pixels,
            stride);
        repaired.Freeze();
        return repaired;
    }

    private static BitmapSource DecodeBitmap(byte[] pngBytes)
    {
        using var stream = new MemoryStream(pngBytes, writable: false);
        var decoder = BitmapDecoder.Create(
            stream,
            BitmapCreateOptions.PreservePixelFormat,
            BitmapCacheOption.OnLoad);
        if (decoder.Frames.Count == 0)
            throw new InvalidDataException("PNG 图片没有可解码帧");
        var frame = decoder.Frames[0];
        ValidateDimensions(frame.PixelWidth, frame.PixelHeight);
        frame.Freeze();
        return frame;
    }

    private static byte[] EncodePng(BitmapSource bitmap)
    {
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        return stream.ToArray();
    }

    private static byte[]? ReadBinaryPayload(object? payload)
    {
        if (payload is byte[] bytes) return (byte[])bytes.Clone();
        if (payload is not Stream stream) return null;

        var originalPosition = stream.CanSeek ? stream.Position : 0;
        try
        {
            if (stream.CanSeek) stream.Position = 0;
            using var copy = new MemoryStream();
            stream.CopyTo(copy);
            return copy.ToArray();
        }
        finally
        {
            if (stream.CanSeek) stream.Position = originalPosition;
        }
    }

    private static void ValidateDimensions(int width, int height)
    {
        if (width <= 0 || height <= 0 ||
            (long)width * height > MaximumDecodedPixels)
        {
            throw new InvalidDataException(
                $"图片尺寸无效或过大：{width}x{height}");
        }
    }

    private static double PositiveDpi(double value) => value > 0 ? value : 96;

    private static void RepairImageFileAtomically(string path, byte[] bytes)
    {
        var temporary = path + $".{Environment.ProcessId}.repair.tmp";
        try
        {
            using (var stream = new FileStream(
                temporary,
                FileMode.Create,
                FileAccess.Write,
                FileShare.None,
                4096,
                FileOptions.WriteThrough))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            try
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("ClipboardImageService.RepairCleanup", ex);
            }
        }
    }
}
