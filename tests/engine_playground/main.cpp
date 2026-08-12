#include "engine/pinyin_engine.h"
#include "common/com_utils.h"
#include "common/logger.h"

#include <Windows.h>

#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    using namespace shuru;

    std::wstring lexicon = L"data\\lexicon";
    if (argc >= 2) {
        lexicon = argv[1];
    }

    PinyinEngine engine;
    if (!engine.Initialize(lexicon)) {
        std::cerr << u8"初始化引擎失败，请检查词库目录: "
                  << WideToUtf8(lexicon) << '\n';
        return 1;
    }

    std::cout << u8"发财拼音引擎演练。输入拼音后回车，输入 quit 退出。\n";
    while (true) {
        std::cout << "> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line == "quit" || line == "exit") {
            break;
        }

        const auto result = engine.Query(line, 9);
        if (result.candidates.empty()) {
            std::cout << u8"(无候选)\n";
            continue;
        }
        for (size_t i = 0; i < result.candidates.size(); ++i) {
            std::cout << (i + 1) << ". " << WideToUtf8(result.candidates[i].text)
                      << "  [" << result.candidates[i].pinyin << "]"
                      << " f=" << result.candidates[i].frequency << '\n';
        }
    }
    return 0;
}
