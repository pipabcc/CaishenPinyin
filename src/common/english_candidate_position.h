#pragma once

#include <cstdint>

namespace shuru {

enum class EnglishCandidatePosition : std::uint8_t {
    First = 0,
    Middle = 1,
    Last = 2,
};

}  // namespace shuru
