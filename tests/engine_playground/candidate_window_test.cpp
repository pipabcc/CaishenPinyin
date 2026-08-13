#include "ime/ui/candidate_window.h"

#include <Windows.h>

#include <cstdio>
#include <vector>

namespace {

BOOL CALLBACK FindCandidateWindow(HWND window, LPARAM value) {
    wchar_t class_name[64] {};
    if (GetClassNameW(window, class_name, ARRAYSIZE(class_name)) > 0 &&
        wcscmp(class_name, L"ShuruCandidateWindowClass") == 0) {
        *reinterpret_cast<HWND*>(value) = window;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int required_width = shuru::CandidateHeaderRequiredWidth(
        200, 50, 13, 9, 12);
    if (required_width != 284) {
        std::fwprintf(stderr, L"candidate header width=%d expected=284\n", required_width);
        return 1;
    }
    const auto header = shuru::BuildCandidateHeaderLayout(
        required_width, 50, 13, 9, 12);
    if (header.composing_left != 13 || header.composing_right != 213 ||
        header.page_left != 225 || header.page_right != 275) {
        std::fwprintf(stderr, L"candidate header rectangles are invalid\n");
        return 2;
    }
    if (shuru::CandidateMetadataFontSize(19) != 15 ||
        shuru::CandidateMetadataFontSize(14) != 11) {
        std::fwprintf(stderr, L"candidate metadata font sizing is invalid\n");
        return 3;
    }

    shuru::CandidateWindow window;
    if (!window.Create(GetModuleHandleW(nullptr))) return 4;

    std::vector<shuru::Candidate> candidates(18);
    for (size_t i = 0; i < candidates.size(); ++i) {
        candidates[i].text = L"候选" + std::to_wstring(i + 1);
    }
    window.SetContent(L"bao", candidates, 0, 0, 9);
    window.SetTypingStats(shuru::TypingStatsSnapshot {1286, true});
    window.Show(POINT {40, 40});

    HWND handle = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(), FindCandidateWindow, reinterpret_cast<LPARAM>(&handle));
    if (handle == nullptr) return 5;

    if (argc > 1 && wcscmp(argv[1], L"--preview") == 0) {
        const ULONGLONG deadline = GetTickCount64() + 30000;
        MSG message {};
        while (GetTickCount64() < deadline) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        window.Destroy();
        return 0;
    }

    const UINT dpi = (std::max)(UINT {96}, GetDpiForWindow(handle));
    const SIZE size = window.WindowSize();
    const int expected_height = MulDiv(74, static_cast<int>(dpi), 96);
    if (size.cy != expected_height) {
        std::fwprintf(stderr, L"candidate height=%d expected=%d\n", size.cy, expected_height);
        return 6;
    }

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (region == nullptr) return 7;
    const int region_type = GetWindowRgn(handle, region);
    const bool rounded = region_type == COMPLEXREGION &&
        !PtInRegion(region, 0, 0) &&
        !PtInRegion(region, size.cx - 1, 0) &&
        !PtInRegion(region, 0, size.cy - 1) &&
        !PtInRegion(region, size.cx - 1, size.cy - 1) &&
        PtInRegion(region, size.cx / 2, size.cy / 2);
    DeleteObject(region);
    if (!rounded) {
        std::fwprintf(stderr, L"candidate window region is not rounded\n");
        return 8;
    }

    window.Hide();
    window.Destroy();
    std::wprintf(L"candidate window compact layout and rounded region passed\n");
    return 0;
}
