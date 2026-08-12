#pragma once

namespace shuru {

struct RuntimeConfig {
    bool offline = true;
    bool learning_enabled = true;
    bool content_logging_enabled = false;
    bool full_width_punctuation = true;
    bool english_default = false;
    bool fuzzy_enabled = true;
    bool fuzzy_initials = true;
    bool fuzzy_finals = true;
    bool fuzzy_missing_vowel = true;
    bool shuangpin_xiaohe = false;
    int candidate_count = 9;
    int candidate_font_size = 19;
};

// Thread-safe, process-cached settings. Values are read from
// %LOCALAPPDATA%\FacaiPinyin\settings.ini; missing/invalid values use safe defaults.
RuntimeConfig GetRuntimeConfig();
void ReloadRuntimeConfig();

}  // namespace shuru
