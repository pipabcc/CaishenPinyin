Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class KillStatus {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static int HideAll() {
    int n=0;
    EnumWindows((h,l)=>{
      var c=new StringBuilder(64);
      GetClassName(h,c,64);
      if (c.ToString()=="ShuruStatusWindowClass") {
        ShowWindow(h, 0); // SW_HIDE
        n++;
        uint pid; GetWindowThreadProcessId(h, out pid);
        Console.WriteLine("HID status hwnd="+h.ToInt64().ToString("X")+" pid="+pid);
      }
      return true;
    }, IntPtr.Zero);
    return n;
  }
}
"@
$n = [KillStatus]::HideAll()
"hidden_status_bars=$n"
"TIP: close Notepad/Chrome/Edge tabs using the IME, or sign out, so ShuruIme20.dll reloads."
