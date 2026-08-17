using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace ShuruSettings;

internal static class ClipboardPasteService
{
    private const uint AncestorRoot = 2;
    private const uint InputKeyboard = 1;
    private const uint PasteMessage = 0x0302;
    private const uint SendMessageAbortIfHung = 0x0002;
    private const ushort VirtualKeyControl = 0x11;
    private const ushort VirtualKeyV = 0x56;
    private const uint KeyEventKeyUp = 0x0002;
    private const int ShowRestore = 9;

    internal static async Task<bool> PasteRecordAsync(
        string recordId,
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        LogStage("PasteRecord.Lookup", $"record={recordId}");
        var record = ClipboardStore.FindRecord(recordId);
        if (record == null)
        {
            LogStage("PasteRecord.Lookup", $"record={recordId}; result=missing");
            return false;
        }
        if (!IsValidTargetWindow(targetWindow, "PasteRecord")) return false;

        if (record.IsImage)
        {
            if (string.IsNullOrWhiteSpace(record.ImagePath) ||
                !File.Exists(record.ImagePath))
            {
                LogStage("PasteRecord.Image",
                    $"record={recordId}; result=missing-file");
                throw new FileNotFoundException(
                    "剪贴板图片文件不存在", record.ImagePath);
            }
            LogStage("PasteRecord.Clipboard",
                $"record={recordId}; type=image; stage=write");
            ClipboardImageService.SetClipboardImage(record.ImagePath);
        }
        else
        {
            LogStage("PasteRecord.Clipboard",
                $"record={recordId}; type=text; stage=write");
            ClipboardImageService.SetClipboardText(record.Content);
        }
        LogStage("PasteRecord.Clipboard",
            $"record={recordId}; result=written");

        return await PasteCurrentClipboardCoreAsync(
            targetWindow, "PasteRecord", cancellationToken).ConfigureAwait(true);
    }

    internal static async Task<bool> PasteTextRequestAsync(
        string requestToken,
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        if (!IsValidTargetWindow(targetWindow, "PasteTextRequest")) return false;
        var text = TextPasteRequestStore.ReadAndDelete(requestToken);
        ClipboardImageService.SetClipboardText(text);
        return await PasteCurrentClipboardCoreAsync(
            targetWindow, "PasteTextRequest", cancellationToken).ConfigureAwait(true);
    }

    internal static async Task<bool> PasteCurrentClipboardAsync(
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        if (!IsValidTargetWindow(targetWindow, "CurrentClipboard")) return false;
        return await PasteCurrentClipboardCoreAsync(
            targetWindow, "CurrentClipboard", cancellationToken).ConfigureAwait(true);
    }

    private static async Task<bool> PasteCurrentClipboardCoreAsync(
        IntPtr targetWindow,
        string operation,
        CancellationToken cancellationToken)
    {
        await Task.Delay(80, cancellationToken).ConfigureAwait(true);
        if (!IsValidTargetWindow(targetWindow, operation)) return false;

        var rootWindow = GetAncestor(targetWindow, AncestorRoot);
        var activateTarget = rootWindow != IntPtr.Zero ? rootWindow : targetWindow;
        LogStage(operation + ".Target",
            $"target={FormatWindow(targetWindow)}; root={FormatWindow(activateTarget)}");

        if (IsIconic(activateTarget))
        {
            ShowWindow(activateTarget, ShowRestore);
            LogStage(operation + ".Foreground", "stage=restore");
        }
        if (!IsForegroundRoot(activateTarget))
        {
            RequestForegroundWindow(activateTarget, operation);
            await Task.Delay(60, cancellationToken).ConfigureAwait(true);
        }
        if (!IsForegroundRoot(activateTarget))
        {
            RequestForegroundWindow(activateTarget, operation);
            await Task.Delay(60, cancellationToken).ConfigureAwait(true);
        }
        if (!IsForegroundRoot(activateTarget))
        {
            LogStage(operation + ".Foreground", "result=failed");
            return false;
        }
        LogStage(operation + ".Foreground", "result=active");

        await Task.Delay(40, cancellationToken).ConfigureAwait(true);
        if (!IsForegroundRoot(activateTarget))
        {
            LogStage(operation + ".Foreground", "result=lost-before-input");
            return false;
        }
        return FocusTargetAndSendPaste(
            targetWindow, activateTarget, operation);
    }

    private static void RequestForegroundWindow(
        IntPtr rootWindow,
        string operation)
    {
        var currentThread = GetCurrentThreadId();
        var targetThread = GetWindowThreadProcessId(rootWindow, out _);
        var foregroundWindow = GetForegroundWindow();
        var foregroundThread = foregroundWindow == IntPtr.Zero
            ? 0
            : GetWindowThreadProcessId(foregroundWindow, out _);
        var attachedForeground = false;
        var attachedTarget = false;
        try
        {
            if (foregroundThread != 0 && foregroundThread != currentThread)
            {
                attachedForeground = AttachThreadInput(
                    currentThread, foregroundThread, true);
            }
            if (targetThread != 0 && targetThread != currentThread &&
                targetThread != foregroundThread)
            {
                attachedTarget = AttachThreadInput(
                    currentThread, targetThread, true);
            }

            BringWindowToTop(rootWindow);
            var accepted = SetForegroundWindow(rootWindow);
            SetActiveWindow(rootWindow);
            LogStage(operation + ".Foreground",
                $"stage=request; accepted={accepted}; " +
                $"attachedForeground={attachedForeground}; " +
                $"attachedTarget={attachedTarget}");
        }
        finally
        {
            if (attachedTarget)
                AttachThreadInput(currentThread, targetThread, false);
            if (attachedForeground)
                AttachThreadInput(currentThread, foregroundThread, false);
        }
    }

    private static bool FocusTargetAndSendPaste(
        IntPtr targetWindow,
        IntPtr rootWindow,
        string operation)
    {
        var targetThread = GetWindowThreadProcessId(targetWindow, out _);
        if (targetThread == 0)
        {
            LogStage(operation + ".Focus", "result=no-target-thread");
            return false;
        }

        var currentThread = GetCurrentThreadId();
        var attached = false;
        try
        {
            if (currentThread != targetThread)
            {
                attached = AttachThreadInput(currentThread, targetThread, true);
                if (!attached)
                {
                    LogStage(operation + ".Focus",
                        $"result=attach-failed; error={Marshal.GetLastWin32Error()}");
                    return false;
                }
            }

            var focusTarget = TargetThreadFocus(targetThread, rootWindow);
            if (focusTarget == IntPtr.Zero)
                focusTarget = IsWindowWithinRoot(targetWindow, rootWindow)
                    ? targetWindow
                    : rootWindow;
            SetFocus(focusTarget);

            var actualFocus = GetFocus();
            if (!IsWindowWithinRoot(actualFocus, rootWindow))
            {
                var threadFocus = TargetThreadFocus(targetThread, rootWindow);
                if (threadFocus == IntPtr.Zero)
                {
                    LogStage(operation + ".Focus",
                        $"result=failed; requested={FormatWindow(focusTarget)}");
                    return false;
                }
                actualFocus = threadFocus;
            }
            LogStage(operation + ".Focus",
                $"result=ready; window={FormatWindow(actualFocus)}");
            if (!IsForegroundRoot(rootWindow))
            {
                LogStage(operation + ".Foreground",
                    "result=lost-during-focus");
                return false;
            }

            if (SupportsDirectPasteMessage(actualFocus))
            {
                var delivered = SendMessageTimeout(
                    actualFocus,
                    PasteMessage,
                    UIntPtr.Zero,
                    IntPtr.Zero,
                    SendMessageAbortIfHung,
                    2000,
                    out _);
                if (delivered == IntPtr.Zero)
                {
                    LogStage(operation + ".PasteMessage",
                        $"result=failed; target={FormatWindow(actualFocus)}; " +
                        $"error={Marshal.GetLastWin32Error()}");
                    return false;
                }
                LogStage(operation + ".PasteMessage",
                    $"result=sent; target={FormatWindow(actualFocus)}");
                return true;
            }

            var inputs = new[]
            {
                KeyboardInput(0x10, KeyEventKeyUp), // Shift Up
                KeyboardInput(0x12, KeyEventKeyUp), // Alt Up
                KeyboardInput(VirtualKeyControl, 0),
                KeyboardInput(VirtualKeyV, 0),
                KeyboardInput(VirtualKeyV, KeyEventKeyUp),
                KeyboardInput(VirtualKeyControl, KeyEventKeyUp)
            };
            var sent = SendInput(
                (uint)inputs.Length,
                inputs,
                Marshal.SizeOf<Input>());
            if (sent != inputs.Length)
            {
                LogStage(operation + ".SendInput",
                    $"result=failed; sent={sent}; expected={inputs.Length}; " +
                    $"error={Marshal.GetLastWin32Error()}");
                return false;
            }
            LogStage(operation + ".SendInput",
                $"result=sent; count={sent}; target={FormatWindow(actualFocus)}");
            return true;
        }
        finally
        {
            if (attached)
                AttachThreadInput(currentThread, targetThread, false);
        }
    }

    private static IntPtr TargetThreadFocus(uint threadId, IntPtr rootWindow)
    {
        var info = new GuiThreadInfo
        {
            Size = (uint)Marshal.SizeOf<GuiThreadInfo>()
        };
        return GetGUIThreadInfo(threadId, ref info) &&
               IsWindowWithinRoot(info.FocusWindow, rootWindow)
            ? info.FocusWindow
            : IntPtr.Zero;
    }

    private static bool SupportsDirectPasteMessage(IntPtr window)
    {
        var className = new StringBuilder(256);
        if (GetClassName(window, className, className.Capacity) <= 0)
            return false;
        var value = className.ToString();
        return string.Equals(value, "Edit", StringComparison.OrdinalIgnoreCase) ||
               value.Contains("RichEdit", StringComparison.OrdinalIgnoreCase) ||
               value.Contains(".EDIT.", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsWindowWithinRoot(IntPtr window, IntPtr rootWindow)
    {
        if (window == IntPtr.Zero || rootWindow == IntPtr.Zero || !IsWindow(window))
            return false;
        return window == rootWindow || IsChild(rootWindow, window) ||
               GetAncestor(window, AncestorRoot) == rootWindow;
    }

    private static bool IsForegroundRoot(IntPtr rootWindow)
    {
        var foreground = GetForegroundWindow();
        if (foreground == IntPtr.Zero) return false;
        var foregroundRoot = GetAncestor(foreground, AncestorRoot);
        return foreground == rootWindow || foregroundRoot == rootWindow;
    }

    private static bool IsValidTargetWindow(IntPtr targetWindow, string operation)
    {
        var valid = targetWindow != IntPtr.Zero && IsWindow(targetWindow);
        if (!valid)
            LogStage(operation + ".Target",
                $"result=invalid; target={FormatWindow(targetWindow)}");
        return valid;
    }

    private static string FormatWindow(IntPtr window) =>
        $"0x{window.ToInt64():X}";

    private static void LogStage(string stage, string detail) =>
        CrashLogger.Log("ClipboardPasteService." + stage, detail);

    private static Input KeyboardInput(ushort virtualKey, uint flags) => new()
    {
        Type = InputKeyboard,
        Data = new InputUnion
        {
            Keyboard = new KeyboardInputData
            {
                VirtualKey = virtualKey,
                Flags = flags
            }
        }
    };

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        internal uint Type;
        internal InputUnion Data;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)] internal MouseInputData Mouse;
        [FieldOffset(0)] internal KeyboardInputData Keyboard;
        [FieldOffset(0)] internal HardwareInputData Hardware;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInputData
    {
        internal int X;
        internal int Y;
        internal uint MouseData;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInputData
    {
        internal ushort VirtualKey;
        internal ushort ScanCode;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HardwareInputData
    {
        internal uint Message;
        internal ushort ParameterLow;
        internal ushort ParameterHigh;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct GuiThreadInfo
    {
        internal uint Size;
        internal uint Flags;
        internal IntPtr ActiveWindow;
        internal IntPtr FocusWindow;
        internal IntPtr CaptureWindow;
        internal IntPtr MenuOwnerWindow;
        internal IntPtr MoveSizeWindow;
        internal IntPtr CaretWindow;
        internal NativeRect CaretRectangle;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(
        uint inputCount,
        Input[] inputs,
        int inputSize);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool BringWindowToTop(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr SetActiveWindow(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassName(
        IntPtr window,
        StringBuilder className,
        int maximumCount);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeoutMilliseconds,
        out UIntPtr result);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr GetFocus();

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hwnd, uint gaFlags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsChild(IntPtr parent, IntPtr window);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachThreadInput(
        uint threadIdAttach,
        uint threadIdAttachTo,
        [MarshalAs(UnmanagedType.Bool)] bool attach);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetGUIThreadInfo(
        uint threadId,
        ref GuiThreadInfo info);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr window, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsIconic(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);
}
