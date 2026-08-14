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
    bool found_antialiased_corner = false;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t coverage =
                shuru::RoundedRectanglePixelCoverage(x, y, 0, 0, 20, 20, 8);
            if (coverage != shuru::RoundedRectanglePixelCoverage(
                                19 - x, y, 0, 0, 20, 20, 8) ||
                coverage != shuru::RoundedRectanglePixelCoverage(
                                x, 19 - y, 0, 0, 20, 20, 8)) {
                std::fwprintf(stderr, L"rounded mask corners are asymmetric\n");
                return 1;
            }
            found_antialiased_corner = found_antialiased_corner ||
                (coverage > 0 && coverage < 255);
        }
    }
    if (!found_antialiased_corner ||
        shuru::RoundedRectanglePixelCoverage(10, 10, 0, 0, 20, 20, 8) != 255 ||
        shuru::RoundedRectanglePixelCoverage(-1, 0, 0, 0, 20, 20, 8) != 0) {
        std::fwprintf(stderr, L"rounded mask antialiasing is invalid\n");
        return 1;
    }

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
    const SIZE size_before_show = window.WindowSize();
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
    if (size_before_show.cx != size.cx || size_before_show.cy != size.cy) {
        std::fwprintf(stderr, L"candidate layout changed during first Show\n");
        return 7;
    }

    const LONG_PTR extended_style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    if ((extended_style & WS_EX_LAYERED) == 0) {
        std::fwprintf(stderr, L"candidate window is not layered\n");
        return 8;
    }
    const ULONG_PTR class_style = GetClassLongPtrW(handle, GCL_STYLE);
    if ((class_style & CS_DROPSHADOW) != 0) {
        std::fwprintf(stderr, L"candidate window still uses CS_DROPSHADOW\n");
        return 9;
    }

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (region == nullptr) return 10;
    const int region_type = GetWindowRgn(handle, region);
    DeleteObject(region);
    if (region_type != ERROR) {
        std::fwprintf(stderr, L"candidate window still has a GDI region\n");
        return 11;
    }

    RECT actual_rect {};
    if (!GetWindowRect(handle, &actual_rect)) return 12;
    const int shadow_margin = MulDiv(12, static_cast<int>(dpi), 96);
    const int expected_actual_width = size.cx + shadow_margin * 2;
    const int expected_actual_height = size.cy + shadow_margin * 2;
    if (actual_rect.right - actual_rect.left != expected_actual_width ||
        actual_rect.bottom - actual_rect.top != expected_actual_height) {
        std::fwprintf(stderr,
            L"layered size=%dx%d expected=%dx%d\n",
            actual_rect.right - actual_rect.left,
            actual_rect.bottom - actual_rect.top,
            expected_actual_width,
            expected_actual_height);
        return 13;
    }

    window.Hide();
    window.Destroy();
    std::wprintf(L"candidate layered shadow and stable first frame passed\n");
    return 0;
}
