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
