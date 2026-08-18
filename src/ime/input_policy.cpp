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

void SchemaSyncState::Initialize(bool configured) noexcept {
    runtime_shuangpin = configured;
    configured_shuangpin = configured;
}

void SchemaSyncState::SetRuntime(bool shuangpin) noexcept {
    runtime_shuangpin = shuangpin;
}

void SchemaSyncState::ApplyConfigured(bool configured) noexcept {
    if (configured != configured_shuangpin) {
        configured_shuangpin = configured;
        runtime_shuangpin = configured;
    }
}

void ShortcutModifierState::KeyDown(WPARAM key) noexcept {
    switch (key) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        control_ = Phase::Pressed;
        break;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        alt_ = Phase::Pressed;
        break;
    case VK_LWIN:
        left_windows_ = Phase::Pressed;
        break;
    case VK_RWIN:
        right_windows_ = Phase::Pressed;
        break;
    default:
        break;
    }
}

void ShortcutModifierState::KeyUp(WPARAM key) noexcept {
    switch (key) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        control_ = Phase::Released;
        break;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        alt_ = Phase::Released;
        break;
    case VK_LWIN:
        left_windows_ = Phase::Released;
        break;
    case VK_RWIN:
        right_windows_ = Phase::Released;
        break;
    default:
        break;
    }
}

void ShortcutModifierState::Reset() noexcept {
    control_ = Phase::Unknown;
    alt_ = Phase::Unknown;
    left_windows_ = Phase::Unknown;
    right_windows_ = Phase::Unknown;
}

void ShortcutModifierState::ResetFromPhysical(
    const ShortcutModifierPhysicalState& physical) noexcept {
    control_ = physical.control ? Phase::Pressed : Phase::Released;
    alt_ = physical.alt ? Phase::Pressed : Phase::Released;
    left_windows_ = physical.left_windows ? Phase::Pressed : Phase::Released;
    right_windows_ = physical.right_windows ? Phase::Pressed : Phase::Released;
}

bool ShortcutModifierState::Resolve(
    Phase* phase, bool physical_down) noexcept {
    if (phase == nullptr) return physical_down;
    if (*phase == Phase::Released) return false;
    if (!physical_down) {
        *phase = Phase::Released;
        return false;
    }
    if (*phase == Phase::Unknown) *phase = Phase::Pressed;
    return true;
}

bool ShortcutModifierState::IsActive(
    const ShortcutModifierPhysicalState& physical) noexcept {
    // 每次都同步全部修饰键，不能依赖 || 短路；否则 Ctrl 按下时 Alt/Win
    // 的释放状态不会被消费，下一键可能继续沿用旧状态。
    const bool control = Resolve(&control_, physical.control);
    const bool alt = Resolve(&alt_, physical.alt);
    const bool left_windows = Resolve(&left_windows_, physical.left_windows);
    const bool right_windows = Resolve(&right_windows_, physical.right_windows);
    return control || alt || left_windows || right_windows;
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
    // 无组合时 Shift 的 KeyDown 必须继续交给宿主，但 TestKeyUp 仍需返回 true，
    // 才能让严格遵循 TSF 流程的宿主继续调用真实 OnKeyUp。真实回调再按
    // keydown_eaten 决定是否吞键，因此应用不会收到缺少 KeyUp 的 Shift。
    return {
        ShiftTapAction::None,
        !sensitive_context && ShouldRequestKeyUpCallback(),
    };
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
