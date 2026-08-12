#include "english_dict.h"

#include "../common/logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>

namespace shuru {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::string EnglishDictionary::Normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (ch >= 'a' && ch <= 'z') {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

bool EnglishDictionary::LoadFromFile(const std::wstring& path) {
    words_.clear();
    freq_.clear();
    std::ifstream in{std::filesystem::path(path)};
    if (!in) {
        SHURU_LOG_WARN("EnglishDictionary open failed");
        return false;
    }
    std::string line;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::string word;
        int frequency = 1000;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            word = Normalize(line);
        } else {
            word = Normalize(line.substr(0, tab));
            try {
                frequency = std::stoi(Trim(line.substr(tab + 1)));
            } catch (...) {
                frequency = 1000;
            }
        }
        if (word.size() < 2) {
            continue;
        }
        freq_[word] = (std::max)(freq_[word], frequency);
        ++loaded;
    }
    words_.reserve(freq_.size());
    for (const auto& kv : freq_) {
        words_.push_back(Item{kv.first, kv.second});
    }
    std::sort(words_.begin(), words_.end(), [](const Item& a, const Item& b) {
        return a.word < b.word;
    });
    SHURU_LOG_INFO("EnglishDictionary loaded entries=%zu unique=%zu", loaded, words_.size());
    return !words_.empty();
}

std::vector<Candidate> EnglishDictionary::LookupExact(const std::string& word) const {
    const std::string key = Normalize(word);
    std::vector<Candidate> out;
    const auto it = freq_.find(key);
    if (it == freq_.end()) {
        return out;
    }
    Candidate c;
    c.text = std::wstring(key.begin(), key.end());
    c.pinyin = key;
    c.frequency = it->second;
    c.from_user = false;
    c.is_english = true;
    out.push_back(std::move(c));
    return out;
}

std::vector<Candidate> EnglishDictionary::LookupPrefix(const std::string& prefix, size_t limit) const {
    const std::string pre = Normalize(prefix);
    std::vector<Candidate> out;
    if (pre.size() < 2 || words_.empty()) {
        return out;
    }
    Item probe{pre, 0};
    auto it = std::lower_bound(words_.begin(), words_.end(), probe,
                               [](const Item& a, const Item& b) { return a.word < b.word; });
    std::vector<Candidate> tmp;
    for (; it != words_.end(); ++it) {
        if (it->word.size() < pre.size() || it->word.compare(0, pre.size(), pre) != 0) {
            break;
        }
        Candidate c;
        c.text = std::wstring(it->word.begin(), it->word.end());
        c.pinyin = it->word;
        c.frequency = it->frequency;
        c.is_english = true;
        tmp.push_back(std::move(c));
        if (tmp.size() >= limit * 4) {
            break;
        }
    }
    std::sort(tmp.begin(), tmp.end(), [](const Candidate& a, const Candidate& b) {
        // 更短更靠前（更接近已输入），再比词频
        const int da = static_cast<int>(a.text.size());
        const int db = static_cast<int>(b.text.size());
        if (da != db) {
            return da < db;
        }
        if (a.frequency != b.frequency) {
            return a.frequency > b.frequency;
        }
        return a.text < b.text;
    });
    if (tmp.size() > limit) {
        tmp.resize(limit);
    }
    return tmp;
}

}  // namespace shuru
