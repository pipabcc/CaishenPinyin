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

    std::vector<shuru::Candidate> candidates(63);
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
    const int shadow_margin = MulDiv(12, static_cast<int>(dpi), 96);
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

    if (window.IsExpanded() || !window.ToggleExpanded()) {
        std::fwprintf(stderr, L"candidate did not expand\n");
        return 21;
    }
    const SIZE expanded_size = window.WindowSize();
    const int expected_expanded_height = size.cy +
        4 * MulDiv(30, static_cast<int>(dpi), 96);
    if (!window.IsExpanded() ||
        expanded_size.cy != expected_expanded_height) {
        std::fwprintf(stderr,
            L"expanded height=%d expected=%d\n",
            expanded_size.cy, expected_expanded_height);
        return 22;
    }
    if (!window.ToggleExpanded() || window.IsExpanded() ||
        window.WindowSize().cy != size.cy) {
        std::fwprintf(stderr, L"candidate did not restore collapsed size\n");
        return 23;
    }

    // 箭头位于右上角字数统计之后，鼠标点击与 Tab 共用展开状态。
    window.Show(POINT {40, 40});
    const int arrow_x = shadow_margin + size.cx -
        MulDiv(9 + 8, static_cast<int>(dpi), 96);
    const int arrow_y = shadow_margin + MulDiv(
        5 + 15, static_cast<int>(dpi), 96);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(arrow_x, arrow_y));
    if (!window.IsExpanded()) {
        std::fwprintf(stderr, L"candidate arrow click did not expand\n");
        return 24;
    }
    size_t expanded_clicked_index = static_cast<size_t>(-1);
    window.SetSelectionHandler([&expanded_clicked_index](size_t index) {
        expanded_clicked_index = index;
    });
    const int expanded_second_row_x = shadow_margin +
        MulDiv(13 + 24, static_cast<int>(dpi), 96);
    const int expanded_second_row_y = shadow_margin +
        MulDiv(5 + 30 + 4 + 30 + 15, static_cast<int>(dpi), 96);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(expanded_second_row_x, expanded_second_row_y));
    SendMessageW(handle, WM_LBUTTONUP, 0,
                 MAKELPARAM(expanded_second_row_x, expanded_second_row_y));
    if (expanded_clicked_index != 9) {
        std::fwprintf(stderr,
            L"expanded second-row click=%zu expected=9\n",
            expanded_clicked_index);
        return 25;
    }
    window.SetSelectionHandler({});
    window.SetSelectedIndex(0);
    window.SetExpanded(false);

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

    // 隐藏后不能保留上一次的可见坐标；新组合的首帧必须直接
    // 提交到新光标位置，不得先在旧位置曝光一帧。
    window.Hide();
    RECT hidden_rect {};
    if (!GetWindowRect(handle, &hidden_rect) ||
        hidden_rect.left > -30000 || hidden_rect.top > -30000) {
        std::fwprintf(stderr, L"hidden candidate retained its previous anchor\n");
        return 19;
    }
    const POINT second_anchor {320, 180};
    window.Show(second_anchor);
    RECT second_rect {};
    if (!GetWindowRect(handle, &second_rect) ||
        second_rect.left != second_anchor.x - shadow_margin ||
        second_rect.top != second_anchor.y - shadow_margin) {
        std::fwprintf(stderr,
            L"candidate first frame origin=(%ld,%ld) expected=(%d,%d)\n",
            second_rect.left, second_rect.top,
            second_anchor.x - shadow_margin,
            second_anchor.y - shadow_margin);
        return 20;
    }

    size_t clicked_index = static_cast<size_t>(-1);
    window.SetSelectionHandler([&clicked_index](size_t index) {
        clicked_index = index;
    });
    window.SetContent(L"v", candidates, 17, 0, 9);
    const SIZE vertical_size = window.WindowSize();
    const int expected_vertical_height =
        MulDiv(38, static_cast<int>(dpi), 96) +
        10 * MulDiv(32, static_cast<int>(dpi), 96) +
        MulDiv(10, static_cast<int>(dpi), 96);
    if (vertical_size.cy != expected_vertical_height) {
        std::fwprintf(stderr,
            L"vertical candidate height=%d expected=%d\n",
            vertical_size.cy, expected_vertical_height);
        return 14;
    }
    window.Show(POINT {40, 40});

    const int first_row_x = shadow_margin + MulDiv(30, static_cast<int>(dpi), 96);
    const int first_row_y = shadow_margin + MulDiv(38 + 16, static_cast<int>(dpi), 96);
    const LPARAM first_row = MAKELPARAM(first_row_x, first_row_y);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, first_row);
    SendMessageW(handle, WM_LBUTTONUP, 0, first_row);
    if (clicked_index != 8) {
        std::fwprintf(stderr,
            L"vertical selection did not reveal index 17, first=%zu\n",
            clicked_index);
        return 15;
    }

    clicked_index = static_cast<size_t>(-1);
    window.SetContent(L"vv", candidates, 0, 0, 9);
    SendMessageW(
        handle, WM_MOUSEWHEEL,
        MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, first_row);
    SendMessageW(handle, WM_LBUTTONUP, 0, first_row);
    if (clicked_index != 3) {
        std::fwprintf(stderr,
            L"vertical mouse wheel offset=%zu expected=3\n", clicked_index);
        return 16;
    }

    clicked_index = static_cast<size_t>(-1);
    window.SetSelectedIndex(17);
    SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, first_row);
    SendMessageW(handle, WM_LBUTTONUP, 0, first_row);
    if (clicked_index != 8) {
        std::fwprintf(stderr,
            L"vertical selected item did not scroll into view, first=%zu\n",
            clicked_index);
        return 17;
    }

    int search_clicks = 0;
    int clear_clicks = 0;
    window.SetSearchHandler([&search_clicks]() { ++search_clicks; });
    window.SetClearSearchHandler([&clear_clicks]() { ++clear_clicks; });
    window.SetContent(L"vfilter", candidates, 0, 0, 9);
    window.Show(POINT {40, 40});
    const SIZE search_size = window.WindowSize();
    const int search_right = search_size.cx -
        MulDiv(12, static_cast<int>(dpi), 96);
    const int search_left = search_right -
        MulDiv(145, static_cast<int>(dpi), 96);
    const int search_y = shadow_margin +
        MulDiv(19, static_cast<int>(dpi), 96);
    SendMessageW(
        handle, WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(
            shadow_margin + search_left +
                MulDiv(10, static_cast<int>(dpi), 96),
            search_y));
    SendMessageW(
        handle, WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(
            shadow_margin + search_right -
                MulDiv(8, static_cast<int>(dpi), 96),
            search_y));
    if (search_clicks != 1 || clear_clicks != 1) {
        std::fwprintf(stderr,
            L"vertical search actions search=%d clear=%d\n",
            search_clicks, clear_clicks);
        return 18;
    }

    window.Hide();
    window.Destroy();
    std::wprintf(L"candidate layered shadow, first frame, and v/vv scrolling passed\n");
    return 0;
}
