using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

namespace ShuruSettings;

// 文本使用即时渲染的 CF_UNICODETEXT，避免 OLE Flush 向旧剪贴板所有者请求数据而阻塞。
internal static class ClipboardTextWriter
{
    private const uint UnicodeTextFormat = 13;
    private const uint MovableZeroInitialized = 0x0042;
    private const int OpenRetryCount = 40;
    private const int OpenRetryDelayMilliseconds = 25;

    internal static void Write(string text, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(text);
        if (text.Contains('\0'))
            throw new ArgumentException("剪贴板文本不能包含空字符。", nameof(text));
        cancellationToken.ThrowIfCancellationRequested();

        var markerFormat = RegisterClipboardFormat(ClipboardImageService.InternalPasteFormat);
        if (markerFormat == 0) throw NativeFailure("注册剪贴板格式失败");

        // EmptyClipboard 需要非空所有者；消息窗口不激活，也不抢占目标输入框焦点。
        var owner = CreateWindowEx(0, "STATIC", null, 0, 0, 0, 0, 0,
            new IntPtr(-3), IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (owner == IntPtr.Zero) throw NativeFailure("创建剪贴板所有者窗口失败");

        var textMemory = IntPtr.Zero;
        var markerMemory = IntPtr.Zero;
        var opened = false;
        try
        {
            textMemory = AllocateText(text);
            markerMemory = AllocateText("1");
            for (var attempt = 0; ; ++attempt)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (OpenClipboard(owner))
                {
                    opened = true;
                    break;
                }
                if (attempt >= OpenRetryCount)
                    throw new COMException("剪贴板正被其它程序占用，请稍后重试。",
                        unchecked((int)0x800401D0));
                if (cancellationToken.WaitHandle.WaitOne(OpenRetryDelayMilliseconds))
                    cancellationToken.ThrowIfCancellationRequested();
            }

            cancellationToken.ThrowIfCancellationRequested();
            if (!EmptyClipboard()) throw NativeFailure("清空剪贴板失败");
            Publish(markerFormat, ref markerMemory);
            Publish(UnicodeTextFormat, ref textMemory);
        }
        finally
        {
            if (opened) CloseClipboard();
            if (textMemory != IntPtr.Zero) GlobalFree(textMemory);
            if (markerMemory != IntPtr.Zero) GlobalFree(markerMemory);
            DestroyWindow(owner);
        }
    }

    private static IntPtr AllocateText(string text)
    {
        var characters = (text + '\0').ToCharArray();
        var memory = GlobalAlloc(MovableZeroInitialized,
            new UIntPtr(checked((uint)characters.Length * sizeof(char))));
        if (memory == IntPtr.Zero) throw NativeFailure("分配剪贴板内存失败");
        var data = GlobalLock(memory);
        if (data == IntPtr.Zero)
        {
            var error = NativeFailure("锁定剪贴板内存失败");
            GlobalFree(memory);
            throw error;
        }
        try
        {
            Marshal.Copy(characters, 0, data, characters.Length);
        }
        finally
        {
            GlobalUnlock(memory);
        }
        return memory;
    }

    private static void Publish(uint format, ref IntPtr memory)
    {
        if (SetClipboardData(format, memory) == IntPtr.Zero)
            throw NativeFailure("写入剪贴板失败");
        // SetClipboardData 成功后内存由系统持有，辅助进程退出后仍可粘贴。
        memory = IntPtr.Zero;
    }

    private static Win32Exception NativeFailure(string message) =>
        new(Marshal.GetLastWin32Error(), message);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint RegisterClipboardFormat(string format);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(uint extendedStyle, string className,
        string? windowName, uint style, int x, int y, int width, int height,
        IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenClipboard(IntPtr owner);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseClipboard();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EmptyClipboard();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetClipboardData(uint format, IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalAlloc(uint flags, UIntPtr size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalLock(IntPtr memory);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalUnlock(IntPtr memory);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GlobalFree(IntPtr memory);
}
