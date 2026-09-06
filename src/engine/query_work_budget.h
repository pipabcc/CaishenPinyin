#pragma once

#include <array>
#include <chrono>
#include <cstddef>

namespace shuru {

enum class QueryStage : std::size_t {
    Indexed, Correction, WordGraph, MixedGraph, Fuzzy, Finalize, Count
};

struct QueryDiagnostics {
    std::array<std::size_t, static_cast<std::size_t>(QueryStage::Count)> work{};
    std::array<double, static_cast<std::size_t>(QueryStage::Count)> milliseconds{};
    std::size_t work_used = 0;
    bool budget_exhausted = false;
};

// 预算按确定的访问/展开次数计量，避免同一输入因机器快慢而得到不同排序。
// 嵌套混输子查询共享同一实例，不能通过重新进入 Query 放大总工作量。
class QueryWorkBudget {
public:
    explicit QueryWorkBudget(std::size_t maximum, QueryDiagnostics* diagnostics = nullptr)
        : remaining_(maximum), diagnostics_(diagnostics) {
        if (diagnostics_) *diagnostics_ = {};
        if (diagnostics_) started_ = Clock::now();
    }

    ~QueryWorkBudget() { RecordElapsed(); }

    bool Consume(std::size_t units = 1) {
        if (units > remaining_) {
            if (diagnostics_) diagnostics_->budget_exhausted = true;
            remaining_ = 0;
            return false;
        }
        remaining_ -= units;
        if (diagnostics_) {
            diagnostics_->work[static_cast<std::size_t>(stage_)] += units;
            diagnostics_->work_used += units;
        }
        return true;
    }

    bool empty() const noexcept { return remaining_ == 0; }
    QueryStage stage() const noexcept { return stage_; }
    void SetStage(QueryStage stage) {
        RecordElapsed();
        stage_ = stage;
    }

private:
    using Clock = std::chrono::steady_clock;
    void RecordElapsed() {
        if (!diagnostics_) return;
        const auto now = Clock::now();
        diagnostics_->milliseconds[static_cast<std::size_t>(stage_)] +=
            std::chrono::duration<double, std::milli>(now - started_).count();
        diagnostics_->budget_exhausted = diagnostics_->budget_exhausted || empty();
        started_ = now;
    }

    std::size_t remaining_;
    QueryDiagnostics* diagnostics_;
    QueryStage stage_ = QueryStage::Indexed;
    Clock::time_point started_{};
};

class QueryStageScope {
public:
    QueryStageScope(QueryWorkBudget& budget, QueryStage stage)
        : budget_(budget), previous_(budget.stage()) { budget_.SetStage(stage); }
    ~QueryStageScope() { budget_.SetStage(previous_); }
private:
    QueryWorkBudget& budget_;
    QueryStage previous_;
};

}  // namespace shuru
