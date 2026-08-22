#include "engine/pinyin_engine.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr size_t kNotFound = static_cast<size_t>(-1);

size_t CandidateIndex(
    const shuru::EngineQueryResult& result, const std::wstring& text) {
    const auto found = std::find_if(
        result.candidates.begin(), result.candidates.end(),
        [&](const shuru::Candidate& candidate) {
            return candidate.is_english && candidate.text == text;
        });
    return found == result.candidates.end()
        ? kNotFound
        : static_cast<size_t>(std::distance(result.candidates.begin(), found));
}

bool IsChineseFirst(const shuru::EngineQueryResult& result) {
    return !result.candidates.empty() && !result.candidates.front().is_english;
}

shuru::QueryOptions EnglishOptions(
    shuru::EnglishCandidatePosition position,
    size_t page_size = 9,
    bool enabled = true) {
    shuru::QueryOptions options;
    options.english_mix_enabled = enabled;
    options.english_candidate_position = position;
    options.candidate_page_size = page_size;
    return options;
}

int Fail(int code, const char* message) {
    std::cerr << message << '\n';
    return code;
}

class ScopedLocalAppData final {
public:
    ScopedLocalAppData() {
        const DWORD original_length =
            GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (original_length != 0) {
            original_.resize(original_length);
            const DWORD written = GetEnvironmentVariableW(
                L"LOCALAPPDATA", original_.data(), original_length);
            if (written == 0 || written >= original_length) {
                original_.clear();
            } else {
                original_.resize(written);
                had_original_ = true;
            }
        }

        std::error_code error;
        const std::filesystem::path temporary_root =
            std::filesystem::temp_directory_path(error);
        if (error || temporary_root.empty()) return;
        directory_ = temporary_root /
            (L"caishen-english-ranking-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory_, error);
        if (error || !SetEnvironmentVariableW(
                L"LOCALAPPDATA", directory_.c_str())) {
            std::filesystem::remove_all(directory_, error);
            directory_.clear();
            return;
        }
        environment_changed_ = true;
    }

    ~ScopedLocalAppData() {
        if (environment_changed_) {
            SetEnvironmentVariableW(
                L"LOCALAPPDATA", had_original_ ? original_.c_str() : nullptr);
        }
        std::error_code error;
        if (!directory_.empty()) {
            std::filesystem::remove_all(directory_, error);
        }
    }

    ScopedLocalAppData(const ScopedLocalAppData&) = delete;
    ScopedLocalAppData& operator=(const ScopedLocalAppData&) = delete;

    bool valid() const noexcept { return environment_changed_; }

    std::filesystem::path user_root() const {
        return directory_ / L"CaishenPinyin";
    }

private:
    std::filesystem::path directory_;
    std::wstring original_;
    bool had_original_ = false;
    bool environment_changed_ = false;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;

    namespace fs = std::filesystem;
    ScopedLocalAppData isolated_local_app_data;
    if (!isolated_local_app_data.valid()) {
        return Fail(3, "isolated LOCALAPPDATA unavailable");
    }
    const fs::path user_root = isolated_local_app_data.user_root();

    shuru::PinyinEngine engine;
    if (!engine.Initialize(argv[1])) return Fail(4, "engine init failed");

    const auto easy = engine.Query("easy", 9);
    if (easy.candidates.size() < 5 || CandidateIndex(easy, L"easy") != 4) {
        return Fail(5, "default easy middle ranking failed");
    }
    const auto engli = engine.Query("engli", 9);
    if (engli.candidates.size() != 9 || CandidateIndex(engli, L"English") != 4) {
        return Fail(6, "default engli middle ranking failed");
    }
    const auto uppercase = engine.Query("ENGLI", 9);
    if (CandidateIndex(uppercase, L"English") != 4) {
        return Fail(7, "uppercase ranking failed");
    }

    const auto first = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::First));
    if (CandidateIndex(first, L"English") != 0) {
        return Fail(8, "first position failed");
    }
    const auto middle = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::Middle));
    if (middle.candidates.size() < 9 || CandidateIndex(middle, L"English") != 4) {
        return Fail(9, "middle position failed");
    }
    const auto last = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::Last));
    if (last.candidates.size() < 9 || CandidateIndex(last, L"English") != 8) {
        return Fail(10, "last position failed");
    }

    for (size_t page_size = 3; page_size <= 11; ++page_size) {
        const auto result = engine.Query(
            "engli", page_size * 10,
            EnglishOptions(shuru::EnglishCandidatePosition::Middle, page_size));
        const size_t actual_page_size = (std::min)(page_size, result.candidates.size());
        if (actual_page_size == 0 ||
            CandidateIndex(result, L"English") != actual_page_size / 2) {
            return Fail(11, "variable middle position failed");
        }
    }

    const auto short_page = engine.Query(
        "engli", 3,
        EnglishOptions(shuru::EnglishCandidatePosition::Middle, 9));
    if (short_page.candidates.size() != 3 ||
        CandidateIndex(short_page, L"English") != 1) {
        return Fail(12, "short page middle position failed");
    }

    const auto disabled = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::First, 9, false));
    if (std::any_of(
            disabled.candidates.begin(), disabled.candidates.end(),
            [](const shuru::Candidate& candidate) {
                return candidate.is_english;
            })) {
        return Fail(13, "disabled English mix still returned English candidates");
    }

    if (!IsChineseFirst(engine.Query("shi", 9)) ||
        !IsChineseFirst(engine.Query("nihao", 9)) ||
        !IsChineseFirst(engine.Query("women", 9)) ||
        !IsChineseFirst(engine.Query("nihoa", 9))) {
        return Fail(14, "Chinese protection failed");
    }

    const fs::path custom_path =
        user_root / L"data" / L"lexicon" / L"custom_phrases.txt";
    fs::create_directories(custom_path.parent_path());
    {
        std::ofstream output(custom_path, std::ios::binary);
        output << "engli\tCUSTOM ENGLISH PHRASE\t5\n";
    }
    if (!engine.ReloadCustomPhrases()) {
        return Fail(15, "custom phrase reload failed");
    }
    const auto custom = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::Middle));
    if (custom.candidates.size() < 6 ||
        custom.candidates[4].source != shuru::CandidateSource::CustomPhrase ||
        custom.candidates[4].text != L"CUSTOM ENGLISH PHRASE" ||
        CandidateIndex(custom, L"English") != 5) {
        return Fail(16, "custom phrase priority failed");
    }

    fs::remove(custom_path);
    if (!engine.ReloadCustomPhrases()) {
        return Fail(17, "empty custom phrase reload failed");
    }
    const auto before_pin = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::Middle));
    const auto pin_target = std::find_if(
        before_pin.candidates.begin(), before_pin.candidates.end(),
        [](const shuru::Candidate& candidate) {
            return !candidate.is_english &&
                candidate.source != shuru::CandidateSource::CustomPhrase;
        });
    if (pin_target == before_pin.candidates.end()) {
        return Fail(18, "pin target missing");
    }
    const std::wstring pinned_text = pin_target->text;
    if (engine.TogglePinnedCandidate(
            shuru::InputSchema::Quanpin, "engli", pinned_text) !=
        shuru::PinnedCandidateToggleResult::Pinned) {
        return Fail(19, "pin setup failed");
    }
    const auto pinned = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::First));
    if (pinned.candidates.empty() || !pinned.candidates.front().pinned ||
        pinned.candidates.front().text != pinned_text ||
        CandidateIndex(pinned, L"English") != 1) {
        return Fail(20, "pinned candidate priority failed");
    }

    if (engine.TogglePinnedCandidate(
            shuru::InputSchema::Quanpin, "engli", L"English") !=
        shuru::PinnedCandidateToggleResult::Pinned) {
        return Fail(21, "pinned English setup failed");
    }
    const auto pinned_english = engine.Query(
        "engli", 90,
        EnglishOptions(shuru::EnglishCandidatePosition::Last));
    if (pinned_english.candidates.empty() ||
        !pinned_english.candidates.front().pinned ||
        pinned_english.candidates.front().text != L"English") {
        return Fail(22, "pinned English did not remain first");
    }

    std::cout << "english_ranking: OK\n";
    return 0;
}
