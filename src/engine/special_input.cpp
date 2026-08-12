#include "special_input.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace shuru {
namespace {

constexpr std::size_t kMaxExpressionLength = 64;
constexpr int kMaxParserDepth = 16;
constexpr int kMaxOperations = 64;
constexpr double kMaxAbsoluteResult = 1.0e15;

bool IsAsciiLower(char ch) noexcept { return ch >= 'a' && ch <= 'z'; }
bool IsAsciiUpper(char ch) noexcept { return ch >= 'A' && ch <= 'Z'; }

class CalculatorParser {
public:
    explicit CalculatorParser(const std::string& expression) : expression_(expression) {}

    bool Parse(double* result) noexcept {
        if (result == nullptr || expression_.empty() ||
            expression_.size() > kMaxExpressionLength) {
            return false;
        }
        double value = 0.0;
        if (!ParseExpression(0, &value) || position_ != expression_.size() ||
            !IsSafe(value)) {
            return false;
        }
        *result = value;
        return true;
    }

private:
    bool ParseExpression(int depth, double* value) noexcept {
        if (!ParseTerm(depth, value)) return false;
        while (position_ < expression_.size() &&
               (expression_[position_] == '+' || expression_[position_] == '-')) {
            const char operation = expression_[position_++];
            double right = 0.0;
            if (++operations_ > kMaxOperations || !ParseTerm(depth, &right)) return false;
            *value = operation == '+' ? *value + right : *value - right;
            if (!IsSafe(*value)) return false;
        }
        return true;
    }

    bool ParseTerm(int depth, double* value) noexcept {
        if (!ParseUnary(depth, value)) return false;
        while (position_ < expression_.size() &&
               (expression_[position_] == '*' || expression_[position_] == '/')) {
            const char operation = expression_[position_++];
            double right = 0.0;
            if (++operations_ > kMaxOperations || !ParseUnary(depth, &right)) return false;
            if (operation == '/' && right == 0.0) return false;
            *value = operation == '*' ? *value * right : *value / right;
            if (!IsSafe(*value)) return false;
        }
        return true;
    }

    bool ParseUnary(int depth, double* value) noexcept {
        if (position_ < expression_.size() &&
            (expression_[position_] == '+' || expression_[position_] == '-')) {
            const bool negative = expression_[position_++] == '-';
            if (++operations_ > kMaxOperations || !ParseUnary(depth, value)) return false;
            if (negative) *value = -*value;
            return IsSafe(*value);
        }
        return ParsePrimary(depth, value);
    }

    bool ParsePrimary(int depth, double* value) noexcept {
        if (position_ >= expression_.size()) return false;
        if (expression_[position_] == '(') {
            if (depth >= kMaxParserDepth) return false;
            ++position_;
            if (!ParseExpression(depth + 1, value) || position_ >= expression_.size() ||
                expression_[position_] != ')') {
                return false;
            }
            ++position_;
            return true;
        }
        return ParseNumber(value);
    }

    bool ParseNumber(double* value) noexcept {
        const std::size_t begin = position_;
        bool has_digit = false;
        bool has_decimal = false;
        double integer = 0.0;
        double fraction = 0.0;
        double scale = 0.1;
        while (position_ < expression_.size()) {
            const char ch = expression_[position_];
            if (ch >= '0' && ch <= '9') {
                has_digit = true;
                const int digit = ch - '0';
                if (has_decimal) {
                    fraction += static_cast<double>(digit) * scale;
                    scale *= 0.1;
                } else {
                    integer = integer * 10.0 + static_cast<double>(digit);
                }
                ++position_;
                if (!IsSafe(integer + fraction)) return false;
                continue;
            }
            if (ch == '.' && !has_decimal) {
                has_decimal = true;
                ++position_;
                continue;
            }
            break;
        }
        if (!has_digit || position_ == begin) return false;
        *value = integer + fraction;
        return true;
    }

    static bool IsSafe(double value) noexcept {
        return std::isfinite(value) && std::abs(value) <= kMaxAbsoluteResult;
    }

    const std::string& expression_;
    std::size_t position_ = 0;
    int operations_ = 0;
};

std::wstring FormatCalculatorResult(double value) {
    if (std::abs(value) < 0.5e-12) value = 0.0;
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(12) << std::fixed << value;
    std::wstring output = stream.str();
    const std::size_t decimal = output.find(L'.');
    if (decimal != std::wstring::npos) {
        while (!output.empty() && output.back() == L'0') output.pop_back();
        if (!output.empty() && output.back() == L'.') output.pop_back();
    }
    return output == L"-0" ? L"0" : output;
}

Candidate MakeNonLearnableCandidate(
    const std::string& raw_input,
    std::wstring text,
    CandidateSource source) {
    Candidate candidate;
    candidate.text = std::move(text);
    candidate.pinyin = raw_input;
    candidate.covered_input_len = raw_input.size();
    candidate.source = source;
    candidate.learnable = false;
    candidate.ranking_score = 1.0e9;
    return candidate;
}

}  // namespace

bool ParseMixedInput(
    const std::string& raw_input,
    std::vector<MixedInputSegment>* segments) {
    if (segments == nullptr) return false;
    segments->clear();
    bool has_literal = false;
    for (char ch : raw_input) {
        const bool literal = IsAsciiUpper(ch);
        if (!literal && !IsAsciiLower(ch) && ch != '\'') {
            segments->clear();
            return false;
        }
        has_literal = has_literal || literal;
        if (segments->empty() || segments->back().literal != literal) {
            segments->push_back({std::string(1, ch), literal});
        } else {
            segments->back().text.push_back(ch);
        }
    }
    if (!has_literal) segments->clear();
    return has_literal;
}

bool IsCalculatorInput(const std::string& raw_input) noexcept {
    return !raw_input.empty() && raw_input.front() == 'v';
}

bool TryEvaluateCalculator(
    const std::string& raw_input,
    std::wstring* formatted_result) noexcept {
    if (formatted_result == nullptr || !IsCalculatorInput(raw_input) ||
        raw_input.size() <= 1) {
        return false;
    }
    try {
        double value = 0.0;
        CalculatorParser parser(raw_input.substr(1));
        if (!parser.Parse(&value)) return false;
        *formatted_result = FormatCalculatorResult(value);
        return !formatted_result->empty();
    } catch (...) {
        return false;
    }
}

std::vector<Candidate> BuildCalculatorCandidates(const std::string& raw_input) {
    std::wstring result;
    if (!TryEvaluateCalculator(raw_input, &result)) return {};
    return {MakeNonLearnableCandidate(raw_input, std::move(result), CandidateSource::Dynamic)};
}

std::vector<Candidate> BuildTimeCandidates(
    const std::string& raw_input,
    const SYSTEMTIME& local_time) {
    if (raw_input != "sj") return {};
    wchar_t combined[64] {};
    wchar_t date[32] {};
    wchar_t time[32] {};
    swprintf_s(
        combined, L"%04u-%02u-%02u %02u:%02u:%02u",
        local_time.wYear, local_time.wMonth, local_time.wDay,
        local_time.wHour, local_time.wMinute, local_time.wSecond);
    swprintf_s(date, L"%u年%u月%u日", local_time.wYear, local_time.wMonth, local_time.wDay);
    swprintf_s(time, L"%02u:%02u:%02u", local_time.wHour, local_time.wMinute, local_time.wSecond);
    static constexpr const wchar_t* kWeekdays[] = {
        L"星期日", L"星期一", L"星期二", L"星期三",
        L"星期四", L"星期五", L"星期六",
    };
    const wchar_t* weekday = local_time.wDayOfWeek < 7
        ? kWeekdays[local_time.wDayOfWeek]
        : L"星期";
    return {
        MakeNonLearnableCandidate(raw_input, combined, CandidateSource::Dynamic),
        MakeNonLearnableCandidate(raw_input, date, CandidateSource::Dynamic),
        MakeNonLearnableCandidate(raw_input, time, CandidateSource::Dynamic),
        MakeNonLearnableCandidate(raw_input, weekday, CandidateSource::Dynamic),
    };
}

std::vector<Candidate> BuildCurrentTimeCandidates(const std::string& raw_input) {
    SYSTEMTIME local_time {};
    GetLocalTime(&local_time);
    return BuildTimeCandidates(raw_input, local_time);
}

}  // namespace shuru
