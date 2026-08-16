using System;
using System.IO;
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

    private const long MaximumDecodedPixels = 100_000_000;

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

        var bitmap = DecodeBitmap(normalized.PngBytes);
        var dataObject = new DataObject();
        dataObject.SetData(InternalPasteFormat, "1");
        dataObject.SetData(
            NativePngFormat,
            new MemoryStream(normalized.PngBytes, writable: false));
        dataObject.SetImage(bitmap);

        var fileDropList = new System.Collections.Specialized.StringCollection { fullPath };
        dataObject.SetFileDropList(fileDropList);

        for (var retry = 0; retry < 5; retry++)
        {
            try
            {
                Clipboard.SetDataObject(dataObject, copy: true);
                break;
            }
            catch
            {
                if (retry == 4) throw;
                Thread.Sleep(30);
            }
        }
    }

    internal static void SetClipboardText(string text)
    {
        var dataObject = new DataObject();
        dataObject.SetData(InternalPasteFormat, "1");
        dataObject.SetData(DataFormats.UnicodeText, text ?? string.Empty);

        for (var retry = 0; retry < 5; retry++)
        {
            try
            {
                Clipboard.SetDataObject(dataObject, copy: true);
                break;
            }
            catch
            {
                if (retry == 4) throw;
                Thread.Sleep(30);
            }
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
