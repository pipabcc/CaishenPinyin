using ShuruSettings;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

internal static class ClipboardTextWriterTests
{
    // 窗口站各有自己的剪贴板，测试实际 Win32 写入时不读取或覆盖用户剪贴板。
    internal static int Run()
    {
        var original = GetProcessWindowStation();
        var station = CreateWindowStation(null, 0, 0x000F037F, IntPtr.Zero);
        var desktop = IntPtr.Zero;
        try
        {
            Require(station != IntPtr.Zero && SetProcessWindowStation(station),
                "创建隔离剪贴板窗口站失败");
            desktop = CreateDesktop("ClipboardTextTests", null, IntPtr.Zero,
                0, 0x000F01FF, IntPtr.Zero);
            Require(desktop != IntPtr.Zero, "创建隔离桌面失败");
            Exception? failure = null;
            var thread = new Thread(() =>
            {
                try
                {
                    Require(SetThreadDesktop(desktop), "绑定隔离桌面失败");
                    RunCases(desktop);
                }
                catch (Exception ex) { failure = ex; }
            }) { IsBackground = true };
            thread.Start();
            Require(thread.Join(15_000), "原生文本剪贴板测试超时");
            if (failure != null) throw failure;
            Console.WriteLine("原生文本剪贴板：多行、长文本、内部标记、占用重试和取消通过");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            SetProcessWindowStation(original);
            if (desktop != IntPtr.Zero) CloseDesktop(desktop);
            if (station != IntPtr.Zero) CloseWindowStation(station);
        }
    }

    private static void RunCases(IntPtr desktop)
    {
        foreach (var text in new[]
        {
            "第一条中文记录🙂",
            "<?xml version=\"1.0\"?>\r\n<root>第二条\t记录</root>\r\n",
            new string('长', 180_000) + "\r\n末尾完整🙂",
            string.Empty
        })
        {
            ClipboardTextWriter.Write(text);
            Require(ReadText() == text, "文本或换行在剪贴板写入后发生改变");
            Require(IsClipboardFormatAvailable(RegisterClipboardFormat(
                ClipboardImageService.InternalPasteFormat)), "缺少内部粘贴标记，可能引起重复入库和排序变化");
        }

        const string original = "占用测试前的记录";
        ClipboardTextWriter.Write(original);
        var sequence = GetClipboardSequenceNumber();
        using (var cancellation = new CancellationTokenSource())
        {
            cancellation.Cancel();
            try
            {
                ClipboardTextWriter.Write("已取消的内容", cancellation.Token);
                throw new InvalidOperationException("已取消请求仍然写入剪贴板");
            }
            catch (OperationCanceledException) { }
        }
        Require(GetClipboardSequenceNumber() == sequence, "取消请求改变了剪贴板");

        Require(OpenClipboard(IntPtr.Zero), "测试无法占用隔离剪贴板");
        var stopwatch = Stopwatch.StartNew();
        Task attempt;
        try
        {
            attempt = WriteOnDesktop(desktop, "不应延迟写入的记录");
            try
            {
                attempt.GetAwaiter().GetResult();
                throw new InvalidOperationException("占用期间意外写入剪贴板");
            }
            catch (COMException ex) when (ex.HResult == unchecked((int)0x800401D0)) { }
            Require(stopwatch.Elapsed < TimeSpan.FromSeconds(3), "占用重试没有按时结束");
        }
        finally { CloseClipboard(); }
        Require(ReadText() == original && GetClipboardSequenceNumber() == sequence,
            "失败后改变了剪贴板或产生迟到写入");

        Require(OpenClipboard(IntPtr.Zero), "测试无法建立短暂占用");
        try
        {
            attempt = WriteOnDesktop(desktop, "占用释放后完成");
            Thread.Sleep(100);
        }
        finally { CloseClipboard(); }
        attempt.GetAwaiter().GetResult();
        Require(ReadText() == "占用释放后完成", "短暂占用释放后未重试成功");
    }

    private static Task WriteOnDesktop(IntPtr desktop, string text)
    {
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        new Thread(() =>
        {
            try
            {
                Require(SetThreadDesktop(desktop), "绑定写入线程桌面失败");
                ClipboardTextWriter.Write(text);
                completion.SetResult();
            }
            catch (Exception ex) { completion.SetException(ex); }
        }) { IsBackground = true }.Start();
        return completion.Task;
    }

    private static string ReadText()
    {
        Require(OpenClipboard(IntPtr.Zero), "读取隔离剪贴板失败");
        try
        {
            var memory = GetClipboardData(13);
            Require(memory != IntPtr.Zero, "没有 Unicode 文本格式");
            var data = GlobalLock(memory);
            Require(data != IntPtr.Zero, "无法锁定文本内存");
            try { return Marshal.PtrToStringUni(data) ?? string.Empty; }
            finally { GlobalUnlock(memory); }
        }
        finally { CloseClipboard(); }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(
            $"{message}，Win32={Marshal.GetLastWin32Error()}");
    }

    [DllImport("user32.dll")]
    private static extern IntPtr GetProcessWindowStation();
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowStation(string? name, uint flags, uint access, IntPtr security);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetProcessWindowStation(IntPtr station);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateDesktop(string name, string? device, IntPtr mode,
        uint flags, uint access, IntPtr security);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetThreadDesktop(IntPtr desktop);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseWindowStation(IntPtr station);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseClipboard();
    [DllImport("user32.dll")]
    private static extern IntPtr GetClipboardData(uint format);
    [DllImport("user32.dll")]
    private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint RegisterClipboardFormat(string format);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsClipboardFormatAvailable(uint format);
    [DllImport("kernel32.dll")]
    private static extern IntPtr GlobalLock(IntPtr memory);
    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalUnlock(IntPtr memory);
}
