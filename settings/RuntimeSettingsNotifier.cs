using System.Runtime.InteropServices;
using System.Text;

namespace ShuruSettings;

internal static class RuntimeSettingsNotifier
{
    private const string CandidateWindowClass = "ShuruCandidateWindowClass";
    private const string MessageName = "CaishenPinyin.RuntimeSettingsChanged.v1";
    private const uint SendMessageAbortIfHung = 0x0002;

    internal static int NotifyCandidateWindows()
    {
        var message = RegisterWindowMessage(MessageName);
        if (message == 0) return 0;
        var notified = 0;
        UIntPtr messageResult;
        EnumWindows((window, _) =>
        {
            var className = new StringBuilder(128);
            if (GetClassName(window, className, className.Capacity) > 0 &&
                string.Equals(className.ToString(), CandidateWindowClass,
                    StringComparison.Ordinal) &&
                SendMessageTimeout(
                    window, message, UIntPtr.Zero, IntPtr.Zero,
                    SendMessageAbortIfHung, 2000, out messageResult) != IntPtr.Zero)
            {
                ++notified;
            }
            return true;
        }, IntPtr.Zero);
        return notified;
    }

    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint RegisterWindowMessage(string message);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassName(
        IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(
        EnumWindowsCallback callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out UIntPtr result);
}
