#pragma once

#include <InputScope.h>
#include <Windows.h>

#include <string>

namespace shuru {

enum class InputScopePrivacy { Unknown, Normal, Sensitive };
bool IsSensitiveInputScope(InputScope scope);
InputScopePrivacy ClassifyInputScopes(const InputScope* scopes, UINT count);

struct NumpadDecision {
    bool handled = false;
    std::wstring text;
};

NumpadDecision DecideNumpadKey(WPARAM key, bool num_lock, const std::string& composition);

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
    ShiftTapRelease Release(bool has_composition, bool sensitive_context) noexcept;
    void Reset() noexcept { armed = false; keydown_eaten = false; }
};

}  // namespace shuru
