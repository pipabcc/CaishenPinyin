#pragma once

#include <cstdint>
#include <string>

namespace shuru {

// 定时刷新和选词入口共用就绪状态，防止引擎已就绪但仍提交过期候选。
class CandidateReadiness {
public:
    void Invalidate() {
        ++generation_;
        waiting_ = false;
        context_ = nullptr;
        input_.clear();
    }
    std::uint64_t BeginWait(const void* context, const std::string& input) {
        ++generation_;
        waiting_ = true;
        context_ = context;
        input_ = input;
        return generation_;
    }
    bool waiting() const noexcept { return waiting_; }
    template<class Refresh>
    bool Poll(std::uint64_t generation, const void* context, const std::string& input,
              bool ready, bool loading, Refresh refresh) {
        if (!waiting_ || generation != generation_ || context != context_ || input != input_)
            return false;
        if (!ready) return loading;
        waiting_ = false;
        refresh();
        return false;
    }
    template<class Refresh>
    bool BeforeSelection(bool ready, Refresh refresh) {
        if (!waiting_ || !ready) return false;
        waiting_ = false;
        refresh();
        return true;
    }
private:
    std::uint64_t generation_ = 0;
    bool waiting_ = false;
    const void* context_ = nullptr;
    std::string input_;
};

}  // namespace shuru
