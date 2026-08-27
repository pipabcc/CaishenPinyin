using System;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;

namespace ShuruSettings;

// 开始菜单搜索框宿主 SearchHost.exe 运行在 AppContainer 沙箱中，访问文件时
// 除常规用户/组检查外还必须命中 S-1-15-2-* 的 ACE，否则一律拒绝。沙箱进程
// 自己改不了 DACL，只能由普通宿主代为授权。
//
// 分级表与 src/common/private_acl.cpp 的 kUserDataGrants 保持一致：clipboard
// 目录刻意排除——剪贴板历史可能含密码等敏感内容，沙箱场景也用不到 v 模式面板。
internal static class AppContainerAccess
{
    private const FileSystemRights ReadRights =
        FileSystemRights.ReadAndExecute | FileSystemRights.Synchronize;

    private const FileSystemRights WriteRights =
        ReadRights | FileSystemRights.Write | FileSystemRights.Delete;

    private const InheritanceFlags FullInheritance =
        InheritanceFlags.ObjectInherit | InheritanceFlags.ContainerInherit;

    // ALL APPLICATION PACKAGES 与 ALL RESTRICTED APPLICATION PACKAGES。
    // 用 SID 字面量构造，避免依赖本地化账户名。
    private static readonly string[] PackageSids = { "S-1-15-2-1", "S-1-15-2-2" };

    private static readonly Grant[] Grants =
    {
        // 根目录只向直接子文件继承：settings.ini 被原子替换后依然能拿到读
        // 权限，而 clipboard 等子目录不会被一并放开。
        new(string.Empty, InheritanceFlags.ObjectInherit,
            PropagationFlags.NoPropagateInherit, ReadRights),
        new("skins", FullInheritance, PropagationFlags.None, ReadRights),
        new("data", FullInheritance, PropagationFlags.None, WriteRights),
        // 用户词库目录被 EnsureCurrentUserOnlyPath 设成受保护 DACL，继承在此
        // 断开，必须显式列出——否则沙箱里打字读不到已学词条也写不回结果。
        new(@"data\lexicon", FullInheritance, PropagationFlags.None, WriteRights),
        new("logs", FullInheritance, PropagationFlags.None, WriteRights),
        new("paste_requests", FullInheritance, PropagationFlags.None, WriteRights),
        new("ui_requests", FullInheritance, PropagationFlags.None, WriteRights),
    };

    // 剪贴板历史可能含密码等敏感内容，沙箱场景也用不到 v 模式面板。
    private static readonly string[] Denied =
    {
        "clipboard",
        // 直接上屏请求包含用户选择的正文，只供同一桌面用户下的设置程序
        // 与目标宿主交换；AppContainer 入口保持旧剪贴板回退，不开放此目录。
        "direct_commit_requests"
    };

    internal static string UserDataRoot => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CaishenPinyin");

    internal static void EnsureUserData()
    {
        foreach (var grant in Grants)
        {
            try
            {
                Apply(grant);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("AppContainerAccess.Apply", ex);
            }
        }

        // 从授权表里缺席只保证「不主动放行」。上级目录若带可继承的 AppContainer
        // ACE（部分机器的 LOCALAPPDATA 就是如此），剪贴板历史照样会漏出去。
        foreach (var denied in Denied)
        {
            try
            {
                Deny(denied);
            }
            catch (Exception ex)
            {
                CrashLogger.Log("AppContainerAccess.Deny", ex);
            }
        }
    }

    private static void Deny(string relative)
    {
        var info = Directory.CreateDirectory(
            Path.Combine(UserDataRoot, relative));
        var security = info.GetAccessControl(AccessControlSections.Access);

        var changed = false;
        foreach (var sid in PackageSids)
        {
            var identity = new SecurityIdentifier(sid);
            if (HasDenyRule(security, identity)) continue;
            security.AddAccessRule(new FileSystemAccessRule(
                identity, FileSystemRights.FullControl, FullInheritance,
                PropagationFlags.None, AccessControlType.Deny));
            changed = true;
        }

        // AddAccessRule 会把 Deny 排到 Allow 之前，继承下来的放行条目不会先命中。
        if (changed) info.SetAccessControl(security);
    }

    private static bool HasDenyRule(
        DirectorySecurity security, SecurityIdentifier identity)
    {
        foreach (var entry in security.GetAccessRules(
                     includeExplicit: true, includeInherited: false,
                     typeof(SecurityIdentifier)))
        {
            if (entry is not FileSystemAccessRule rule) continue;
            if (rule.AccessControlType != AccessControlType.Deny) continue;
            if (!rule.IdentityReference.Equals(identity)) continue;
            if (rule.InheritanceFlags != FullInheritance) continue;
            if (rule.FileSystemRights.HasFlag(FileSystemRights.FullControl))
                return true;
        }
        return false;
    }

    private static void Apply(Grant grant)
    {
        var path = grant.Relative.Length == 0
            ? UserDataRoot
            : Path.Combine(UserDataRoot, grant.Relative);
        var info = Directory.CreateDirectory(path);
        var security = info.GetAccessControl(AccessControlSections.Access);

        var changed = false;
        foreach (var sid in PackageSids)
        {
            var identity = new SecurityIdentifier(sid);
            if (HasRule(security, identity, grant)) continue;
            security.AddAccessRule(new FileSystemAccessRule(
                identity, grant.Rights, grant.Inheritance, grant.Propagation,
                AccessControlType.Allow));
            changed = true;
        }

        // 已授权时不落盘，避免每次启动都改写 DACL。
        if (changed) info.SetAccessControl(security);
    }

    // 只认显式条目：继承来的 ACE 会随父目录 DACL 变化而消失。
    private static bool HasRule(
        DirectorySecurity security, SecurityIdentifier identity, Grant grant)
    {
        foreach (var entry in security.GetAccessRules(
                     includeExplicit: true, includeInherited: false,
                     typeof(SecurityIdentifier)))
        {
            if (entry is not FileSystemAccessRule rule) continue;
            if (rule.AccessControlType != AccessControlType.Allow) continue;
            if (!rule.IdentityReference.Equals(identity)) continue;
            if (rule.InheritanceFlags != grant.Inheritance) continue;
            if (rule.PropagationFlags != grant.Propagation) continue;
            if ((rule.FileSystemRights & grant.Rights) == grant.Rights) return true;
        }
        return false;
    }

    private sealed record Grant(
        string Relative,
        InheritanceFlags Inheritance,
        PropagationFlags Propagation,
        FileSystemRights Rights);
}
