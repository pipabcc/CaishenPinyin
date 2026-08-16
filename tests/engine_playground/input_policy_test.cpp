#include "ime/activation_state.h"
#include "ime/input_policy.h"
#include "ime/punctuation_state.h"
#include "ime/ui/ime_ui_logic.h"

#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "check failed line " << __LINE__ << '\n'; return 1; } } while (0)

int main() {
    using namespace shuru;
    CHECK(CandidateQueryLimit(9, false) == 90);
    CHECK(CandidateQueryLimit(3, false) == 30);
    CHECK(CandidateQueryLimit(9, true) == 256);
    CHECK(CandidateQueryLimit(3, true) == 256);

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

    CHECK(IsVerticalUtilityMode("v"));
    CHECK(IsVerticalUtilityMode("v123"));
    CHECK(IsVerticalUtilityMode("vv"));
    CHECK(!IsVerticalUtilityMode("vvv"));
    CHECK(!IsVerticalUtilityMode("vvv1+2"));
    CHECK(IsVerticalUtilityMode("Vabc"));
    CHECK(!IsVerticalUtilityMode(""));
    CHECK(!IsVerticalUtilityMode("lv"));
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
    CHECK(IsReliableCandidateRect(valid_rect));
    CHECK(!IsReliableCandidateRect(valid_rect, true));
    CHECK(!IsReliableCandidateRect(flat_rect));
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

    const auto row = BuildCandidateRowLayout(
        std::vector<int>(9, 48), 0, 13, 4, 8, 16);
    CHECK(row.size() == 9 && row.back().index == 8);
    CHECK(CandidateRowRequiredWidth(row, 9) == row.back().hit_right + 9);
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
    std::cout << "input_policy: OK\n";
    return 0;
}
