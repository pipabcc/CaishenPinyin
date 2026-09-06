// 公共资源供沙箱读取，个人数据拒绝访问；验证旧权限迁移和真实受限进程。
#include "common/private_acl.h"
#include "common/user_data_paths.h"

#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

bool ProbeWithAppContainer(const fs::path& root, const fs::path& private_file,
                            const fs::path& public_file) {
    const auto name = L"CaishenAclTest_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64());
    PSID sid = nullptr;
    if (FAILED(CreateAppContainerProfile(name.c_str(), name.c_str(), L"临时权限回归测试",
                                         nullptr, 0, &sid))) return false;
    struct Cleanup {
        const std::wstring& name;
        PSID sid;
        ~Cleanup() { DeleteAppContainerProfile(name.c_str()); FreeSid(sid); }
    } cleanup{name, sid};
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const auto probe = root / L"acl-probe.exe";
    if (!CopyFileW(executable, probe.c_str(), FALSE) ||
        !shuru::EnsureAppContainerAccess(root.wstring(), shuru::AclInheritance::Full, false) ||
        !shuru::EnsureAppContainerAccess(probe.wstring(), shuru::AclInheritance::None, false)) return false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = sid;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
            &capabilities, sizeof(capabilities), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    auto command = L"\"" + probe.wstring() + L"\" --probe-private \"" + private_file.wstring() +
        L"\" \"" + public_file.wstring() + L"\"";
    const bool created = CreateProcessW(probe.c_str(), command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process) != FALSE;
    const auto creation_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        std::cerr << "AppContainer process creation error=" << creation_error << '\n';
        return false;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
    DWORD code = 99;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0) std::cerr << "AppContainer probe exit=" << code << '\n';
    return wait == WAIT_OBJECT_0 && code == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--probe-private") {
        if (!shuru::IsCurrentProcessAppContainer()) return 90;
        const DWORD access_masks[] = {GENERIC_READ, GENERIC_WRITE};
        for (const DWORD access : access_masks) {
            HANDLE file = CreateFileW(argv[2], access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return 91; }
            if (GetLastError() != ERROR_ACCESS_DENIED) return 92;
        }
        HANDLE file = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 93;
        CloseHandle(file);
        return 0;
    }
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

    // 私有路径不再为应用包组添加允许条目。
    const fs::path guarded = root / L"guarded";
    CHECK(fs::create_directories(guarded, error) && !error);
    CHECK(shuru::EnsureCurrentUserOnlyPath(guarded.wstring(), true));
    CHECK(!MentionsAllPackages(ReadDacl(guarded.wstring())));

    const fs::path private_only = root / L"private-only";
    CHECK(fs::create_directories(private_only, error) && !error);
    CHECK(shuru::EnsureCurrentUserPrivatePath(
        private_only.wstring(), true));
    CHECK(!MentionsAllPackages(ReadDacl(private_only.wstring())));

    // 分级表：个人数据保持私有，公共资源只读，根目录仅向直接子文件继承。
    const fs::path isolated = root / L"local-app-data";
    const fs::path user_data = isolated / L"CaishenPinyin";
    CHECK(fs::create_directories(user_data / L"clipboard", error) && !error);
    CHECK(fs::create_directories(
        user_data / L"direct_commit_requests", error) && !error);
    // 旧文件可能保留显式应用包权限，迁移必须覆盖受保护的子目录和文件。
    CHECK(fs::create_directories(user_data / L"data" / L"lexicon", error) && !error);
    CHECK(shuru::EnsureCurrentUserOnlyPath(
        (user_data / L"data" / L"lexicon").wstring(), true));
    const auto private_file = user_data / L"data" / L"lexicon" / L"user_dict.txt";
    { std::ofstream file(private_file); file << "nihao\tprivate\t1\n"; }
    CHECK(shuru::EnsureAppContainerAccess(private_file.wstring(), shuru::AclInheritance::None, true));
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

    for (const wchar_t* writable : {L"logs", L"ui_requests"}) {
        const std::wstring path = (user_data / writable).wstring();
        CHECK(MentionsAllPackages(ReadDacl(path)));
        CHECK(ReadDacl(path).find(L"0x1301bf") != std::wstring::npos);
    }
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"skins").wstring())));
    CHECK(DeniesAllPackages(ReadDacl((user_data / L"data").wstring())));
    CHECK(!MentionsAllPackages(ReadDacl(private_file.wstring())));
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"snapshot").wstring())));

    // 剪贴板历史可能含密码，任何沙箱应用都不该读到——即便父目录已放行。
    const std::wstring clipboard_dacl =
        ReadDacl((user_data / L"clipboard").wstring());
    CHECK(DeniesAllPackages(clipboard_dacl));
    // Deny 必须排在继承下来的 Allow 之前才真正生效。
    CHECK(clipboard_dacl.find(L"(D;") < clipboard_dacl.find(L";;;AC)"));

    const std::wstring direct_commit_dacl = ReadDacl(
        (user_data / L"direct_commit_requests").wstring());
    CHECK(DeniesAllPackages(direct_commit_dacl));
    CHECK(direct_commit_dacl.find(L"(D;") <
          direct_commit_dacl.find(L";;;AC)"));

    // settings.ini 靠根目录的 OI+NP 继承拿到读权限，被原子替换后依然有效。
    CHECK(MentionsAllPackages(ReadDacl((user_data / L"settings.ini").wstring())));
    const auto public_file = user_data / L"snapshot" / L"public.bin";
    { std::ofstream file(public_file); file << "public system lexicon\n"; }
    CHECK(ProbeWithAppContainer(root, private_file, public_file));

    fs::remove_all(root, error);
    std::cout << "appcontainer acl ok\n";
    return 0;
}
