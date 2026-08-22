// AppContainer 授权回归：开始菜单搜索框宿主 SearchHost.exe 运行在沙箱中，
// 访问文件除常规用户/组检查外还必须命中 S-1-15-2-* 的 ACE。用户数据目录缺这
// 条 ACE 时皮肤会退回内置默认、统计归零、学习结果丢失。
#include "common/private_acl.h"
#include "common/user_data_paths.h"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(x) do { if (!(x)) { \
    std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)

namespace {

namespace fs = std::filesystem;

std::wstring ReadDacl(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return {};
    }
    LPWSTR text = nullptr;
    std::wstring sddl;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text,
            nullptr)) {
        sddl = text;
        LocalFree(text);
    }
    LocalFree(descriptor);
    return sddl;
}

// SDDL 会把 S-1-15-2-1 缩写成 AC、S-1-15-2-2 缩写成 (无简写，保留原样)。
bool MentionsAllPackages(const std::wstring& sddl) {
    return sddl.find(L";;;AC)") != std::wstring::npos ||
           sddl.find(L";;;S-1-15-2-1)") != std::wstring::npos;
}

// 显式 Deny 条目，SDDL 里以 (D;...) 开头。
bool DeniesAllPackages(const std::wstring& sddl) {
    std::size_t position = sddl.find(L"(D;");
    while (position != std::wstring::npos) {
        const std::size_t end = sddl.find(L')', position);
        if (end == std::wstring::npos) break;
        const std::wstring ace = sddl.substr(position, end - position + 1);
        if (ace.find(L";AC)") != std::wstring::npos ||
            ace.find(L";S-1-15-2-1)") != std::wstring::npos) {
            return true;
        }
        position = sddl.find(L"(D;", position + 1);
    }
    return false;
}

std::size_t CountAces(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return 0;
    }
    const std::size_t count = dacl == nullptr ? 0 : dacl->AceCount;
    LocalFree(descriptor);
    return count;
}

}  // namespace

int wmain() {
    const fs::path root = fs::temp_directory_path() /
        (L"caishen-appcontainer-acl-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    CHECK(fs::create_directories(root, error) && !error);

    // 普通测试进程不应被识别为沙箱宿主，否则授权入口会被整体跳过。
    CHECK(!shuru::IsCurrentProcessAppContainer());

    // 1. 单点授权后 DACL 中出现 AppContainer 条目。
    const fs::path standalone = root / L"standalone";
    CHECK(fs::create_directories(standalone, error) && !error);
    CHECK(shuru::EnsureAppContainerAccess(
        standalone.wstring(), shuru::AclInheritance::Full, true));
    CHECK(MentionsAllPackages(ReadDacl(standalone.wstring())));

    // 2. 幂等：已授权时不再追加条目。
    const std::size_t after_first = CountAces(standalone.wstring());
    CHECK(after_first > 0);
    CHECK(shuru::EnsureAppContainerAccess(
        standalone.wstring(), shuru::AclInheritance::Full, true));
    CHECK(CountAces(standalone.wstring()) == after_first);

    // 3. 合并而非替换：既有条目必须保留，否则宿主自身会被挡在外面。
    const std::wstring merged = ReadDacl(standalone.wstring());
    CHECK(merged.find(L";;;SY)") != std::wstring::npos ||
          merged.find(L";;;BA)") != std::wstring::npos ||
          merged.find(L"S-1-5-21-") != std::wstring::npos);

    // 4. 受保护 DACL 仍要放行沙箱宿主——学习数据与固定候选走这条路径。
    const fs::path guarded = root / L"guarded";
    CHECK(fs::create_directories(guarded, error) && !error);
    CHECK(shuru::EnsureCurrentUserOnlyPath(guarded.wstring(), true));
    CHECK(MentionsAllPackages(ReadDacl(guarded.wstring())));

    // 5. 分级表：clipboard 必须封死，data 可写，根目录只向直接子文件继承。
    const fs::path isolated = root / L"local-app-data";
    const fs::path user_data = isolated / L"CaishenPinyin";
    CHECK(fs::create_directories(user_data / L"clipboard", error) && !error);
    // 用户词库目录在真实环境里已被 EnsureCurrentUserOnlyPath 设成受保护 DACL，
    // 继承到此为止；分级表若不显式列出它，沙箱中的学习数据就仍然读不到。
    CHECK(fs::create_directories(user_data / L"data" / L"lexicon", error) && !error);
    CHECK(shuru::EnsureCurrentUserOnlyPath(
        (user_data / L"data" / L"lexicon").wstring(), true));
    { std::ofstream settings(user_data / L"settings.ini"); settings << "SkinId=wz\n"; }
    // 模拟部分机器上 LOCALAPPDATA（或 Temp）自带可继承放行条目的情形：这种
    // 父目录下「不授权」根本挡不住剪贴板历史外泄，必须靠显式 Deny。
    CHECK(shuru::EnsureAppContainerAccess(
        isolated.wstring(), shuru::AclInheritance::Full, false));
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"clipboard").wstring())));
    CHECK(SetEnvironmentVariableW(L"LOCALAPPDATA", isolated.c_str()));

    shuru::EnsureUserDataAppContainerAccess();

    const std::wstring root_dacl = ReadDacl(user_data.wstring());
    CHECK(MentionsAllPackages(root_dacl));
    // OI + NP：只传给直接子文件，不下发给 clipboard 等子目录。
    CHECK(root_dacl.find(L"(A;OINP;") != std::wstring::npos);
    CHECK(root_dacl.find(L"(A;OICI;0x1200a9;;;AC)") == std::wstring::npos);

    for (const wchar_t* writable : {L"data", L"data\\lexicon", L"logs",
                                    L"ui_requests"}) {
        const std::wstring path = (user_data / writable).wstring();
        CHECK(MentionsAllPackages(ReadDacl(path)));
        CHECK(ReadDacl(path).find(L"0x1301bf") != std::wstring::npos);
    }
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"skins").wstring())));

    // 剪贴板历史可能含密码，任何沙箱应用都不该读到——即便父目录已放行。
    const std::wstring clipboard_dacl =
        ReadDacl((user_data / L"clipboard").wstring());
    CHECK(DeniesAllPackages(clipboard_dacl));
    // Deny 必须排在继承下来的 Allow 之前才真正生效。
    CHECK(clipboard_dacl.find(L"(D;") < clipboard_dacl.find(L";;;AC)"));

    // settings.ini 靠根目录的 OI+NP 继承拿到读权限，被原子替换后依然有效。
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"settings.ini").wstring())));

    fs::remove_all(root, error);
    std::cout << "appcontainer acl ok\n";
    return 0;
}
