#include "engine/pinned_candidate_store.h"

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "check failed at line " << __LINE__ << '\n';       \
            return 1;                                                        \
        }                                                                    \
    } while (false)

int wmain() {
    namespace fs = std::filesystem;
    wchar_t temporary_root[MAX_PATH] {};
    const DWORD length = GetTempPathW(ARRAYSIZE(temporary_root), temporary_root);
    CHECK(length > 0 && length < ARRAYSIZE(temporary_root));
    const fs::path root = fs::path(temporary_root) /
        (L"caishen-pinned-candidate-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path path = root / L"pinned_candidates.tsv";
    std::error_code error;
    fs::remove_all(root, error);

    shuru::PinnedCandidateStore first(path.wstring());
    shuru::PinnedCandidateStore observer(path.wstring());
    CHECK(!first.Lookup(shuru::PinnedCandidateSchema::Quanpin, "ni"));
    CHECK(!observer.Lookup(shuru::PinnedCandidateSchema::Quanpin, "ni"));

    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "NI", L"你") ==
        shuru::PinnedCandidateToggleResult::Pinned);
    CHECK(observer.Lookup(
        shuru::PinnedCandidateSchema::Quanpin, "ni") == L"你");

    std::vector<shuru::Candidate> candidates(3);
    candidates[0].text = L"泥";
    candidates[1].text = L"你";
    candidates[2].text = L"拟";
    observer.Promote(
        shuru::PinnedCandidateSchema::Quanpin, "Ni", &candidates);
    CHECK(candidates.front().text == L"你" && candidates.front().pinned);
    CHECK(!candidates[1].pinned && !candidates[2].pinned);

    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni", L"妮") ==
        shuru::PinnedCandidateToggleResult::Pinned);
    CHECK(observer.Lookup(
        shuru::PinnedCandidateSchema::Quanpin, "ni") == L"妮");
    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni", L"妮") ==
        shuru::PinnedCandidateToggleResult::Unpinned);
    CHECK(!observer.Lookup(shuru::PinnedCandidateSchema::Quanpin, "ni"));

    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni", L"你") ==
        shuru::PinnedCandidateToggleResult::Pinned);
    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::ShuangpinXiaohe, "ni", L"泥") ==
        shuru::PinnedCandidateToggleResult::Pinned);
    CHECK(observer.Lookup(
        shuru::PinnedCandidateSchema::Quanpin, "ni") == L"你");
    CHECK(observer.Lookup(
        shuru::PinnedCandidateSchema::ShuangpinXiaohe, "ni") == L"泥");

    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "", L"无") ==
        shuru::PinnedCandidateToggleResult::Failed);
    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni1", L"无") ==
        shuru::PinnedCandidateToggleResult::Failed);
    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni", L"含\t制表符") ==
        shuru::PinnedCandidateToggleResult::Failed);
    CHECK(first.Toggle(
        shuru::PinnedCandidateSchema::Quanpin, "ni",
        std::wstring(1, static_cast<wchar_t>(0xD800))) ==
        shuru::PinnedCandidateToggleResult::Failed);

    fs::remove_all(root, error);
    std::cout << "pinned_candidate_store: OK\n";
    return 0;
}
