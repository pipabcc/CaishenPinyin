using System;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;

namespace ShuruSettings;

internal static class UserDataPrivacy
{
    internal static void ProtectDirectory(string path, bool recursive = false)
    {
        var directory = Directory.CreateDirectory(path);
        if (directory.Attributes.HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("个人数据目录不能是重解析点。");
        var user = WindowsIdentity.GetCurrent().User ??
            throw new InvalidOperationException("无法取得当前用户身份。");
        var security = new DirectorySecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.AddAccessRule(new FileSystemAccessRule(user, FileSystemRights.FullControl,
            InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
            PropagationFlags.None, AccessControlType.Allow));
        directory.SetAccessControl(security);
        if (!recursive) return;
        foreach (var item in directory.EnumerateFileSystemInfos())
        {
            if (item.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue;
            if (item is DirectoryInfo child) ProtectDirectory(child.FullName, recursive: true);
            else ProtectFile(item.FullName);
        }
    }

    internal static void ProtectFile(string path)
    {
        var file = new FileInfo(path);
        if (!file.Exists) return;
        if (file.Attributes.HasFlag(FileAttributes.ReparsePoint))
            throw new IOException("个人数据文件不能是重解析点。");
        var user = WindowsIdentity.GetCurrent().User ??
            throw new InvalidOperationException("无法取得当前用户身份。");
        var security = new FileSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.AddAccessRule(new FileSystemAccessRule(user,
            FileSystemRights.FullControl, AccessControlType.Allow));
        file.SetAccessControl(security);
    }
}
