#pragma once

#include <string>

namespace shuru {

// ACE 的继承范围。
enum class AclInheritance {
    // 仅作用于对象本身。
    None,
    // 对象本身及其直接子文件，不含子目录，且不再向下传播。
    // 用于用户数据根目录：settings.ini 被重建也能拿到权限，而 clipboard
    // 等敏感子目录不会被一并放开。
    DirectFilesOnly,
    // 对象本身及其下所有子目录与子文件。
    Full,
};

// 当前进程是否运行在 AppContainer 沙箱中，见 common/user_data_paths.h 的
// IsCurrentProcessAppContainer()。

// 为 AppContainer 宿主授予访问权限：把 ALL APPLICATION PACKAGES
// (S-1-15-2-1) 与 ALL RESTRICTED APPLICATION PACKAGES (S-1-15-2-2) 的 ACE
// 合并进既有 DACL，保留原有条目与继承链。AppContainer 自身改不了 ACL，
// 调用方必须在普通宿主进程中执行。已授权时直接返回，避免重复写入。
bool EnsureAppContainerAccess(
    const std::wstring& path, AclInheritance inheritance, bool allow_write);

// 用显式 Deny 把某条路径对所有 AppContainer 封死。仅仅「不授权」并不够：
// 部分机器的 LOCALAPPDATA 上级目录带可继承的 AppContainer ACE，权限会顺着
// 继承链漏进来。剪贴板历史可能含密码，必须硬性拒绝。
bool EnsureAppContainerDenied(const std::wstring& path);

// 按分级表为用户数据目录授权，使沙箱宿主能读到皮肤、设置与统计，并写回
// 学习数据。clipboard 目录刻意排除——剪贴板历史敏感且沙箱场景用不到。
// 进程内只实际执行一次；在 AppContainer 中调用是空操作。
void EnsureUserDataAppContainerAccess();

// Applies a protected DACL granting full control only to the current user,
// while keeping AppContainer hosts able to read and write the same path.
// Existing parent directories are hardened as well. Returns false without
// blocking input when security APIs or the filesystem reject the operation.
bool EnsureCurrentUserOnlyPath(const std::wstring& path, bool is_directory);

// Applies a protected DACL granting full control only to the current user.
// Unlike EnsureCurrentUserOnlyPath, this never adds AppContainer allow ACEs;
// use it for clipboard-derived or similarly sensitive exchange directories.
bool EnsureCurrentUserPrivatePath(
    const std::wstring& path, bool is_directory);

}  // namespace shuru
