using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace ShuruSettings;

internal static class ClipboardPasteService
{
    private const uint InputKeyboard = 1;
    private const ushort VirtualKeyControl = 0x11;
    private const ushort VirtualKeyV = 0x56;
    private const uint KeyEventKeyUp = 0x0002;
    private const int ShowRestore = 9;

    internal static async Task<bool> PasteRecordAsync(
        string recordId,
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        var record = ClipboardStore.FindRecord(recordId);
        if (record == null) return false;

        if (record.IsImage)
            ClipboardImageService.SetClipboardImage(record.ImagePath);
        else
            ClipboardImageService.SetClipboardText(record.Content);

        return await PasteCurrentClipboardAsync(
            targetWindow, cancellationToken).ConfigureAwait(true);
    }

    internal static async Task<bool> PasteTextRequestAsync(
        string requestToken,
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        if (targetWindow == IntPtr.Zero || !IsWindow(targetWindow))
            return false;
        var text = TextPasteRequestStore.ReadAndDelete(requestToken);
        ClipboardImageService.SetClipboardText(text);
        return await PasteCurrentClipboardAsync(
            targetWindow, cancellationToken).ConfigureAwait(true);
    }

    internal static async Task<bool> PasteCurrentClipboardAsync(
        IntPtr targetWindow,
        CancellationToken cancellationToken = default)
    {
        if (targetWindow == IntPtr.Zero || !IsWindow(targetWindow))
            return false;
        await Task.Delay(80, cancellationToken).ConfigureAwait(true);

        var rootWindow = GetAncestor(targetWindow, 2); // GA_ROOT = 2
        var activateTarget = rootWindow != IntPtr.Zero ? rootWindow : targetWindow;

        SetForegroundWindow(activateTarget);
        SetFocus(targetWindow);
        await Task.Delay(60, cancellationToken).ConfigureAwait(true);

        var inputs = new[]
        {
            KeyboardInput(0x10, KeyEventKeyUp), // Shift Up
            KeyboardInput(0x12, KeyEventKeyUp), // Alt Up
            KeyboardInput(VirtualKeyControl, 0),
            KeyboardInput(VirtualKeyV, 0),
            KeyboardInput(VirtualKeyV, KeyEventKeyUp),
            KeyboardInput(VirtualKeyControl, KeyEventKeyUp)
        };
        return SendInput(
            (uint)inputs.Length,
            inputs,
            Marshal.SizeOf<Input>()) == inputs.Length;
    }

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

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(
        uint inputCount,
        Input[] inputs,
        int inputSize);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hwnd, uint gaFlags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr window, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);
}
