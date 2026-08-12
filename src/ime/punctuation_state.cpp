#include "punctuation_state.h"

#include <cwctype>

namespace shuru {
namespace {

bool IsTokenChar(wchar_t ch) {
    return std::iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'@' || ch == L'.';
}

std::wstring TailToken(const std::wstring& text) {
    size_t start = text.size();
    while (start > 0 && IsTokenChar(text[start - 1])) --start;
    return text.substr(start);
}

bool LooksLikeUrlOrEmail(const std::wstring& text) {
    size_t start = text.size();
    while (start > 0 && !std::iswspace(text[start - 1])) --start;
    const std::wstring token = text.substr(start);
    return token.find(L'@') != std::wstring::npos || token.find(L"www.") == 0 ||
           token.find(L"http://") == 0 || token.find(L"https://") == 0 ||
           token.find(L"ftp://") == 0;
}

bool DecimalContext(const std::wstring& text) {
    return !text.empty() && std::iswdigit(text.back());
}

}  // namespace

void ChinesePunctuationState::Reset() {
    double_quote_open_ = true;
    single_quote_open_ = true;
}

std::wstring ChinesePunctuationState::Translate(
    WPARAM key, bool shift, bool full_width, const std::wstring& preceding_text) {
    if (!full_width) return {};
    const bool url = LooksLikeUrlOrEmail(preceding_text);
    switch (key) {
    case VK_OEM_COMMA: return shift ? L"《" : L"，";
    case VK_OEM_PERIOD:
        if (!shift && (DecimalContext(preceding_text) || url)) return L".";
        return shift ? L"》" : L"。";
    case VK_OEM_1: return shift ? (url ? L":" : L"：") : L"；";
    case VK_OEM_2: return shift ? L"？" : (url ? L"/" : L"、");
    case VK_OEM_4: return shift ? L"【" : L"「";
    case VK_OEM_6: return shift ? L"】" : L"」";
    case VK_OEM_5: return shift ? L"｜" : (url ? L"\\" : L"、");
    case VK_OEM_7:
        if (shift) {
            const wchar_t ch = double_quote_open_ ? L'“' : L'”';
            double_quote_open_ = !double_quote_open_;
            return std::wstring(1, ch);
        } else {
            const wchar_t ch = single_quote_open_ ? L'‘' : L'’';
            single_quote_open_ = !single_quote_open_;
            return std::wstring(1, ch);
        }
    case VK_OEM_MINUS: return shift ? L"——" : L"-";
    case VK_OEM_PLUS: return shift ? L"+" : L"=";
    case VK_OEM_3: return shift ? L"～" : L"·";
    case '1': return shift ? L"！" : L"";
    case '4': return shift ? L"￥" : L"";
    case '6': return shift ? L"……" : L"";
    case '9': return shift ? L"（" : L"";
    case '0': return shift ? L"）" : L"";
    default: return {};
    }
}

}  // namespace shuru
