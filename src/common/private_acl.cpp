#include "private_acl.h"

#include "user_data_paths.h"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <filesystem>
#include <vector>

namespace shuru {
namespace {

// 与安装目录、系统词库目录既有的 (A;OICI;0x1200a9;;;AC) 掩码保持一致。
constexpr DWORD kAppContainerReadMask =
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
// 统计与学习数据都走临时文件 + MoveFileExW 原子替换，需要写与删除。
constexpr DWORD kAppContainerWriteMask =
    kAppContainerReadMask | FILE_GENERIC_WRITE | DELETE;

DWORD InheritanceFlags(AclInheritance inheritance) noexcept {
    switch (inheritance) {
    case AclInheritance::DirectFilesOnly:
        return OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE;
    case AclInheritance::Full:
        return OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    default:
        return 0;
    }
}

class LocalSid {
public:
    explicit LocalSid(const wchar_t* sddl) {
        if (!ConvertStringSidToSidW(sddl, &sid_)) sid_ = nullptr;
    }
    ~LocalSid() {
        if (sid_ != nullptr) LocalFree(sid_);
    }
    LocalSid(const LocalSid&) = delete;
    LocalSid& operator=(const LocalSid&) = delete;
    PSID get() const noexcept { return sid_; }
    bool valid() const noexcept { return sid_ != nullptr; }

private:
    PSID sid_ = nullptr;
};

bool CurrentUserSid(std::vector<BYTE>* storage, PSID* sid) {
    if (storage == nullptr || sid == nullptr) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    storage->resize(bytes);
    const BOOL ok = bytes != 0 && GetTokenInformation(
        token, TokenUser, storage->data(), bytes, &bytes);
    CloseHandle(token);
    if (!ok) return false;
    *sid = reinterpret_cast<TOKEN_USER*>(storage->data())->User.Sid;
    return IsValidSid(*sid) != FALSE;
}

void FillAccess(
    EXPLICIT_ACCESSW* access,
    PSID sid,
    DWORD permissions,
    DWORD inheritance,
    TRUSTEE_TYPE trustee_type) {
    *access = EXPLICIT_ACCESSW {};
    access->grfAccessPermissions = permissions;
    access->grfAccessMode = SET_ACCESS;
    access->grfInheritance = inheritance;
    access->Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access->Trustee.TrusteeType = trustee_type;
    access->Trustee.ptstrName = static_cast<LPWSTR>(sid);
}

// 只认显式条目：继承来的 ACE 会随父目录 DACL 变化而消失，不能当作已授权。
bool HasExplicitAce(
    PACL acl, PSID sid, DWORD mask, DWORD inheritance, BYTE ace_type) {
    if (acl == nullptr || sid == nullptr) return false;
    constexpr DWORD kInheritMask =
        OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        LPVOID entry = nullptr;
        if (!GetAce(acl, index, &entry)) continue;
        auto* header = static_cast<ACE_HEADER*>(entry);
        if (header->AceType != ace_type) continue;
        if ((header->AceFlags & INHERITED_ACE) != 0) continue;
        if ((header->AceFlags & kInheritMask) != inheritance) continue;
        // ACCESS_ALLOWED_ACE 与 ACCESS_DENIED_ACE 布局一致，共用一条解析路径。
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if ((ace->Mask & mask) != mask) continue;
        if (EqualSid(reinterpret_cast<PSID>(&ace->SidStart), sid) != FALSE) {
            return true;
        }
    }
    return false;
}

bool ApplyAcl(
    const std::wstring& path,
    PSID sid,
    bool directory,
    bool allow_app_containers) {
    const LocalSid all_packages(L"S-1-15-2-1");
    const LocalSid restricted_packages(L"S-1-15-2-2");
    const DWORD inheritance = directory
        ? InheritanceFlags(AclInheritance::Full)
        : InheritanceFlags(AclInheritance::None);

    // 受保护 DACL 会切断继承，因此沙箱宿主所需的 ACE 必须在同一批里写入，
    // 否则开始菜单等 AppContainer 场景读不到学习数据与固定候选。
    EXPLICIT_ACCESSW entries[3] {};
    ULONG count = 0;
    FillAccess(
        &entries[count++], sid, GENERIC_ALL, inheritance, TRUSTEE_IS_USER);
    if (allow_app_containers && all_packages.valid()) {
        FillAccess(
            &entries[count++], all_packages.get(), kAppContainerWriteMask,
            inheritance, TRUSTEE_IS_WELL_KNOWN_GROUP);
    }
    if (allow_app_containers && restricted_packages.valid()) {
        FillAccess(
            &entries[count++], restricted_packages.get(),
            kAppContainerWriteMask, inheritance, TRUSTEE_IS_WELL_KNOWN_GROUP);
    }

    PACL acl = nullptr;
    if (SetEntriesInAclW(count, entries, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

struct UserDataGrant {
    const wchar_t* relative;
    AclInheritance inheritance;
    bool allow_write;
};

// clipboard 目录刻意缺席：剪贴板历史可能含密码等敏感内容，而沙箱宿主用不到
// v 模式面板。根目录用 DirectFilesOnly，使 settings.ini 即便被重建也能继承到
// 读权限，同时不会把 clipboard 子目录一并放开。
constexpr UserDataGrant kUserDataGrants[] = {
    {L"",                AclInheritance::DirectFilesOnly, false},
    {L"skins",           AclInheritance::Full,            false},
    {L"data",            AclInheritance::Full,            true},
    // 用户词库目录被 EnsureCurrentUserOnlyPath 设成受保护 DACL，继承在此断开，
    // 必须显式列出——否则沙箱里打字既读不到已学词条也写不回学习结果。
    {L"data\\lexicon",   AclInheritance::Full,            true},
    {L"logs",            AclInheritance::Full,            true},
    {L"paste_requests",  AclInheritance::Full,            true},
    {L"ui_requests",     AclInheritance::Full,            true},
};

// 从授权表里缺席只保证「我们不主动放行」。若上级目录带可继承的 AppContainer
// ACE（部分机器的 Temp、LOCALAPPDATA 就是如此），权限仍会漏下来，因此这些
// 路径还要显式 Deny。
constexpr const wchar_t* kUserDataDenied[] = {
    L"clipboard",
    // 独立搜索窗口只在普通桌面宿主启用直接上屏。请求正文可能来自剪贴板
    // 历史，绝不能因为上级目录的继承条目暴露给任意 AppContainer。
    L"direct_commit_requests",
};

}  // namespace

bool EnsureAppContainerAccess(
    const std::wstring& path, AclInheritance inheritance, bool allow_write) {
    if (path.empty()) return false;
    const LocalSid all_packages(L"S-1-15-2-1");
    if (!all_packages.valid()) return false;
    const LocalSid restricted_packages(L"S-1-15-2-2");

    PACL existing_dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &existing_dacl, nullptr,
            &descriptor) != ERROR_SUCCESS) {
        return false;
    }

    const DWORD mask =
        allow_write ? kAppContainerWriteMask : kAppContainerReadMask;
    const DWORD flags = InheritanceFlags(inheritance);
    const bool has_all = HasExplicitAce(
        existing_dacl, all_packages.get(), mask, flags,
        ACCESS_ALLOWED_ACE_TYPE);
    const bool has_restricted = !restricted_packages.valid() ||
        HasExplicitAce(
            existing_dacl, restricted_packages.get(), mask, flags,
            ACCESS_ALLOWED_ACE_TYPE);
    if (has_all && has_restricted) {
        LocalFree(descriptor);
        return true;
    }

    EXPLICIT_ACCESSW entries[2] {};
    ULONG count = 0;
    FillAccess(
        &entries[count++], all_packages.get(), mask, flags,
        TRUSTEE_IS_WELL_KNOWN_GROUP);
    if (restricted_packages.valid()) {
        FillAccess(
            &entries[count++], restricted_packages.get(), mask, flags,
            TRUSTEE_IS_WELL_KNOWN_GROUP);
    }

    // 以既有 DACL 为基础合并，且不设置 PROTECTED，保留原有条目与继承链。
    PACL merged = nullptr;
    const DWORD merge_result =
        SetEntriesInAclW(count, entries, existing_dacl, &merged);
    LocalFree(descriptor);
    if (merge_result != ERROR_SUCCESS) return false;

    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, merged, nullptr);
    LocalFree(merged);
    return result == ERROR_SUCCESS;
}

bool EnsureAppContainerDenied(const std::wstring& path) {
    if (path.empty()) return false;
    const LocalSid all_packages(L"S-1-15-2-1");
    if (!all_packages.valid()) return false;
    const LocalSid restricted_packages(L"S-1-15-2-2");

    PACL existing_dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &existing_dacl, nullptr,
            &descriptor) != ERROR_SUCCESS) {
        return false;
    }

    const DWORD flags = InheritanceFlags(AclInheritance::Full);
    const bool has_all = HasExplicitAce(
        existing_dacl, all_packages.get(), FILE_ALL_ACCESS, flags,
        ACCESS_DENIED_ACE_TYPE);
    const bool has_restricted = !restricted_packages.valid() ||
        HasExplicitAce(
            existing_dacl, restricted_packages.get(), FILE_ALL_ACCESS, flags,
            ACCESS_DENIED_ACE_TYPE);
    if (has_all && has_restricted) {
        LocalFree(descriptor);
        return true;
    }

    EXPLICIT_ACCESSW entries[2] {};
    ULONG count = 0;
    FillAccess(
        &entries[count], all_packages.get(), FILE_ALL_ACCESS, flags,
        TRUSTEE_IS_WELL_KNOWN_GROUP);
    entries[count++].grfAccessMode = DENY_ACCESS;
    if (restricted_packages.valid()) {
        FillAccess(
            &entries[count], restricted_packages.get(), FILE_ALL_ACCESS, flags,
            TRUSTEE_IS_WELL_KNOWN_GROUP);
        entries[count++].grfAccessMode = DENY_ACCESS;
    }

    // SetEntriesInAcl 会把 Deny 排到 Allow 之前，因此上级继承下来的放行条目
    // 不会先命中。
    PACL merged = nullptr;
    const DWORD merge_result =
        SetEntriesInAclW(count, entries, existing_dacl, &merged);
    LocalFree(descriptor);
    if (merge_result != ERROR_SUCCESS) return false;

    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, merged, nullptr);
    LocalFree(merged);
    return result == ERROR_SUCCESS;
}

void EnsureUserDataAppContainerAccess() {
    static volatile LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) != 0) return;
    // 沙箱进程改不了自己要读的目录的 DACL，只能由普通宿主代劳。
    if (IsCurrentProcessAppContainer()) return;
    const std::wstring root = CaishenLocalAppData();
    if (root.empty()) return;
    const std::wstring base = root + L"\\CaishenPinyin";

    for (const auto& grant : kUserDataGrants) {
        std::wstring path = base;
        if (grant.relative[0] != L'\0') {
            path += L"\\";
            path += grant.relative;
        }
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(path), error);
        if (error) continue;
        EnsureAppContainerAccess(path, grant.inheritance, grant.allow_write);
    }

    // 剪贴板历史单独封死：上级目录若带可继承的放行条目，「不授权」是挡不住的。
    for (const wchar_t* denied : kUserDataDenied) {
        const std::wstring path = base + L"\\" + denied;
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(path), error);
        if (error) continue;
        EnsureAppContainerDenied(path);
    }
}

bool EnsureCurrentUserOnlyPath(const std::wstring& path, bool is_directory) {
    if (path.empty()) return false;
    std::vector<BYTE> sid_storage;
    PSID sid = nullptr;
    if (!CurrentUserSid(&sid_storage, &sid)) return false;
    try {
        const std::filesystem::path target(path);
        const std::filesystem::path directory = is_directory ? target : target.parent_path();
        if (directory.empty()) return false;
        std::filesystem::create_directories(directory);
        if (!ApplyAcl(directory.wstring(), sid, true, true)) return false;
        if (!is_directory && std::filesystem::exists(target)) {
            return ApplyAcl(target.wstring(), sid, false, true);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool EnsureCurrentUserPrivatePath(
    const std::wstring& path, bool is_directory) {
    if (path.empty()) return false;
    std::vector<BYTE> sid_storage;
    PSID sid = nullptr;
    if (!CurrentUserSid(&sid_storage, &sid)) return false;
    try {
        const std::filesystem::path target(path);
        const std::filesystem::path directory = is_directory
            ? target : target.parent_path();
        if (directory.empty()) return false;
        std::filesystem::create_directories(directory);
        if (!ApplyAcl(directory.wstring(), sid, true, false)) return false;
        if (!is_directory && std::filesystem::exists(target)) {
            return ApplyAcl(target.wstring(), sid, false, false);
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace shuru
