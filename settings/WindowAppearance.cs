using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace ShuruSettings;

internal static class WindowAppearance
{
    private const int DwmWindowCornerPreference = 33;
    private const int DwmRoundCorner = 2;

    internal static void EnableRoundedCorners(Window window)
    {
        ArgumentNullException.ThrowIfNull(window);
        window.SourceInitialized += (_, _) =>
        {
            var handle = new WindowInteropHelper(window).Handle;
            if (handle == IntPtr.Zero) return;
            var preference = DwmRoundCorner;
            _ = DwmSetWindowAttribute(
                handle,
                DwmWindowCornerPreference,
                ref preference,
                Marshal.SizeOf<int>());
        };
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(
        IntPtr window,
        int attribute,
        ref int value,
        int valueSize);
}
