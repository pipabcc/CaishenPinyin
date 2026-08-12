#pragma once

#include <string>

namespace shuru {

// 小鹤双拼解码（实用子集，覆盖常用音节）
inline std::string MapXiaoheInitial(char key) {
    switch (key) {
    case 'u': return "sh";
    case 'i': return "ch";
    case 'v': return "zh";
    case 'a': case 'o': case 'e': case 'b': case 'p': case 'm': case 'f':
    case 'd': case 't': case 'n': case 'l': case 'g': case 'k': case 'h':
    case 'j': case 'q': case 'x': case 'r': case 'z': case 'c': case 's':
    case 'y': case 'w':
        return std::string(1, key);
    default:
        return std::string(1, key);
    }
}

inline bool IsZeroInitialKey(char key) {
    return key == 'a' || key == 'o' || key == 'e';
}

inline std::string MapXiaoheFinal(char key, const std::string& initial) {
    switch (key) {
    case 'q': return "iu";
    case 'w': return "ei";
    case 'e': return "e";
    case 'r': return "uan";
    case 't': return "ve";
    case 'y': return "un";
    case 'u': return "u";
    case 'i': return "i";
    case 'o': return "o";
    case 'p': return "un";
    case 'a': return "a";
    case 's':
        return (initial == "j" || initial == "q" || initial == "x" || initial == "y") ? "iong" : "ong";
    case 'd': return "ai";
    case 'f': return "en";
    case 'g': return "eng";
    case 'h': return "ang";
    case 'j': return "an";
    case 'k':
        if (initial == "g" || initial == "k" || initial == "h") {
            return "uai";
        }
        return "ing";
    case 'l':
        if (initial == "j" || initial == "q" || initial == "x" || initial == "y") {
            return "iang";
        }
        if (initial == "d" || initial == "t" || initial == "n" || initial == "l" ||
            initial == "g" || initial == "k" || initial == "h" || initial == "zh" ||
            initial == "ch" || initial == "sh" || initial == "r" || initial == "z" ||
            initial == "c" || initial == "s" || initial.empty()) {
            return "uang";
        }
        return "iang";
    case 'z': return "ou";
    case 'x': return "ie";
    case 'c':
        // ao / iao
        if (initial == "d" || initial == "t" || initial == "n" || initial == "l" ||
            initial == "j" || initial == "q" || initial == "x") {
            return "iao";
        }
        return "ao";
    case 'v':
        return (initial == "j" || initial == "q" || initial == "x" || initial == "y" || initial == "n" || initial == "l")
                   ? "ue"
                   : "ui";
    case 'b': return "in";
    case 'n': return "iao";
    case 'm': return "ian";
    default: return std::string(1, key);
    }
}

inline std::string DecodeXiaoheZero(char k1, char k2) {
    // 小鹤零声母常用表
    struct Pair { char a; char b; const char* py; };
    static const Pair kTable[] = {
        {'a', 'a', "a"},   {'a', 'd', "ai"},  {'a', 'j', "an"}, {'a', 'h', "ang"}, {'a', 'c', "ao"},
        {'o', 'o', "o"},   {'o', 'z', "ou"},
        {'e', 'e', "e"},   {'e', 'w', "ei"},  {'e', 'f', "en"}, {'e', 'g', "eng"}, {'e', 'r', "er"},
    };
    for (const auto& p : kTable) {
        if (p.a == k1 && p.b == k2) {
            return p.py;
        }
    }
    return std::string(1, k1) + MapXiaoheFinal(k2, "");
}

inline std::string DecodeXiaoheSyllable(char k1, char k2) {
    if (IsZeroInitialKey(k1)) {
        return DecodeXiaoheZero(k1, k2);
    }
    const std::string initial = MapXiaoheInitial(k1);
    const std::string fin = MapXiaoheFinal(k2, initial);

    if (initial == "y") {
        if (fin == "i") return "yi";
        if (fin == "in") return "yin";
        if (fin == "ing") return "ying";
        if (fin == "u") return "yu";
        if (fin == "ue") return "yue";
        if (fin == "un") return "yun";
        if (fin == "uan") return "yuan";
        if (fin == "ve") return "yue";
    }
    if (initial == "w") {
        if (fin == "u") return "wu";
        if (fin == "o") return "wo";
        if (fin == "ei") return "wei";
        if (fin == "en") return "wen";
        if (fin == "eng") return "weng";
        if (fin == "an") return "wan";
        if (fin == "ang") return "wang";
        if (fin == "ai") return "wai";
    }
    // ju/qu/xu
    if ((initial == "j" || initial == "q" || initial == "x") && fin == "u") {
        return initial + "u";
    }
    if ((initial == "j" || initial == "q" || initial == "x") && fin == "ue") {
        return initial + "ue";
    }
    if ((initial == "j" || initial == "q" || initial == "x") && fin == "un") {
        return initial + "un";
    }
    if ((initial == "j" || initial == "q" || initial == "x") && fin == "uan") {
        return initial + "uan";
    }
    return initial + fin;
}

// code: 双拼码；返回用于检索的全拼；preview 可选返回预览串（全拼+未完成码）
inline std::string DecodeXiaoheShuangpin(const std::string& code, std::string* preview = nullptr) {
    std::string full;
    size_t i = 0;
    while (i + 1 < code.size()) {
        full += DecodeXiaoheSyllable(code[i], code[i + 1]);
        i += 2;
    }
    std::string rest;
    if (i < code.size()) {
        const char k = code[i];
        if (IsZeroInitialKey(k)) {
            rest.push_back(k);
        } else {
            rest = MapXiaoheInitial(k);
        }
    }
    if (preview) {
        *preview = full;
        if (!rest.empty()) {
            if (!preview->empty()) {
                *preview += "'";
            }
            *preview += rest;
        }
        if (preview->empty()) {
            *preview = code;
        }
    }
    return full + rest;
}

}  // namespace shuru
