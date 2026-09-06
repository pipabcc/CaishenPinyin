#include "engine/shared_engine.h"
#include "engine/user_dictionary_store.h"
#include "engine/user_bigram.h"
#include "common/com_utils.h"
#include "ime/candidate_readiness.h"

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    Require(static_cast<bool>(file), "测试文件写入失败");
}

int WordCount(const fs::path& path, const std::wstring& word) {
    shuru::UserDictionaryState state;
    if (!shuru::LoadUserDictionaryState(path.wstring(), &state)) return -1;
    for (const auto& entry : state.dictionary.SnapshotUserEntries())
        if (entry.word == word) return entry.selection_count;
    return 0;
}

template<class Predicate> void WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, "等待学习保存超时");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

HANDLE StartChild(const std::wstring& executable, const std::wstring& arguments) {
    std::wstring command = L"\"" + executable + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "子测试启动失败");
    CloseHandle(process.hThread);
    return process.hProcess;
}

void FinishChild(HANDLE process) {
    const auto wait = WaitForSingleObject(process, 60000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process, 99);
    DWORD exit_code = 99;
    GetExitCodeProcess(process, &exit_code);
    CloseHandle(process);
    Require(wait == WAIT_OBJECT_0 && exit_code == 0, "子测试执行失败");
}

void CheckMetadata(const fs::path& root) {
    shuru::Dictionary dictionary;
    Require(dictionary.LoadFromUtf8Lines({u8"ni\t你\t100\t7\t1700000000",
        u8"ni\t拟\t90\t2\t1700000010"}, true), "元数据样例装载失败");
    dictionary.IncreaseUserWord("ni", L"拟", 20, 0, 1700001000);
    dictionary.AddWord("ni", L"你", 200, true);
    const auto path = root / L"metadata.txt";
    Require(dictionary.SaveUserToFile(path.wstring()), "元数据写入失败");
    shuru::Dictionary reloaded;
    Require(reloaded.LoadFromFile(path.wstring(), true), "元数据重载失败");
    for (const auto& entry : reloaded.SnapshotUserEntries()) {
        if (entry.word == L"拟") Require(entry.selection_count == 3 && entry.last_used_unix == 1700001000,
                                        "重排后保存了别人的学习元数据");
        if (entry.word == L"你") Require(entry.selection_count == 7 && entry.last_used_unix == 1700000000,
                                        "AddWord 重排破坏了学习元数据");
    }
}

void CheckBigram(const fs::path& root) {
    const auto path = root / L"bigram" / L"user_bigram.txt";
    shuru::UserBigramModel first, second, loaded;
    first.Observe(L"今天", L"开心", 100);
    second.Observe(L"明天", L"发财", 101);
    Require(first.SaveToFile(path.wstring()) && second.SaveToFile(path.wstring()), "搭配保存失败");
    Require(first.SaveToFile(path.wstring()), "重复保存失败");
    Require(loaded.LoadFromFile(path.wstring()) && loaded.Count(L"今天", L"开心") == 1 &&
        loaded.Count(L"明天", L"发财") == 1, "搭配覆盖或重复累加");
    first.Observe(L"今天", L"开心", 102);
    HANDLE blocker = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(blocker != INVALID_HANDLE_VALUE, "失败注入句柄创建失败");
    const bool unexpectedly_saved = first.SaveToFile(path.wstring());
    CloseHandle(blocker);
    Require(!unexpectedly_saved && first.pending_size() == 1, "保存失败丢失了待写增量");
    Require(first.SaveToFile(path.wstring()) && first.SaveToFile(path.wstring()), "保存重试失败");
    Require(loaded.LoadFromFile(path.wstring()) && loaded.Count(L"今天", L"开心") == 2,
        "保存重试重复计数");

    shuru::UserBigramModel bounded;
    for (size_t index = 0; index < 200000; ++index)
        bounded.Observe(L"词" + std::to_wstring(index), L"后继", 100);
    Require(bounded.size() == 4096 && bounded.pending_size() == 4096, "搭配容量上限失效");
    Require(bounded.Observe(L"词0", L"新增搭配", 101), "已满模型不能更新已有前词");
    for (int index = 0; index < 40; ++index)
        bounded.Observe(L"词0", L"后继" + std::to_wstring(index), 102 + index);
    Require(bounded.Successors(L"词0", 100).size() == 16 && bounded.pending_size() <= 4096 * 16,
        "后继或待写集合没有限量");
    Require(bounded.SaveToFile((root / L"bounded.txt").wstring()), "限量模型保存失败");
    Write(root / L"oversized.txt", "");
    {
        std::ofstream file(root / L"oversized.txt", std::ios::binary);
        for (int index = 0; index < 5000; ++index)
            file << "p" << index << "\tnext\t1\t100\n";
        file << "p0\tnew\t2\t101\n";
    }
    Require(loaded.LoadFromFile((root / L"oversized.txt").wstring()) && loaded.size() == 4096 &&
        loaded.Count(L"p0", L"new") == 2, "装载容量边界错误");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && (std::wstring(argv[1]) == L"--learn" || std::wstring(argv[1]) == L"--bigram")) {
        shuru::PinyinEngine child;
        if (!child.Initialize(argv[2])) return 2;
        for (int index = 0; index < 3; ++index) {
            if (std::wstring(argv[1]) == L"--learn") child.Learn("nihao", L"拟好");
            else child.ObserveBigram(L"共享", L"搭配");
        }
        return 0;
    }
    const auto root = fs::temp_directory_path() /
        (L"caishen-learning-" + std::to_wstring(GetCurrentProcessId()));
    bool acquired = false;
    try {
        fs::create_directories(root);
        const auto lexicon = root / L"system";
        Write(lexicon / L"base_dict.txt", u8"nihao\t你好\t100\nshijie\t世界\t100\n");
        const auto local = root / L"local";
        fs::create_directories(local);
        SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
        CheckMetadata(root);
        CheckBigram(root);
        auto loading_gate = std::make_unique<shuru::UserLearningFileLock>();
        Require(loading_gate->owns(), "加载故障注入准备失败");
        auto* engine = shuru::SharedEngine::Acquire(lexicon.wstring());
        acquired = true;
        Require(engine && !shuru::SharedEngine::WaitForReady(300), "加载未被延迟超过等待上限");
        shuru::CandidateReadiness readiness;
        int context = 1;
        const auto token = readiness.BeginWait(&context, "ni");
        shuru::EngineQueryResult first_candidates;
        auto refresh = [&] { first_candidates = engine->Query("ni", 10); };
        Require(readiness.Poll(token, &context, "ni", false, true, refresh) &&
            first_candidates.candidates.empty(), "加载中产生了假候选");
        loading_gate.reset();
        Require(engine && shuru::SharedEngine::WaitForReady(5000), "共享引擎初始化失败");
        Require(readiness.BeforeSelection(engine->IsReady(), refresh) &&
            !first_candidates.candidates.empty() && first_candidates.candidates.front().text == L"你",
            "就绪瞬间选词没有补查中文候选");
        const fs::path user_path(engine->user_dict_path());
        engine->Learn("nihao", L"拟好");
        WaitUntil([&] { return WordCount(user_path, L"拟好") == 1; });
        Require(engine->UndoLastLearning(), "落盘后的撤销失败");
        WaitUntil([&] { return WordCount(user_path, L"拟好") == 0; });

        {
            shuru::UserLearningFileLock held;
            Require(held.owns(), "失败注入互斥量获取失败");
            engine->Learn("nihao", L"拟好");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            shuru::UserDictionaryState cleared;
            Require(shuru::ReplaceUserDictionary(user_path.wstring(), {}, false, &cleared), "清空失败");
        }
        engine->Learn("shijie", L"世杰");
        WaitUntil([&] { return WordCount(user_path, L"世杰") == 1; });
        Require(WordCount(user_path, L"拟好") == 0, "在途保存恢复了已清空的旧词");
        Require(engine->ClearUserDictionary(), "引擎清空接口失败");
        Require(WordCount(user_path, L"世杰") == 0, "引擎清空接口没有真正删除");

        engine->Learn("nihao", L"拟好");
        WaitUntil([&] { return WordCount(user_path, L"拟好") == 1; });
        if (argc >= 3) {
            FinishChild(StartChild(argv[1], L"run --project \"" + std::wstring(argv[2]) +
                L"\" --configuration Release -- --user-dictionary-clear \"" + user_path.wstring() + L"\""));
        } else {
            shuru::UserDictionaryState cleared;
            Require(shuru::ReplaceUserDictionary(user_path.wstring(), {}, false, &cleared), "外部清空失败");
        }
        shuru::SharedEngine::Release();
        acquired = false;
        auto* again = shuru::SharedEngine::Acquire(lexicon.wstring());
        acquired = true;
        Require(again == engine, "用户数据刷新重建了基础引擎");
        engine->Learn("shijie", L"世杰");
        WaitUntil([&] { return WordCount(user_path, L"世杰") == 1; });
        Require(WordCount(user_path, L"拟好") == 0, "设置程序清空后旧词恢复");
        Require(engine->ClearUserDictionary(), "并发测试准备失败");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        const auto arguments = L"--learn \"" + lexicon.wstring() + L"\"";
        HANDLE first = StartChild(executable, arguments);
        HANDLE second = StartChild(executable, arguments);
        FinishChild(first);
        FinishChild(second);
        Require(WordCount(user_path, L"拟好") == 6, "两个宿主的同词增量没有相加");
        const auto bigram_arguments = L"--bigram \"" + lexicon.wstring() + L"\"";
        first = StartChild(executable, bigram_arguments);
        second = StartChild(executable, bigram_arguments);
        FinishChild(first);
        FinishChild(second);
        shuru::UserBigramModel shared_pairs;
        const auto bigram_path = user_path.parent_path() / L"user_bigram.txt";
        Require(shared_pairs.LoadFromFile(bigram_path.wstring()) &&
            shared_pairs.Count(L"共享", L"搭配") == 6, "多个进程的搭配增量没有相加");
        const auto imported = root / L"import.txt";
        Write(imported, u8"xinci\t新词\t20\t1\t100\n");
        Require(engine->ImportUserDictionary(imported.wstring()) &&
            WordCount(user_path, L"新词") == 1 && WordCount(user_path, L"拟好") == 6,
            "导入没有保留已有用户词");
        Require(!engine->PredictNext(L"共享", 9).empty(), "导入错误清空了搭配学习");
        shuru::SharedEngine::Release();
        acquired = false;
        shuru::SharedEngine::Shutdown();
        fs::remove_all(root);
        std::cout << "learning_persistence: OK\n";
        return 0;
    } catch (const std::exception& error) {
        if (acquired) shuru::SharedEngine::Release();
        shuru::SharedEngine::Shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
