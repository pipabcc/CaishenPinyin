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

    InputScope sensitive[] = {IS_DEFAULT, IS_PASSWORD};
    InputScope normal[] = {IS_EMAIL_SMTPEMAILADDRESS};
    CHECK(ClassifyInputScopes(sensitive, 2) == InputScopePrivacy::Sensitive);
    CHECK(ClassifyInputScopes(normal, 1) == InputScopePrivacy::Normal);
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
    CHECK(GetCandidatePagingDirection(VK_OEM_COMMA, false) == CandidatePagingDirection::Previous);
    CHECK(GetCandidatePagingDirection(VK_OEM_PERIOD, false) == CandidatePagingDirection::Next);
    CHECK(GetCandidatePagingDirection(VK_OEM_COMMA, true) == CandidatePagingDirection::None);
    CHECK(GetCandidatePagingDirection(VK_OEM_MINUS, true) == CandidatePagingDirection::None);

    Candidate predicted;
    predicted.text = L"短剑";
    predicted.pinyin = "duanjian";
    predicted.covered_input_len = 4;
    const auto predicted_plan = PlanCandidateCommit("duan", predicted);
    CHECK(predicted_plan.learned_input == "duan");
    CHECK(predicted_plan.learned_pinyin == "duanjian");

    const auto row = BuildCandidateRowLayout(
        std::vector<int>(9, 48), 0, 13, 4, 8, 16);
    CHECK(row.size() == 9 && row.back().index == 8);
    CHECK(CandidateRowRequiredWidth(row, 9) == row.back().hit_right + 9);

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
