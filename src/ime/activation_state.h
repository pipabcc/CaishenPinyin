#pragma once

#include <cstdint>
#include <vector>

namespace shuru {

enum class ActivationResource : std::uint8_t {
    ThreadManager, Engine, ThreadSink, KeySink, TextSink, Ui
};

class ActivationState {
public:
    bool Add(ActivationResource resource) {
        const auto bit = Bit(resource);
        if ((bits_ & bit) != 0) return false;
        bits_ |= bit;
        order_.push_back(resource);
        return true;
    }
    void Remove(ActivationResource resource) { bits_ &= ~Bit(resource); }
    bool Has(ActivationResource resource) const { return (bits_ & Bit(resource)) != 0; }
    bool Empty() const { return bits_ == 0; }
    std::vector<ActivationResource> RollbackOrder() const {
        return std::vector<ActivationResource>(order_.rbegin(), order_.rend());
    }
    void Clear() { bits_ = 0; order_.clear(); }

private:
    static std::uint32_t Bit(ActivationResource resource) {
        return 1u << static_cast<unsigned>(resource);
    }
    std::uint32_t bits_ = 0;
    std::vector<ActivationResource> order_;
};

}  // namespace shuru
