#include "ime/activation_state.h"
#include "ime/first_key_recovery.h"
#include "ime/input_policy.h"
#include "ime/punctuation_state.h"
#include "ime/ui/ime_ui_logic.h"

#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)

int main() {
    using namespace shuru;
    CHECK(IsRecoverableAsciiLetter(L'j'));
    CHECK(IsRecoverableAsciiLetter(L'Z'));
    CHECK(!IsRecoverableAsciiLetter(L'1'));
    CHECK(!IsRecoverableAsciiLetter(L'中'));
    CHECK(IsRecoverableAsciiRun(L"ni", L'n'));
    CHECK(!IsRecoverableAsciiRun(L"ni1", L'n'));
    CHECK(!IsRecoverableAsciiRun(L"ni", L'h'));
    const std::wstring maximum_recovery_text(
        kFirstKeyRecoveryMaximumTextLength, L'a');
    CHECK(IsRecoverableAsciiRun(maximum_recovery_text, L'a'));
    CHECK(!IsRecoverableAsciiRun(
        maximum_recovery_text + L'a', L'a'));

    FirstKeyRecoverySignals recovery {
        1500, 1000, true,
        true, false, true, false, true, true, false, false};
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::Eligible);
    recovery.now = recovery.trigger_tick + kFirstKeyRecoveryWindowMs;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::Eligible);
    recovery.now = 1500;
    recovery.trigger_tick = 0;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::CopyNotObserved);
    recovery.trigger_tick = 1000;
    recovery.now = 1000 + kFirstKeyRecoveryWindowMs + 1;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::CopyExpired);
    recovery.now = 1500;
    recovery.copy_scope_matches = false;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::CopyScopeChanged);
    // 复制上下文仍一致时不依赖额外的焦点重建通知。
    recovery.copy_scope_matches = true;
    recovery.trigger_tick = 1200;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::Eligible);
    recovery.chinese_mode = false;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::EnglishMode);
    recovery.chinese_mode = true;
    recovery.has_composition = true;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::CompositionActive);
    recovery.has_composition = false;
    recovery.current_context = false;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::DifferentContext);
    recovery.current_context = true;
    recovery.self_edit = true;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::SelfEdit);
    recovery.self_edit = false;
    recovery.single_text_range = false;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::UnexpectedRangeCount);
    recovery.single_text_range = true;
    recovery.single_ascii_letter = false;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::UnexpectedText);
    recovery.single_ascii_letter = true;
    recovery.sensitive_context = true;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::SensitiveContext);
    recovery.sensitive_context = false;
    recovery.recovery_pending = true;
    CHECK(EvaluateFirstKeyRecovery(recovery) ==
          FirstKeyRecoveryDecision::AlreadyPending);

    CHECK(CandidateQueryLimit(9, false) == 90);
    CHECK(CandidateQueryLimit(3, false) == 30);
    CHECK(CandidateQueryLimit(9, true) == 256);
    CHECK(CandidateQueryLimit(3, true) == 256);

    SchemaSyncState schema;
    schema.Initialize(false);
    CHECK(!schema.runtime_shuangpin && !schema.configured_shuangpin);
    schema.SetRuntime(true);
    schema.ApplyConfigured(false);
    CHECK(schema.runtime_shuangpin);  // unchanged setting must not undo F10
    schema.ApplyConfigured(true);
    CHECK(schema.runtime_shuangpin && schema.configured_shuangpin);
    schema.SetRuntime(false);
    schema.ApplyConfigured(true);
    CHECK(!schema.runtime_shuangpin);  // F10 remains active after later queries
    schema.ApplyConfigured(false);
    CHECK(!schema.runtime_shuangpin && !schema.configured_shuangpin);

    ChinesePunctuationState punctuation;
    CHECK(punctuation.Translate(VK_OEM_COMMA, false, true, L"") == L"，");
    CHECK(punctuation.Translate(VK_OEM_7, true, true, L"") == L"“");
    CHECK(punctuation.Translate(VK_OEM_7, true, true, L"") == L"”");
    CHECK(punctuation.Translate(VK_OEM_7, false, true, L"") == L"‘");
    CHECK(punctuation.Translate(VK_OEM_7, false, true, L"") == L"’");
    CHECK(punctuation.Translate('6', true, true, L"") == L"……");
    CHECK(punctuation.Translate(VK_OEM_MINUS, true, true, L"") == L"——");
    CHECK(punctuation.Translate('4', true, true, L"") == L"￥");
    CHECK(punctuation.Translate(VK_OEM_PERIOD, false, true, L"12") == L".");
    CHECK(punctuation.Translate(VK_OEM_2, false, true, L"https://a") == L"/");
    CHECK(punctuation.Translate(VK_OEM_PERIOD, false, false, L"").empty());
    CHECK(punctuation.Translate(VK_OEM_1, true, true, L"https://host") == L":");
    CHECK(punctuation.Translate(VK_OEM_1, true, true, L"中文") == L"：");
    CHECK(punctuation.Translate(VK_OEM_5, false, true, L"https://host") == L"\\");
    punctuation.Reset();
    CHECK(punctuation.Translate(VK_OEM_7, true, true, L"") == L"“");

    for (WPARAM key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key) {
        const auto on = DecideNumpadKey(key, true, "ni");
        CHECK(on.handled && on.text.size() == 3 && on.text[2] == L'0' + key - VK_NUMPAD0);
        CHECK(!DecideNumpadKey(key, false, "ni").handled);
    }
    CHECK(DecideNumpadKey(VK_DECIMAL, false, "ni").text == L"ni.");
    CHECK(DecideNumpadKey(VK_DIVIDE, true, "").text == L"/");
    CHECK(DecideNumpadKey(VK_MULTIPLY, true, "").text == L"*");
    CHECK(DecideNumpadKey(VK_ADD, true, "").text == L"+");
    CHECK(DecideNumpadKey(VK_SUBTRACT, true, "").text == L"-");

    CHECK(!DecideNumpadKey(VK_INSERT, false, "ni").handled);
    CHECK(!DecideNumpadKey('1', true, "ni").handled);
    CHECK(DecideCalculatorKey('1', false, true).character == '1');
    CHECK(DecideCalculatorKey('8', true, true).character == '*');
    CHECK(DecideCalculatorKey('9', true, true).character == '(');
    CHECK(DecideCalculatorKey('0', true, true).character == ')');
    CHECK(DecideCalculatorKey(VK_OEM_PLUS, true, true).character == '+');
    CHECK(DecideCalculatorKey(VK_OEM_MINUS, false, true).character == '-');
    CHECK(DecideCalculatorKey(VK_OEM_2, false, true).character == '/');
    CHECK(!DecideCalculatorKey(VK_OEM_2, true, true).handled);
    CHECK(!DecideCalculatorKey(VK_OEM_PERIOD, true, true).handled);
    CHECK(!DecideCalculatorKey(VK_OEM_MINUS, true, true).handled);
    CHECK(DecideCalculatorKey(VK_NUMPAD7, false, true).character == '7');
    CHECK(!DecideCalculatorKey(VK_NUMPAD7, false, false).handled);
    CHECK(DecideShiftTapAction(true, true, false) == ShiftTapAction::CommitRawComposition);
    CHECK(DecideShiftTapAction(true, false, false) == ShiftTapAction::ToggleEnglishMode);
    CHECK(DecideShiftTapAction(false, true, false) == ShiftTapAction::None);
    CHECK(DecideShiftTapAction(true, true, true) == ShiftTapAction::None);
    ShiftTapState shift_tap;
    CHECK(shift_tap.Begin(true, false, false));
    CHECK(shift_tap.ShouldEatKeyUp() && shift_tap.HasPendingKey());
    auto shift_release = shift_tap.Release(true, false);
    CHECK(shift_release.eaten && shift_release.action == ShiftTapAction::CommitRawComposition);
    CHECK(!shift_tap.HasPendingKey());
    CHECK(!shift_tap.Begin(false, false, false));
    const ShiftTapRelease idle_test_key_up = shift_tap.TestKeyUp(false);
    CHECK(idle_test_key_up.eaten &&
          idle_test_key_up.action == ShiftTapAction::None);
    shift_release = shift_tap.Release(false, false);
    CHECK(!shift_release.eaten && shift_release.action == ShiftTapAction::ToggleEnglishMode);
    CHECK(shift_tap.Begin(true, false, false));
    shift_tap.CancelAction();
    shift_release = shift_tap.Release(true, false);
    CHECK(shift_release.eaten && shift_release.action == ShiftTapAction::None);
    CHECK(!shift_tap.Begin(true, true, false));
    shift_release = shift_tap.Release(true, false);
    CHECK(!shift_release.eaten && shift_release.action == ShiftTapAction::None);

    // 对应 TextService 的完整 TSF 回调顺序，提交动作只能在最终 KeyUp 产生。
    ShiftTapState shift_callbacks;
    const bool test_key_down = shift_callbacks.Begin(true, false, false);
    const bool key_down = shift_callbacks.ShouldEatKeyUp();
    const ShiftTapRelease test_key_up = shift_callbacks.TestKeyUp(false);
    const ShiftTapRelease key_up = shift_callbacks.KeyUp(true, false);
    CHECK(test_key_down && key_down);
    CHECK(test_key_up.eaten && test_key_up.action == ShiftTapAction::None);
    CHECK(key_up.eaten && key_up.action == ShiftTapAction::CommitRawComposition);
    CHECK(!shift_callbacks.HasPendingKey());

    // 严格 TSF 宿主：无组合时 TestKeyDown 放行 Shift，TestKeyUp 必须请求
    // 真实回调；真实 KeyUp 切换模式但仍放行给宿主。
    ShiftTapState strict_host_shift;
    CHECK(!strict_host_shift.Begin(false, false, false));
    const ShiftTapRelease strict_test_key_up =
        strict_host_shift.TestKeyUp(false);
    CHECK(strict_test_key_up.eaten);
    const ShiftTapRelease strict_key_up =
        strict_host_shift.KeyUp(false, false);
    CHECK(strict_key_up.action == ShiftTapAction::ToggleEnglishMode &&
          !strict_key_up.eaten && !strict_host_shift.HasPendingKey());

    ShiftTapState shifted_shortcut;
    CHECK(!shifted_shortcut.Begin(false, false, false));
    shifted_shortcut.CancelAction();
    CHECK(!shifted_shortcut.TestKeyUp(false).eaten &&
          shifted_shortcut.KeyUp(false, false).action == ShiftTapAction::None);

    ShortcutModifierDecisionCache shortcut_cache;
    bool shortcut_decision = false;
    shortcut_cache.Store('Z', 0x1234, true, 100);
    CHECK(shortcut_cache.Consume(
        'Z', 0x1234, 150, &shortcut_decision));
    CHECK(shortcut_decision && !shortcut_cache.valid);
    CHECK(!shortcut_cache.Consume(
        'Z', 0x1234, 151, &shortcut_decision));
    shortcut_cache.Store('A', 1, true, 200);
    CHECK(!shortcut_cache.Consume('B', 1, 201, &shortcut_decision));
    CHECK(!shortcut_cache.Consume('A', 1, 202, &shortcut_decision));
    shortcut_cache.Store('A', 1, true, 1000);
    CHECK(!shortcut_cache.Consume('A', 1, 2001, &shortcut_decision));
    shortcut_cache.Store('A', 1, true, 2100);
    shortcut_cache.Clear();
    CHECK(!shortcut_cache.Consume('A', 1, 2101, &shortcut_decision));
    shortcut_cache.Store('A', 1, false, 0xFFFFFFF0u);
    CHECK(shortcut_cache.Consume('A', 1, 5, &shortcut_decision));
    CHECK(!shortcut_decision);

    ShortcutModifierState modifiers;
    ShortcutModifierPhysicalState physical;
    CHECK(!modifiers.IsActive(physical));
    const ShortcutModifierPhysicalState queued_control {
        true, true, false, false, false, false};
    const ShortcutModifierPhysicalState asynchronous_control {
        true, false, false, false, false, false};
    const auto confirmed_control = ConfirmShortcutModifierPhysicalState(
        queued_control, asynchronous_control);
    CHECK(confirmed_control.left_control &&
          !confirmed_control.right_control);
    const auto released_after_focus_switch =
        ConfirmShortcutModifierPhysicalState(
            ShortcutModifierPhysicalState {}, asynchronous_control);
    CHECK(!released_after_focus_switch.left_control &&
          !released_after_focus_switch.right_control);
    modifiers.KeyDown(VK_CONTROL);
    physical.left_control = true;
    CHECK(modifiers.IsActive(physical));
    modifiers.KeyUp(VK_CONTROL);
    // Ctrl+C 松开后的线程键态即使暂时仍显示按下，也不能放走下一字母。
    CHECK(!modifiers.IsActive(physical));
    physical.left_control = false;
    CHECK(!modifiers.IsActive(physical));
    modifiers.KeyDown(VK_CONTROL);
    physical.left_control = true;
    CHECK(modifiers.IsActive(physical));
    modifiers.KeyUp(VK_CONTROL);
    physical.left_control = false;
    physical.left_alt = true;
    modifiers.Reset();
    CHECK(modifiers.IsActive(physical));
    physical.left_alt = false;
    CHECK(!modifiers.IsActive(physical));
    physical.left_windows = true;
    physical.right_windows = true;
    modifiers.ResetFromPhysical(physical);
    CHECK(modifiers.IsActive(physical));
    modifiers.KeyUp(VK_LWIN);
    modifiers.KeyUp(VK_RWIN);
    CHECK(!modifiers.IsActive(physical));

    // Ctrl+C 被宿主接管且不回调 Ctrl KeyUp：释放轮询观察到左 Ctrl
    // 已物理弹起后必须清除旧世代，随后输入 J 应进入拼音组合。
    ShortcutModifierState missing_ctrl_key_up;
    ShortcutModifierPhysicalState copy_physical;
    missing_ctrl_key_up.KeyDown(VK_CONTROL, 0);
    copy_physical.left_control = true;
    CHECK(missing_ctrl_key_up.IsActive(copy_physical));
    CHECK(missing_ctrl_key_up.HasPressedModifier());
    copy_physical.left_control = false;
    CHECK(!missing_ctrl_key_up.IsActive(copy_physical));
    CHECK(!missing_ctrl_key_up.HasPressedModifier());
    // 已确认释放的世代不能被一次滞后的物理键态重新激活。
    copy_physical.left_control = true;
    CHECK(!missing_ctrl_key_up.IsActive(copy_physical));

    // 扫描码扩展位区分左右 Ctrl；持续按住右 Ctrl 时仍保持快捷键。
    ShortcutModifierState held_right_ctrl;
    constexpr LPARAM kExtendedKeyMessage = LPARAM {1} << 24;
    CHECK(ResolveShortcutModifierSide(VK_CONTROL, 0) == VK_LCONTROL);
    CHECK(ResolveShortcutModifierSide(
        VK_CONTROL, kExtendedKeyMessage) == VK_RCONTROL);
    CHECK(ResolveShortcutModifierSide(
        VK_MENU, kExtendedKeyMessage) == VK_RMENU);
    held_right_ctrl.KeyDown(VK_CONTROL, kExtendedKeyMessage);
    ShortcutModifierPhysicalState held_physical;
    held_physical.right_control = true;
    CHECK(held_right_ctrl.IsActive(held_physical));
    CHECK(held_right_ctrl.HasPressedModifier());
    held_right_ctrl.KeyUp(VK_CONTROL, kExtendedKeyMessage);
    CHECK(!held_right_ctrl.IsActive(held_physical));
    CHECK(IsShortcutModifierKey(VK_CONTROL));
    CHECK(IsShortcutModifierKey(VK_RMENU));
    CHECK(IsShortcutModifierKey(VK_LWIN));
    CHECK(!IsShortcutModifierKey(VK_SHIFT));

    CHECK(IsUtilityMode("v"));
    CHECK(IsUtilityMode("vvv1+2"));
    CHECK(IsUtilityMode("VVV"));
    CHECK(!IsUtilityMode("lv"));
    CHECK(IsVerticalUtilityMode("v"));
    CHECK(IsVerticalUtilityMode("v123"));
    CHECK(IsVerticalUtilityMode("vv"));
    CHECK(!IsVerticalUtilityMode("vvv"));
    CHECK(!IsVerticalUtilityMode("vvv1+2"));
    CHECK(!IsVerticalUtilityMode("VVV1+2"));
    CHECK(IsVerticalUtilityMode("Vabc"));
    CHECK(!IsVerticalUtilityMode(""));
    CHECK(!IsVerticalUtilityMode("lv"));
    CHECK(ShouldUsePlainUtilityBackground(true, true));
    CHECK(!ShouldUsePlainUtilityBackground(true, false));
    CHECK(!ShouldUsePlainUtilityBackground(false, true));
    char filter_digit = 0;
    CHECK(TryGetVerticalUtilityFilterDigit('0', false, &filter_digit));
    CHECK(filter_digit == '0');
    CHECK(TryGetVerticalUtilityFilterDigit(
        VK_NUMPAD9, true, &filter_digit));
    CHECK(filter_digit == '9');
    CHECK(!TryGetVerticalUtilityFilterDigit(
        VK_NUMPAD9, false, &filter_digit));
    CHECK(!TryGetVerticalUtilityFilterDigit('A', true, &filter_digit));

    InputScope sensitive[] = {IS_DEFAULT, IS_PASSWORD};
    InputScope normal[] = {IS_EMAIL_SMTPEMAILADDRESS, IS_SEARCH, IS_URL};
    CHECK(ClassifyInputScopes(sensitive, 2) == InputScopePrivacy::Sensitive);
    CHECK(ClassifyInputScopes(normal, 3) == InputScopePrivacy::Normal);
    CHECK(ClassifyInputScopes(nullptr, 0) == InputScopePrivacy::Unknown);

    CandidatePageState candidates;
    candidates.total = 12;
    candidates.page_size = 5;
    candidates.MovePrevious();
    CHECK(candidates.selected == 11 && candidates.page == 2);
    candidates.MoveNext();
    CHECK(candidates.selected == 0 && candidates.page == 0);
    candidates.Select(6);
    CHECK(candidates.selected == 6 && candidates.page == 1);
    candidates.NextPage();
    CHECK(candidates.selected == 10 && candidates.page == 2);
    candidates.PreviousPage();
    CHECK(candidates.selected == 5 && candidates.page == 1);
    CHECK(candidates.GlobalIndex(4) == 9 && candidates.IsSelectableSlot(4));
    candidates.page_size = 0;
    candidates.Select(3);
    CHECK(candidates.PageSize() == 1 && candidates.page == 3);

    CandidatePageState expanded_candidates;
    expanded_candidates.total = 12;
    expanded_candidates.page_size = 5;
    expanded_candidates.Select(3);
    expanded_candidates.MoveRowDown();
    CHECK(expanded_candidates.selected == 8 && expanded_candidates.page == 1);
    expanded_candidates.MoveRowDown();
    CHECK(expanded_candidates.selected == 11 && expanded_candidates.page == 2);
    expanded_candidates.MoveRowDown();
    CHECK(expanded_candidates.selected == 11 && expanded_candidates.page == 2);
    expanded_candidates.MoveRowUp();
    CHECK(expanded_candidates.selected == 6 && expanded_candidates.page == 1);
    expanded_candidates.Select(1);
    expanded_candidates.MoveRowUp();
    CHECK(expanded_candidates.selected == 1 && expanded_candidates.page == 0);

    std::vector<Candidate> display_candidates(2);
    display_candidates[0].input_segmentation = "wo'men'zhi'dao";
    display_candidates[1].input_segmentation = "wo'men'xiang'xin";
    CHECK(CandidateComposingDisplay(
        display_candidates, 0, L"womenzhidao") == L"wo'men'zhi'dao");
    CHECK(CandidateComposingDisplay(
        display_candidates, 1, L"womenzhidao") == L"wo'men'xiang'xin");
    display_candidates[1].input_segmentation.clear();
    CHECK(CandidateComposingDisplay(
        display_candidates, 1, L"womenzhidao") == L"womenzhidao");

    const RECT valid_rect {10, 20, 11, 40};
    const RECT flat_rect {10, 20, 11, 20};
    const RECT host_rect {100, 100, 900, 700};
    const RECT point_host_rect {346, 266, 346, 266};
    CHECK(IsReliableCandidateRect(valid_rect));
    CHECK(!IsReliableCandidateRect(valid_rect, true));
    CHECK(!IsReliableCandidateRect(flat_rect));
    CHECK(IsUsableCandidateHostRect(host_rect));
    CHECK(!IsUsableCandidateHostRect(point_host_rect));
    CHECK(IsCandidateRectPlausibleForHost(
        RECT {120, 130, 121, 150}, host_rect));
    CHECK(IsCandidateRectPlausibleForHost(
        RECT {50, 80, 51, 100}, host_rect));
    CHECK(!IsCandidateRectPlausibleForHost(valid_rect, host_rect));
    CHECK(!IsCandidateRectPlausibleForHost(flat_rect, host_rect));
    CHECK(!IsCandidateRectPlausibleForHost(valid_rect, point_host_rect));
    CHECK(GetCandidatePagingDirection(VK_PRIOR, true) == CandidatePagingDirection::Previous);
    CHECK(GetCandidatePagingDirection(VK_NEXT, false) == CandidatePagingDirection::Next);
    CHECK(GetCandidatePagingDirection(VK_OEM_COMMA, false) == CandidatePagingDirection::None);
    CHECK(GetCandidatePagingDirection(VK_OEM_PERIOD, false) == CandidatePagingDirection::None);
    CHECK(GetCandidatePagingDirection(VK_OEM_COMMA, true) == CandidatePagingDirection::None);
    CHECK(GetCandidatePagingDirection(VK_OEM_MINUS, false) == CandidatePagingDirection::Previous);
    CHECK(GetCandidatePagingDirection(VK_OEM_PLUS, false) == CandidatePagingDirection::Next);
    CHECK(GetCandidatePagingDirection(VK_OEM_MINUS, true) == CandidatePagingDirection::None);
    CHECK(GetCandidateRowDirection(VK_UP, true) == CandidateRowDirection::Up);
    CHECK(GetCandidateRowDirection(VK_DOWN, false) == CandidateRowDirection::Down);
    CHECK(GetCandidateRowDirection(VK_OEM_COMMA, false) == CandidateRowDirection::Up);
    CHECK(GetCandidateRowDirection(VK_OEM_PERIOD, false) == CandidateRowDirection::Down);
    CHECK(GetCandidateRowDirection(VK_OEM_COMMA, true) == CandidateRowDirection::None);
    CHECK(GetCandidateRowDirection(VK_OEM_PERIOD, true) == CandidateRowDirection::None);

    Candidate predicted;
    predicted.text = L"短剑";
    predicted.pinyin = "duanjian";
    predicted.covered_input_len = 4;
    const auto predicted_plan = PlanCandidateCommit("duan", predicted);
    CHECK(predicted_plan.learned_input == "duan");
    CHECK(predicted_plan.learned_pinyin == "duanjian");

    Candidate clipboard_candidate;
    clipboard_candidate.text = L"第一行...";
    clipboard_candidate.full_content = L"第一行\r\n第二行";
    clipboard_candidate.covered_input_len = 1;
    const auto clipboard_plan = PlanCandidateCommit(
        "v", clipboard_candidate);
    CHECK(clipboard_plan.has_coverage && clipboard_plan.remaining.empty());
    CHECK(clipboard_plan.committed == L"第一行\r\n第二行");
    CHECK(CandidateCommitText(clipboard_candidate) ==
          clipboard_candidate.full_content);

    const std::vector<int> row_widths(9, 48);
    const auto row = BuildCandidateRowLayout(
        row_widths, row_widths, 0, 13, 4, 18, 12);
    CHECK(row.size() == 9 && row.back().index == 8);
    CHECK(CandidateRowRequiredWidth(row, 9) == row.back().hit_right + 9);
    CHECK(CandidateItemTextRight(710, row.front(), 140) ==
          row.front().highlight_right);
    CandidateItemLayout clipped_item;
    clipped_item.text_left = 560;
    clipped_item.text_right = 700;
    clipped_item.highlight_right = 704;
    CHECK(CandidateItemTextRight(710, clipped_item, 140) == 570);
    CHECK(CandidateExpandedFirstPage(0, 5) == 0);
    CHECK(CandidateExpandedFirstPage(4, 5) == 0);
    CHECK(CandidateExpandedFirstPage(5, 5) == 5);
    CHECK(CandidateExpandedRowCount(90, 9, 0, 5) == 5);
    CHECK(CandidateExpandedRowCount(63, 9, 5, 5) == 2);

    const auto compact_window = BuildCandidateWindowVerticalLayout(5, 30, 4);
    CHECK(compact_window.composing_top == 5 && compact_window.composing_bottom == 35);
    CHECK(compact_window.separator_y == 37 && compact_window.candidate_top == 39);
    CHECK(compact_window.candidate_bottom == 69 && compact_window.window_height == 74);

    ActivationState state;
    CHECK(state.Add(ActivationResource::ThreadManager));
    CHECK(state.Add(ActivationResource::Engine));
    CHECK(state.Add(ActivationResource::KeySink));
    const auto rollback = state.RollbackOrder();
    CHECK(rollback.size() == 3 && rollback[0] == ActivationResource::KeySink &&
          rollback[2] == ActivationResource::ThreadManager);
    state.Clear();
    CHECK(state.Empty());
    // 验证字母虚拟键码与功能键判定
    for (WPARAM vk = 'A'; vk <= 'Z'; ++vk) {
        CHECK(IsVirtualKeyAlpha(vk));
        CHECK(!IsFunctionKey(vk));
    }
    for (WPARAM vk = VK_F1; vk <= VK_F24; ++vk) {
        CHECK(!IsVirtualKeyAlpha(vk));
        CHECK(IsFunctionKey(vk));
    }
    for (WPARAM vk = 'a'; vk <= 'z'; ++vk) {
        // 虚拟键码不应包含小写 ASCII 字符
        CHECK(!IsVirtualKeyAlpha(vk));
    }
    CHECK(!IsVirtualKeyAlpha(VK_SPACE));
    CHECK(!IsVirtualKeyAlpha(VK_RETURN));
    CHECK(!IsVirtualKeyAlpha(VK_ESCAPE));
    CHECK(!IsVirtualKeyAlpha(VK_TAB));
    CHECK(IsClipboardPasteInjectedShortcut(
        'V', kClipboardPasteInputMarker));
    CHECK(!IsClipboardPasteInjectedShortcut(
        'A', kClipboardPasteInputMarker));
    CHECK(!IsClipboardPasteInjectedShortcut('V', 0));

    std::cout << "input_policy: OK\n";
    return 0;
}
