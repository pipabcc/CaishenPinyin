#include "private_acl.h"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <filesystem>
#include <vector>

namespace shuru {
namespace {

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

bool ApplyAcl(const std::wstring& path, PSID sid, bool directory) {
    EXPLICIT_ACCESSW access {};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}

}  // namespace

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
        if (!ApplyAcl(directory.wstring(), sid, true)) return false;
        if (!is_directory && std::filesystem::exists(target)) {
            return ApplyAcl(target.wstring(), sid, false);
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace shuru
