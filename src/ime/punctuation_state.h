#pragma once

#include <Windows.h>
#include <string>

namespace shuru {

class ChinesePunctuationState {
public:
    void Reset();
    std::wstring Translate(WPARAM key, bool shift, bool full_width,
                           const std::wstring& preceding_text);

private:
    bool double_quote_open_ = true;
    bool single_quote_open_ = true;
};

}  // namespace shuru
