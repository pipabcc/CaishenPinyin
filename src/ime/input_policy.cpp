#include "input_policy.h"

namespace shuru {

bool IsSensitiveInputScope(InputScope scope) {
    switch (scope) {
    case IS_PASSWORD:
    case IS_PRIVATE:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
        return true;
    default:
        return false;
    }
}

InputScopePrivacy ClassifyInputScopes(const InputScope* scopes, UINT count) {
    if (scopes == nullptr || count == 0) return InputScopePrivacy::Unknown;
    for (UINT i = 0; i < count; ++i) {
        if (IsSensitiveInputScope(scopes[i])) return InputScopePrivacy::Sensitive;
    }
    return InputScopePrivacy::Normal;
}

NumpadDecision DecideNumpadKey(WPARAM key, bool num_lock, const std::string& composition) {
    NumpadDecision result;
    auto prefix = [&]() { result.text.assign(composition.begin(), composition.end()); };
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        if (!num_lock) return result;  // navigation semantics belong to the host
        result.handled = true;
        prefix();
        result.text.push_back(static_cast<wchar_t>(L'0' + key - VK_NUMPAD0));
        return result;
    }
    wchar_t output = 0;
    switch (key) {
    case VK_DECIMAL: output = L'.'; break;
    case VK_DIVIDE: output = L'/'; break;
    case VK_MULTIPLY: output = L'*'; break;
    case VK_ADD: output = L'+'; break;
    case VK_SUBTRACT: output = L'-'; break;
    default: return result;
    }
    result.handled = true;
    prefix();
    result.text.push_back(output);
    return result;
}

CalculatorKeyDecision DecideCalculatorKey(
    WPARAM key,
    bool shift_down,
    bool num_lock) noexcept {
    CalculatorKeyDecision decision;
    if (key >= '0' && key <= '9') {
        decision.handled = true;
        if (shift_down && key == '8') decision.character = '*';
        else if (shift_down && key == '9') decision.character = '(';
        else if (shift_down && key == '0') decision.character = ')';
        else if (!shift_down) decision.character = static_cast<char>(key);
        else decision.handled = false;
        return decision;
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        if (!num_lock) return decision;
        decision.handled = true;
        decision.character = static_cast<char>('0' + key - VK_NUMPAD0);
        return decision;
    }
    switch (key) {
    case VK_DECIMAL: decision.character = '.'; break;
    case VK_OEM_PERIOD: decision.character = shift_down ? 0 : '.'; break;
    case VK_DIVIDE: decision.character = '/'; break;
    case VK_OEM_2: decision.character = shift_down ? 0 : '/'; break;
    case VK_MULTIPLY: decision.character = '*'; break;
    case VK_ADD: decision.character = '+'; break;
    case VK_SUBTRACT: decision.character = '-'; break;
    case VK_OEM_MINUS: decision.character = shift_down ? 0 : '-'; break;
    case VK_OEM_PLUS: decision.character = shift_down ? '+' : 0; break;
    default: return decision;
    }
    decision.handled = decision.character != 0;
    return decision;
}

ShiftTapAction DecideShiftTapAction(
    bool armed,
    bool has_composition,
    bool sensitive_context) noexcept {
    if (!armed || sensitive_context) return ShiftTapAction::None;
    return has_composition
        ? ShiftTapAction::CommitRawComposition
        : ShiftTapAction::ToggleEnglishMode;
}

bool ShiftTapState::Begin(
    bool has_composition,
    bool has_shortcut_modifier,
    bool repeated) noexcept {
    if (!repeated) {
        armed = !has_shortcut_modifier;
        keydown_eaten = armed && has_composition;
    }
    return keydown_eaten;
}

ShiftTapRelease ShiftTapState::TestKeyUp(bool sensitive_context) noexcept {
    if (ShouldEatKeyUp()) return {ShiftTapAction::None, true};
    return Release(false, sensitive_context);
}

ShiftTapRelease ShiftTapState::KeyUp(
    bool has_composition,
    bool sensitive_context) noexcept {
    if (!HasPendingKey()) return {};
    return Release(has_composition, sensitive_context);
}

ShiftTapRelease ShiftTapState::Release(
    bool has_composition,
    bool sensitive_context) noexcept {
    ShiftTapRelease release {
        DecideShiftTapAction(armed, has_composition, sensitive_context),
        keydown_eaten,
    };
    Reset();
    return release;
}

}  // namespace shuru
