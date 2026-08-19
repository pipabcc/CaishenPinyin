#include "ime/ui/candidate_window.h"
#include "ime/ui/directwrite_text_renderer.h"
#include "ime/ui/skin_manager.h"
#include "common/runtime_config.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
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
    wchar_t temporary_path[MAX_PATH] {};
    const DWORD temporary_length = GetTempPathW(
        ARRAYSIZE(temporary_path), temporary_path);
    if (temporary_length == 0 || temporary_length >= ARRAYSIZE(temporary_path)) return 29;
    wchar_t configured_test_data[MAX_PATH] {};
    const DWORD configured_length = GetEnvironmentVariableW(
        L"CAISHEN_CANDIDATE_TEST_LOCALAPPDATA", configured_test_data,
        ARRAYSIZE(configured_test_data));
    const std::wstring isolated_local_app_data = configured_length > 0 &&
            configured_length < ARRAYSIZE(configured_test_data)
        ? std::wstring(configured_test_data, configured_length)
        : std::wstring(temporary_path, temporary_length) +
            L"caishen-candidate-window-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(isolated_local_app_data.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", isolated_local_app_data.c_str());

    if (shuru::CandidateLayeredFontQuality() != ANTIALIASED_QUALITY ||
        shuru::CandidateLayeredFontQuality() == CLEARTYPE_QUALITY) {
        std::fwprintf(stderr, L"layered candidate window uses subpixel font quality\n");
        return 34;
    }

    if (shuru::CandidateVModeListFontSize(19) != 15 ||
        shuru::CandidateVModeListFontSize(14) != 14) {
        std::fwprintf(stderr, L"V-mode font hierarchy is invalid\n");
        return 35;
    }

    shuru::SkinTheme sampled_theme;
    sampled_theme.font_size = 18;
    sampled_theme.pinyin_margin = {1, 1, 1, 1};
    sampled_theme.candidate_margin = {1, 1, 1, 1};
    constexpr int sample_width = 24;
    constexpr int sample_height = 40;
    std::vector<std::uint8_t> sample_pixels(
        sample_width * sample_height * 4, 255);
    for (int y = 27; y < 39; ++y) {
        for (int x = 1; x < 23; ++x) {
            auto* pixel = sample_pixels.data() +
                (static_cast<size_t>(y) * sample_width + x) * 4;
            pixel[0] = 51;
            pixel[1] = 102;
            pixel[2] = 153;
            pixel[3] = 255;
        }
    }
    if (shuru::EstimateCandidateBackgroundColorFromPixels(
            sample_pixels.data(), sample_width, sample_height,
            sample_width * 4, sampled_theme) != RGB(153, 102, 51)) {
        std::fwprintf(stderr, L"candidate-area background sampling failed\n");
        return 41;
    }
    if (shuru::ResolveSkinOverlayPosition(
            shuru::SkinOverlayAnchor::End, 507, 150, 7, 15) != 342 ||
        shuru::ResolveSkinOverlayPosition(
            shuru::SkinOverlayAnchor::End, 175, 150, 0, 9) != 16 ||
        shuru::ResolveSkinOverlayPosition(
            shuru::SkinOverlayAnchor::End, 1014, 300, 14, 30) != 684 ||
        shuru::ResolveSkinOverlayPosition(
            shuru::SkinOverlayAnchor::Start, 507, 150, 7, 15) != 7) {
        std::fwprintf(stderr, L"overlay anchor resolution is invalid\n");
        return 45;
    }

    shuru::DirectWriteTextRenderer directwrite_text;
    if (!directwrite_text.IsAvailable() ||
        directwrite_text.ResolveFontFamily(L"font-that-does-not-exist") !=
            L"Microsoft YaHei UI") {
        std::fwprintf(stderr, L"DirectWrite or font fallback is unavailable\n");
        return 36;
    }
    const shuru::CandidateTextStyle render_style {
        L"Microsoft YaHei UI", 19.0f,
        shuru::CandidateTextWeight::Regular,
        shuru::CandidateTextAlignment::Near, true};
    for (const UINT render_dpi : {96U, 104U, 120U, 144U, 192U}) {
        constexpr int render_width = 180;
        constexpr int render_height = 48;
        std::vector<std::uint8_t> rendered(
            render_width * render_height * 4, 0);
        const RECT render_rect {0, 0, render_width, render_height};
        if (!directwrite_text.DrawText(
                rendered.data(), render_width, render_height, render_rect,
                L"候选文字 Abc", RGB(17, 24, 39), render_style,
                render_dpi)) {
            std::fwprintf(stderr, L"DirectWrite failed at %u DPI\n", render_dpi);
            return 37;
        }
        bool found_coverage = false;
        for (size_t index = 0; index < rendered.size() / 4; ++index) {
            const auto blue = rendered[index * 4];
            const auto green = rendered[index * 4 + 1];
            const auto red = rendered[index * 4 + 2];
            const auto alpha = rendered[index * 4 + 3];
            found_coverage = found_coverage || alpha != 0;
            if (blue > alpha || green > alpha || red > alpha) {
                std::fwprintf(stderr, L"text layer is not premultiplied\n");
                return 38;
            }
        }
        if (!found_coverage || directwrite_text.MeasureText(
                L"候选文字 Abc", render_style, render_dpi) <= 0.0f) {
            std::fwprintf(stderr, L"DirectWrite output is empty\n");
            return 39;
        }
    }

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

    constexpr int composite_width = 33;
    constexpr int composite_height = 33;
    std::vector<std::uint8_t> surface_alpha(
        composite_width * composite_height, 0);
    for (int y = 12; y <= 20; ++y) {
        for (int x = 12; x <= 20; ++x) {
            surface_alpha[static_cast<size_t>(y) * composite_width + x] = 255;
        }
    }
    surface_alpha[static_cast<size_t>(12) * composite_width + 12] = 128;
    std::vector<std::uint8_t> pixels(
        static_cast<size_t>(composite_width) * composite_height * 4, 0);
    shuru::CompositeCandidateSurfaceAndShadow(
        pixels.data(), surface_alpha, composite_width, composite_height,
        0, 0, 0, 0);
    for (size_t index = 0; index < surface_alpha.size(); ++index) {
        if (pixels[index * 4 + 3] != surface_alpha[index]) {
            std::fwprintf(stderr, L"surface alpha was not preserved\n");
            return 30;
        }
    }

    std::fill(pixels.begin(), pixels.end(), std::uint8_t {0});
    shuru::CompositeCandidateSurfaceAndShadow(
        pixels.data(), surface_alpha, composite_width, composite_height,
        2, 4, 3, 44);
    bool found_outside_shadow = false;
    bool found_shadow_gradient = false;
    std::uint8_t first_shadow_alpha = 0;
    for (size_t index = 0; index < surface_alpha.size(); ++index) {
        if (surface_alpha[index] != 0) continue;
        const std::uint8_t alpha = pixels[index * 4 + 3];
        if (alpha == 255) {
            std::fwprintf(stderr, L"shadow contains an opaque outside ring\n");
            return 31;
        }
        if (alpha == 0) continue;
        found_outside_shadow = true;
        if (first_shadow_alpha == 0) {
            first_shadow_alpha = alpha;
        } else if (alpha != first_shadow_alpha) {
            found_shadow_gradient = true;
        }
    }
    if (!found_outside_shadow || !found_shadow_gradient) {
        std::fwprintf(stderr, L"shadow alpha does not form a diffuse gradient\n");
        return 32;
    }

    const int required_width = shuru::CandidateHeaderRequiredWidth(
        200, 50, 13, 9, 12);
    if (required_width != 284) {
        std::fwprintf(stderr, L"candidate header width=%d expected=284\n", required_width);
        return 1;
    }
    const std::vector<int> diary_widths(9, 36);
    const auto diary_row = shuru::BuildCandidateRowLayout(
        diary_widths, diary_widths, 0, 46, 4, 18, 12);
    const int diary_window_width = shuru::CandidateRowRequiredWidth(
        diary_row, 140);
    for (const auto& item : diary_row) {
        const int text_right = shuru::CandidateItemTextRight(
            diary_window_width, item, 140);
        if (text_right <= item.text_left) {
            std::fwprintf(stderr,
                L"native skin right margin clipped candidate text\n");
            return 40;
        }
    }
    const RECT pin_rect = shuru::BuildCandidatePinRect(
        diary_row.front(), diary_window_width, 140, 42, 72);
    if (pin_rect.right - pin_rect.left != 18 || pin_rect.top != 42 ||
        pin_rect.bottom != 72 ||
        pin_rect.left != diary_row.front().highlight_right) {
        std::fwprintf(stderr, L"candidate pin rectangle is invalid\n");
        return 50;
    }
    if (diary_row.front().highlight_left != 42 ||
        diary_row.front().highlight_right != 86 ||
        diary_row[1].text_left - diary_row.front().text_left != 66) {
        std::fwprintf(stderr, L"candidate compact bounds are invalid\n");
        return 51;
    }

    const std::vector<std::vector<int>> expanded_widths {
        {60, 20, 40},
        {30, 50, 35},
    };
    const auto expanded_columns = shuru::BuildCandidateColumnWidths(
        expanded_widths);
    const auto expanded_first = shuru::BuildCandidateRowLayout(
        expanded_widths[0], expanded_columns, 0, 13, 4, 18, 12);
    const auto expanded_second = shuru::BuildCandidateRowLayout(
        expanded_widths[1], expanded_columns, 3, 13, 4, 18, 12);
    for (std::size_t column = 0; column < expanded_first.size(); ++column) {
        if (expanded_first[column].text_left !=
                expanded_second[column].text_left ||
            expanded_first[column].pin_left !=
                expanded_second[column].pin_left ||
            expanded_first[column].hit_left !=
                expanded_second[column].hit_left) {
            std::fwprintf(stderr, L"expanded candidate columns are not aligned\n");
            return 52;
        }
    }
    if (expanded_first[1].highlight_right ==
        expanded_second[1].highlight_right) {
        std::fwprintf(stderr, L"candidate highlight did not follow text width\n");
        return 53;
    }

    // 选中胶囊在序号左侧和候选词右侧各留内边距。项间距必须大于两倍
    // 内边距，否则相邻胶囊会咬合；首项胶囊也不能越出窗口左边。
    // 数值需与 candidate_window.h 的 kHighlightPaddingX/kCandidateItemGap 一致。
    constexpr int kHighlightPaddingX = 8;
    constexpr int kCandidateItemGap = 18;
    const std::vector<int> capsule_widths(4, 52);
    const auto capsule_row = shuru::BuildCandidateRowLayout(
        capsule_widths, capsule_widths, 0, 13, kHighlightPaddingX, 0,
        kCandidateItemGap);
    if (capsule_row.front().highlight_left < 0) {
        std::fwprintf(stderr,
            L"first candidate highlight overflows the window left edge\n");
        return 54;
    }
    for (std::size_t slot = 0; slot < capsule_row.size(); ++slot) {
        const auto& item = capsule_row[slot];
        if (item.highlight_left != item.text_left - kHighlightPaddingX ||
            item.highlight_right != item.text_right + kHighlightPaddingX) {
            std::fwprintf(stderr,
                L"candidate highlight padding is not applied\n");
            return 55;
        }
        if (slot + 1 < capsule_row.size() &&
            capsule_row[slot + 1].highlight_left <= item.highlight_right) {
            std::fwprintf(stderr, L"adjacent candidate highlights overlap\n");
            return 56;
        }
    }
    // 行宽必须把末项右侧的胶囊内边距一起算进去，否则末字会被裁掉。
    if (shuru::CandidateRowRequiredWidth(capsule_row, 9) <
        capsule_row.back().highlight_right + 9) {
        std::fwprintf(stderr,
            L"candidate row width excludes the trailing highlight padding\n");
        return 57;
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

    // KillTimer 不能移除已经排队的 WM_TIMER。旧消息必须被唯一 ID 忽略，
    // 不能抢先执行后来设置的候选窗定位回调。
    int deferred_calls = 0;
    int deferred_value = 0;
    window.StartDeferredAction([&]() {
        ++deferred_calls;
        deferred_value = 1;
    }, 1);
    Sleep(40);
    MSG queued_timer {};
    if (!PeekMessageW(
            &queued_timer, window.GetHwnd(), WM_TIMER, WM_TIMER,
            PM_NOREMOVE)) {
        std::fwprintf(stderr, L"deferred timer message was not queued\n");
        return 46;
    }
    window.StartDeferredAction([&]() {
        ++deferred_calls;
        deferred_value = 2;
    }, 40);
    while (PeekMessageW(
            &queued_timer, window.GetHwnd(), WM_TIMER, WM_TIMER,
            PM_REMOVE)) {
        DispatchMessageW(&queued_timer);
    }
    if (deferred_calls != 0) {
        std::fwprintf(stderr, L"stale deferred timer executed current action\n");
        return 47;
    }
    const ULONGLONG deferred_deadline = GetTickCount64() + 500;
    while (deferred_calls == 0 && GetTickCount64() < deferred_deadline) {
        while (PeekMessageW(
                &queued_timer, window.GetHwnd(), 0, 0, PM_REMOVE)) {
            TranslateMessage(&queued_timer);
            DispatchMessageW(&queued_timer);
        }
        Sleep(2);
    }
    if (deferred_calls != 1 || deferred_value != 2) {
        std::fwprintf(stderr,
                      L"deferred timer coalescing failed: calls=%d value=%d\n",
                      deferred_calls, deferred_value);
        return 48;
    }

    int shift_poll_calls = 0;
    window.StartShiftReleasePolling([&shift_poll_calls]() {
        ++shift_poll_calls;
        return false;
    });
    const ULONGLONG shift_poll_deadline = GetTickCount64() + 500;
    while (shift_poll_calls == 0 && GetTickCount64() < shift_poll_deadline) {
        while (PeekMessageW(
                &queued_timer, window.GetHwnd(), 0, 0, PM_REMOVE)) {
            TranslateMessage(&queued_timer);
            DispatchMessageW(&queued_timer);
        }
        Sleep(2);
    }
    if (shift_poll_calls != 1) {
        std::fwprintf(stderr,
                      L"shift release polling calls=%d expected=1\n",
                      shift_poll_calls);
        return 49;
    }

    int shortcut_poll_calls = 0;
    window.StartShortcutReleasePolling([&shortcut_poll_calls]() {
        ++shortcut_poll_calls;
        return false;
    });
    const ULONGLONG shortcut_poll_deadline = GetTickCount64() + 500;
    while (shortcut_poll_calls == 0 &&
           GetTickCount64() < shortcut_poll_deadline) {
        while (PeekMessageW(
                &queued_timer, window.GetHwnd(), 0, 0, PM_REMOVE)) {
            TranslateMessage(&queued_timer);
            DispatchMessageW(&queued_timer);
        }
        Sleep(2);
    }
    if (shortcut_poll_calls != 1) {
        std::fwprintf(stderr,
                      L"shortcut release polling calls=%d expected=1\n",
                      shortcut_poll_calls);
        return 54;
    }

    std::vector<shuru::Candidate> candidates(63);
    for (size_t i = 0; i < candidates.size(); ++i) {
        candidates[i].text = L"候选" + std::to_wstring(i + 1);
    }
    window.SetContent(L"bao", candidates, 0, 0, 9);
    window.SetPinningEnabled(true);
    window.SetTypingStats(shuru::TypingStatsSnapshot {1286, true});
    const SIZE size_before_show = window.WindowSize();
    window.Show(POINT {40, 40});

    HWND handle = nullptr;
    EnumThreadWindows(
        GetCurrentThreadId(), FindCandidateWindow, reinterpret_cast<LPARAM>(&handle));
    if (handle == nullptr) return 5;

    if (argc > 1 && wcscmp(argv[1], L"--preview") == 0) {
        const ULONGLONG deadline = GetTickCount64() + 8000;
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
    const int shadow_margin = MulDiv(16, static_cast<int>(dpi), 96);
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

    int pin_clicks = 0;
    int selection_clicks = 0;
    window.SetPinHandler([&pin_clicks](size_t) { ++pin_clicks; });
    window.SetSelectionHandler(
        [&selection_clicks](size_t) { ++selection_clicks; });
    const int candidate_row_y = shadow_margin +
        MulDiv(5 + 30 + 4 + 15, static_cast<int>(dpi), 96);
    bool found_pin_target = false;
    for (int x = shadow_margin; x < shadow_margin + size.cx; ++x) {
        pin_clicks = 0;
        selection_clicks = 0;
        const LPARAM point = MAKELPARAM(x, candidate_row_y);
        SendMessageW(handle, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(handle, WM_LBUTTONUP, 0, point);
        if (pin_clicks == 0) continue;
        if (pin_clicks != 1 || selection_clicks != 0) {
            std::fwprintf(
                stderr,
                L"pin click dispatched pin=%d selection=%d\n",
                pin_clicks, selection_clicks);
            return 51;
        }
        found_pin_target = true;
        break;
    }
    if (!found_pin_target) {
        std::fwprintf(stderr, L"candidate pin target was not clickable\n");
        return 52;
    }
    window.SetPinHandler({});
    window.SetSelectionHandler({});
    window.SetSelectedIndex(0);

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
    window.SetContent(L"v", candidates, 17, 0, 9, true, true);
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
    window.SetContent(L"vv", candidates, 0, 0, 9, true, true);
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
    window.SetContent(L"vfilter", candidates, 0, 0, 9, true, true);
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

    // 规范化 SSF 使用素材自身透明轮廓、原生最小尺寸和逐帧动画，
    // 不应再套默认阴影或 74px 卡片高度。
    wchar_t local_app_data[MAX_PATH] {};
    const DWORD local_length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data, ARRAYSIZE(local_app_data));
    const std::wstring skin_root = std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin\\skins\\candidate-test-native";
    const std::wstring overlay_root = skin_root + L"\\overlay_frames";
    const std::wstring overlay_frames_root = overlay_root + L"\\0";
    CreateDirectoryW((std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin").c_str(), nullptr);
    CreateDirectoryW((std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin\\skins").c_str(), nullptr);
    CreateDirectoryW(skin_root.c_str(), nullptr);
    CreateDirectoryW(overlay_root.c_str(), nullptr);
    CreateDirectoryW(overlay_frames_root.c_str(), nullptr);
    wchar_t module_path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    std::wstring module_directory(module_path);
    module_directory.resize(module_directory.find_last_of(L"\\/"));
    const std::wstring source_frame =
        module_directory + L"\\data\\skins\\classic_blue\\cand_bg.png";
    CopyFileW(source_frame.c_str(), (skin_root + L"\\background.png").c_str(), FALSE);
    CopyFileW(source_frame.c_str(),
        (overlay_frames_root + L"\\frame_000.png").c_str(), FALSE);
    CopyFileW(source_frame.c_str(),
        (overlay_frames_root + L"\\frame_001.png").c_str(), FALSE);
    {
        std::ofstream ini(skin_root + L"\\skin.ini", std::ios::binary);
        ini << "[Display]\nfont_family=Microsoft YaHei UI\nfont_size=18\n"
               "[Scheme_H1]\nbg_image=background.png\n"
               "layout_horizontal=0,16,16\nlayout_vertical=0,16,16\n"
               "pinyin_margin=20,2,24,20\ncandidate_margin=4,8,24,70\n"
               "native_appearance=1\nhas_shadow=0\nshow_separator=0\n"
               "native_min_width=420\nnative_min_height=120\n"
               "[AnimationOverlays]\ncount=1\n"
               "[AnimationOverlay0]\nframe_count=2\n"
               "horizontal_anchor=end\nvertical_anchor=end\n"
               "margin_left=0\nmargin_top=0\nmargin_right=10\nmargin_bottom=5\n"
               "frame_0=overlay_frames\\0\\frame_000.png\ndelay_0=80\n"
               "frame_1=overlay_frames\\0\\frame_001.png\ndelay_1=80\n";
    }
    const std::wstring settings_path = std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin\\settings.ini";
    {
        std::ofstream settings(settings_path, std::ios::binary);
        settings << "SkinId=candidate-test-native\n";
    }
    shuru::ReloadRuntimeConfig();
    shuru::CandidateWindow native_window;
    if (!native_window.Create(GetModuleHandleW(nullptr))) return 26;
    native_window.SetContent(L"bao", candidates, 0, 0, 9);
    native_window.Show(POINT {40, 40});
    auto& native_skin = shuru::SkinManager::Instance();
    if (!native_skin.CurrentTheme().native_appearance ||
        !native_skin.CurrentTheme().is_user_skin ||
        native_skin.CurrentTheme().has_shadow || !native_skin.HasAnimation() ||
        native_skin.CurrentTheme().native_width != 420 ||
        native_skin.CurrentTheme().native_height != 120 ||
        native_skin.CurrentFrameDelayMs() != 80 || !native_skin.AdvanceFrame()) {
        std::fwprintf(stderr, L"native overlay animation metadata is invalid\n");
        return 26;
    }
    const SIZE native_size = native_window.WindowSize();
    RECT native_rect {};
    GetWindowRect(native_window.GetHwnd(), &native_rect);
    if (native_size.cx < MulDiv(280, static_cast<int>(dpi), 96) ||
        native_size.cy < MulDiv(74, static_cast<int>(dpi), 96) ||
        native_rect.right - native_rect.left != native_size.cx ||
        native_rect.bottom - native_rect.top != native_size.cy) {
        std::fwprintf(stderr, L"native skin size or shadow policy is invalid\n");
        return 27;
    }

    // 导入皮肤的 v/vv/vvv 工具页都只使用候选区域主色；只有 v/vv
    // 切换为竖向列表，普通候选仍保持素材原生尺寸和动画元数据。
    const COLORREF sampled_background =
        native_skin.CurrentTheme().utility_background_color;
    native_window.SetContent(L"v", candidates, 0, 0, 9, true, true);
    native_window.Show(POINT {40, 40});
    const SIZE imported_v_size = native_window.WindowSize();
    RECT imported_v_rect {};
    GetWindowRect(native_window.GetHwnd(), &imported_v_rect);
    const int expected_v_shadow = MulDiv(16, static_cast<int>(dpi), 96);
    if (imported_v_rect.right - imported_v_rect.left !=
            imported_v_size.cx + expected_v_shadow * 2 ||
        imported_v_rect.bottom - imported_v_rect.top !=
            imported_v_size.cy + expected_v_shadow * 2) {
        std::fwprintf(stderr, L"imported skin v-mode shadow margin is invalid\n");
        return 44;
    }
    native_window.SetContent(L"vvv1+2", candidates, 0, 0, 9, true, false);
    native_window.Show(POINT {40, 40});
    const SIZE imported_vvv_size = native_window.WindowSize();
    if (!shuru::ShouldUsePlainUtilityBackground(
            true, native_skin.CurrentTheme().is_user_skin) ||
        sampled_background !=
            native_skin.CurrentTheme().utility_background_color ||
        !native_window.UsesPlainUtilityBackgroundForTesting() ||
        native_window.IsSkinAnimationTimerActiveForTesting() ||
        imported_v_size.cx != MulDiv(350, static_cast<int>(dpi), 96) ||
        imported_vvv_size.cx == imported_v_size.cx) {
        std::fwprintf(stderr, L"imported utility skin mode policy is invalid\n");
        return 42;
    }
    native_window.SetContent(L"bao", candidates, 0, 0, 9, false, false);
    native_window.Show(POINT {40, 40});
    RECT restored_rect {};
    GetWindowRect(native_window.GetHwnd(), &restored_rect);
    if (native_window.UsesPlainUtilityBackgroundForTesting() ||
        !native_window.IsSkinAnimationTimerActiveForTesting() ||
        restored_rect.right - restored_rect.left != native_window.WindowSize().cx ||
        restored_rect.bottom - restored_rect.top != native_window.WindowSize().cy) {
        std::fwprintf(stderr, L"normal imported skin did not restore shadowless layout\n");
        return 43;
    }

    {
        std::ofstream settings(settings_path, std::ios::binary);
        settings << "SkinId=classic_gold\n";
    }
    SendMessageW(
        native_window.GetHwnd(), shuru::RuntimeSettingsChangedMessage(), 0, 0);
    RECT reloaded_rect {};
    GetWindowRect(native_window.GetHwnd(), &reloaded_rect);
    const int reloaded_shadow_margin = MulDiv(16, static_cast<int>(dpi), 96);
    if (shuru::GetRuntimeConfig().skin_id != L"classic_gold" ||
        native_skin.CurrentTheme().id != L"classic_gold" ||
        !native_skin.CurrentTheme().has_shadow || native_skin.HasAnimation() ||
        reloaded_rect.right - reloaded_rect.left !=
            native_window.WindowSize().cx + reloaded_shadow_margin * 2 ||
        reloaded_rect.bottom - reloaded_rect.top !=
            native_window.WindowSize().cy + reloaded_shadow_margin * 2) {
        std::fwprintf(stderr, L"runtime skin reload message did not apply immediately\n");
        return 33;
    }
    native_window.Destroy();
    DeleteFileW(settings_path.c_str());
    DeleteFileW((skin_root + L"\\background.png").c_str());
    DeleteFileW((overlay_frames_root + L"\\frame_000.png").c_str());
    DeleteFileW((overlay_frames_root + L"\\frame_001.png").c_str());
    DeleteFileW((skin_root + L"\\skin.ini").c_str());
    RemoveDirectoryW(overlay_frames_root.c_str());
    RemoveDirectoryW(overlay_root.c_str());
    RemoveDirectoryW(skin_root.c_str());
    RemoveDirectoryW((std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin\\skins").c_str());
    RemoveDirectoryW((std::wstring(local_app_data, local_length) +
        L"\\CaishenPinyin").c_str());
    RemoveDirectoryW(isolated_local_app_data.c_str());
    std::wprintf(L"candidate shadow, runtime skin reload, and v/vv scrolling passed\n");
    return 0;
}
