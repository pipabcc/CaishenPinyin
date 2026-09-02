using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace ShuruSettings;

internal static class ImeRegistrationRepair
{
    internal const string CommandLineSwitch = "-register-ime-pair";
    private const string Clsid = "{7C4E9F2A-1B3D-4A8E-9F6C-2D5E8B1A4C7F}";
    private const ushort X86Machine = 0x014c;
    private const ushort X64Machine = 0x8664;

    internal static void RunElevated(string? registeredDllPath)
    {
        var directory = ResolveInstallDirectory(registeredDllPath);
        var executable = Path.Combine(AppContext.BaseDirectory, "ShuruSettings.exe");
        if (!File.Exists(executable))
            executable = Environment.ProcessPath ??
                throw new FileNotFoundException("未找到设置中心可执行文件。");

        var startInfo = new ProcessStartInfo(executable)
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden
        };
        startInfo.ArgumentList.Add(CommandLineSwitch);
        startInfo.ArgumentList.Add(directory);
        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException("无法启动输入法注册修复程序。");
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(
                $"输入法双架构注册修复返回错误码 {process.ExitCode}。");
    }

    internal static int Repair(string directory)
    {
        try
        {
            directory = Path.GetFullPath(directory);
            var x64Dll = Path.Combine(directory, "ShuruIme.dll");
            var x86Dll = Path.Combine(directory, "ShuruIme32.dll");
            ValidatePeMachine(x64Dll, X64Machine);
            ValidatePeMachine(x86Dll, X86Machine);

            RunRegsvr32(x86Dll, "SysWOW64", unregister: false);
            try
            {
                RunRegsvr32(x64Dll, "System32", unregister: false);
                if (!RegistrationMatches(RegistryView.Registry32, x86Dll) ||
                    !RegistrationMatches(RegistryView.Registry64, x64Dll))
                {
                    throw new InvalidOperationException(
                        "注册完成后，32 位或 64 位 COM 路径校验失败。");
                }
            }
            catch
            {
                TryUnregister(x64Dll, "System32");
                TryUnregister(x86Dll, "SysWOW64");
                throw;
            }
            return 0;
        }
        catch (Exception ex)
        {
            CrashLogger.Log("ImeRegistrationRepair", ex);
            return 1;
        }
    }

    internal static string? FindRegisteredX64Dll()
    {
        using var root = RegistryKey.OpenBaseKey(
            RegistryHive.ClassesRoot, RegistryView.Registry64);
        using var key = root.OpenSubKey($"CLSID\\{Clsid}\\InprocServer32");
        return key?.GetValue(null) as string;
    }

    internal static string RegistrationStatus()
    {
        var x64 = ReadRegisteredPath(RegistryView.Registry64);
        var x86 = ReadRegisteredPath(RegistryView.Registry32);
        if (string.IsNullOrWhiteSpace(x64) && string.IsNullOrWhiteSpace(x86))
            return "未检测到已注册的输入法，设置仍会保存到当前用户。";
        var installedPath = !string.IsNullOrWhiteSpace(x64) ? x64 : x86;
        var directory = Path.GetDirectoryName(installedPath);
        if (string.IsNullOrWhiteSpace(directory))
            return "输入法注册路径无效，请重新保存设置以修复。";
        if (string.IsNullOrWhiteSpace(x64) || string.IsNullOrWhiteSpace(x86) ||
            !File.Exists(x64) || !File.Exists(x86))
        {
            return $"输入法已安装：{directory}（组件不完整，请重新保存设置以修复）";
        }
        return $"输入法已安装：{directory}";
    }

    internal static bool NeedsRepair(string? registeredX64Dll)
    {
        // 未安装状态仍允许用户预先保存设置；只有已经存在 x64 注册时，
        // 才把缺失的 x86 配套注册视为可自动修复的问题。
        if (string.IsNullOrWhiteSpace(registeredX64Dll)) return false;
        if (!File.Exists(registeredX64Dll)) return true;
        var directory = Path.GetDirectoryName(
            Path.GetFullPath(registeredX64Dll));
        if (string.IsNullOrWhiteSpace(directory)) return true;
        var expectedX86Dll = Path.Combine(directory, "ShuruIme32.dll");
        return !File.Exists(expectedX86Dll) ||
            !RegistrationMatches(RegistryView.Registry64, registeredX64Dll) ||
            !RegistrationMatches(RegistryView.Registry32, expectedX86Dll);
    }

    private static string ResolveInstallDirectory(string? registeredDllPath)
    {
        if (!string.IsNullOrWhiteSpace(registeredDllPath) &&
            File.Exists(registeredDllPath))
        {
            return Path.GetDirectoryName(Path.GetFullPath(registeredDllPath))!;
        }
        var adjacentDll = Path.Combine(AppContext.BaseDirectory, "ShuruIme.dll");
        if (File.Exists(adjacentDll)) return AppContext.BaseDirectory;
        throw new FileNotFoundException("未找到已注册的 ShuruIme.dll。", registeredDllPath);
    }

    private static void ValidatePeMachine(string path, ushort expected)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("输入法组件不完整。", path);
        using var stream = File.OpenRead(path);
        using var reader = new BinaryReader(stream);
        if (reader.ReadUInt16() != 0x5a4d || stream.Length < 0x40)
            throw new InvalidDataException($"输入法组件不是有效 PE 文件：{path}");
        stream.Position = 0x3c;
        var peOffset = reader.ReadInt32();
        if (peOffset < 0 || peOffset + 6 > stream.Length)
            throw new InvalidDataException($"输入法组件 PE 头损坏：{path}");
        stream.Position = peOffset;
        if (reader.ReadUInt32() != 0x00004550 || reader.ReadUInt16() != expected)
            throw new InvalidDataException($"输入法组件架构不正确：{path}");
    }

    private static void RunRegsvr32(
        string dll, string systemDirectory, bool unregister)
    {
        var executable = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            systemDirectory, "regsvr32.exe");
        var startInfo = new ProcessStartInfo(executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        };
        startInfo.ArgumentList.Add("/s");
        if (unregister) startInfo.ArgumentList.Add("/u");
        startInfo.ArgumentList.Add(dll);
        using var process = Process.Start(startInfo) ??
            throw new InvalidOperationException($"无法启动 {systemDirectory} 注册程序。");
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(
                $"{systemDirectory} 注册程序返回错误码 {process.ExitCode}。");
    }

    private static void TryUnregister(string dll, string systemDirectory)
    {
        try { RunRegsvr32(dll, systemDirectory, unregister: true); }
        catch (Exception ex) { CrashLogger.Log("ImeRegistrationRollback", ex); }
    }

    private static string? ReadRegisteredPath(RegistryView view)
    {
        using var root = RegistryKey.OpenBaseKey(RegistryHive.ClassesRoot, view);
        using var key = root.OpenSubKey($"CLSID\\{Clsid}\\InprocServer32");
        return key?.GetValue(null) as string;
    }

    private static bool RegistrationMatches(RegistryView view, string expected)
    {
        var registered = ReadRegisteredPath(view);
        return !string.IsNullOrWhiteSpace(registered) &&
            string.Equals(Path.GetFullPath(registered), Path.GetFullPath(expected),
                StringComparison.OrdinalIgnoreCase);
    }
}
