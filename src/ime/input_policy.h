#pragma once

#include <InputScope.h>
#include <Windows.h>

#include <string>

namespace shuru {

enum class InputScopePrivacy { Unknown, Normal, Sensitive };
bool IsSensitiveInputScope(InputScope scope);
InputScopePrivacy ClassifyInputScopes(const InputScope* scopes, UINT count);

// F10 modifies the current runtime schema. The persisted setting takes over
// only when its value actually changes afterwards.
struct SchemaSyncState {
    bool runtime_shuangpin = false;
    bool configured_shuangpin = false;

    void Initialize(bool configured) noexcept;
    void SetRuntime(bool shuangpin) noexcept;
    void ApplyConfigured(bool configured) noexcept;
};

constexpr bool IsVirtualKeyAlpha(WPARAM wparam) noexcept {
    return wparam >= 'A' && wparam <= 'Z';
}

constexpr bool IsFunctionKey(WPARAM wparam) noexcept {
    return wparam >= VK_F1 && wparam <= VK_F24;
}

constexpr bool IsShortcutModifierKey(WPARAM wparam) noexcept {
    return wparam == VK_CONTROL || wparam == VK_LCONTROL ||
           wparam == VK_RCONTROL || wparam == VK_MENU ||
           wparam == VK_LMENU || wparam == VK_RMENU ||
           wparam == VK_LWIN || wparam == VK_RWIN;
}

struct ShortcutModifierPhysicalState {
    bool left_control = false;
    bool right_control = false;
    bool left_alt = false;
    bool right_alt = false;
    bool left_windows = false;
    bool right_windows = false;
};

constexpr ShortcutModifierPhysicalState ConfirmShortcutModifierPhysicalState(
    const ShortcutModifierPhysicalState& queued,
    const ShortcutModifierPhysicalState& asynchronous) noexcept {
    return {
        queued.left_control && asynchronous.left_control,
        queued.right_control && asynchronous.right_control,
        queued.left_alt && asynchronous.left_alt,
        queued.right_alt && asynchronous.right_alt,
        queued.left_windows && asynchronous.left_windows,
        queued.right_windows && asynchronous.right_windows,
    };
}

constexpr WPARAM ResolveShortcutModifierSide(
    WPARAM key, LPARAM key_message) noexcept {
    const bool extended =
        (static_cast<ULONG_PTR>(key_message) & (ULONG_PTR {1} << 24)) != 0;
    if (key == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (key == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return key;
}

// TSF 的测试回调和真实回调并不总是成对到达。显式记录修饰键的
// 按下/释放世代，避免 Ctrl+C 松开后仍用消息队列中的旧键态放走首字母。
class ShortcutModifierState {
public:
    void KeyDown(WPARAM key, LPARAM key_message = 0) noexcept;
    void KeyUp(WPARAM key, LPARAM key_message = 0) noexcept;
    void Reset() noexcept;
    void ResetFromPhysical(
        const ShortcutModifierPhysicalState& physical) noexcept;
    bool IsActive(
        const ShortcutModifierPhysicalState& physical) noexcept;
    bool HasPressedModifier() const noexcept;

private:
    enum class Phase : std::uint8_t { Unknown, Released, Pressed };

    static bool Resolve(Phase* phase, bool physical_down) noexcept;

    Phase left_control_ = Phase::Unknown;
    Phase right_control_ = Phase::Unknown;
    Phase left_alt_ = Phase::Unknown;
    Phase right_alt_ = Phase::Unknown;
    Phase left_windows_ = Phase::Unknown;
    Phase right_windows_ = Phase::Unknown;
};

// Cross-process marker used by ShuruSettings when it injects its Ctrl+V paste
// sequence. It prevents the injected V from being interpreted as v-mode input.
inline constexpr ULONG_PTR kClipboardPasteInputMarker = 0x4350494DUL;  // "CPIM"

constexpr bool IsClipboardPasteInjectedShortcut(
    WPARAM wparam,
    ULONG_PTR extra_info) noexcept {
    return wparam == 'V' && extra_info == kClipboardPasteInputMarker;
}

struct NumpadDecision {
    bool handled = false;
    std::wstring text;
};

NumpadDecision DecideNumpadKey(WPARAM key, bool num_lock, const std::string& composition);

struct CalculatorKeyDecision {
    bool handled = false;
    char character = 0;
};

CalculatorKeyDecision DecideCalculatorKey(
    WPARAM key,
    bool shift_down,
    bool num_lock) noexcept;

enum class ShiftTapAction {
    None,
    CommitRawComposition,
    ToggleEnglishMode,
};

ShiftTapAction DecideShiftTapAction(
    bool armed,
    bool has_composition,
    bool sensitive_context) noexcept;

struct ShiftTapRelease {
    ShiftTapAction action = ShiftTapAction::None;
    bool eaten = false;
};

struct ShiftTapState {
    bool armed = false;
    bool keydown_eaten = false;

    bool Begin(bool has_composition, bool has_shortcut_modifier, bool repeated) noexcept;
    void CancelAction() noexcept { armed = false; }
    bool ShouldEatKeyUp() const noexcept { return keydown_eaten; }
    bool HasPendingKey() const noexcept { return armed || keydown_eaten; }
    bool ShouldRequestKeyUpCallback() const noexcept { return HasPendingKey(); }
    ShiftTapRelease TestKeyUp(bool sensitive_context) noexcept;
    ShiftTapRelease KeyUp(bool has_composition, bool sensitive_context) noexcept;
    ShiftTapRelease Release(bool has_composition, bool sensitive_context) noexcept;
    void Reset() noexcept { armed = false; keydown_eaten = false; }
};

}  // namespace shuru
