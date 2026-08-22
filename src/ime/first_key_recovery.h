#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shuru {

inline constexpr std::uint64_t kFirstKeyRecoveryWindowMs = 8000;
inline constexpr std::uint64_t kFirstKeyRecoveryPendingTimeoutMs = 1000;
inline constexpr std::size_t kFirstKeyRecoveryMaximumTextLength = 64;

constexpr bool IsRecoverableAsciiLetter(wchar_t character) noexcept {
    return (character >= L'a' && character <= L'z') ||
           (character >= L'A' && character <= L'Z');
}

constexpr bool IsRecoverableAsciiRun(
    std::wstring_view text, wchar_t expected_first_character) noexcept {
    if (text.empty() ||
        text.size() > kFirstKeyRecoveryMaximumTextLength ||
        text.front() != expected_first_character) {
        return false;
    }
    for (const wchar_t character : text) {
        if (!IsRecoverableAsciiLetter(character)) return false;
    }
    return true;
}

struct FirstKeyRecoverySignals {
    std::uint64_t now = 0;
    std::uint64_t trigger_tick = 0;
    bool copy_scope_matches = false;
    bool chinese_mode = false;
    bool has_composition = false;
    bool current_context = false;
    bool self_edit = false;
    bool single_text_range = false;
    bool single_ascii_letter = false;
    bool sensitive_context = false;
    bool recovery_pending = false;
};

enum class FirstKeyRecoveryDecision {
    Eligible,
    CopyNotObserved,
    CopyExpired,
    CopyScopeChanged,
    EnglishMode,
    CompositionActive,
    DifferentContext,
    SelfEdit,
    UnexpectedRangeCount,
    UnexpectedText,
    SensitiveContext,
    AlreadyPending,
};

constexpr FirstKeyRecoveryDecision EvaluateFirstKeyRecovery(
    const FirstKeyRecoverySignals& signals) noexcept {
    if (signals.trigger_tick == 0) {
        return FirstKeyRecoveryDecision::CopyNotObserved;
    }
    if (signals.now < signals.trigger_tick ||
        signals.now - signals.trigger_tick > kFirstKeyRecoveryWindowMs) {
        return FirstKeyRecoveryDecision::CopyExpired;
    }
    if (!signals.copy_scope_matches) {
        return FirstKeyRecoveryDecision::CopyScopeChanged;
    }
    if (!signals.chinese_mode) {
        return FirstKeyRecoveryDecision::EnglishMode;
    }
    if (signals.has_composition) {
        return FirstKeyRecoveryDecision::CompositionActive;
    }
    if (!signals.current_context) {
        return FirstKeyRecoveryDecision::DifferentContext;
    }
    if (signals.self_edit) {
        return FirstKeyRecoveryDecision::SelfEdit;
    }
    if (!signals.single_text_range) {
        return FirstKeyRecoveryDecision::UnexpectedRangeCount;
    }
    if (!signals.single_ascii_letter) {
        return FirstKeyRecoveryDecision::UnexpectedText;
    }
    if (signals.sensitive_context) {
        return FirstKeyRecoveryDecision::SensitiveContext;
    }
    if (signals.recovery_pending) {
        return FirstKeyRecoveryDecision::AlreadyPending;
    }
    return FirstKeyRecoveryDecision::Eligible;
}

}  // namespace shuru
