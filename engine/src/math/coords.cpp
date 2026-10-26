#include "vxe/math/coords.hpp"

#include <cmath>
#include <limits>

namespace vxe {
namespace {

// Rounds down and converts to int64 without undefined behavior. Converting a
// double outside the int64 range (or NaN) to an integer is undefined, so the
// value is clamped first. The bounds are exactly representable as doubles.
i64 floorToI64(f64 value) noexcept {
    constexpr auto kMin = static_cast<f64>(std::numeric_limits<i64>::min());
    constexpr f64 kMaxExclusive = 9223372036854775808.0; // 2^63, the first double above int64 max.
    if (std::isnan(value)) {
        return 0;
    }
    const f64 floored = std::floor(value);
    if (floored < kMin) {
        return std::numeric_limits<i64>::min();
    }
    if (floored >= kMaxExclusive) {
        return std::numeric_limits<i64>::max();
    }
    return static_cast<i64>(floored);
}

} // namespace

BlockPos blockAt(const DVec3& position) noexcept {
    return {.x = floorToI64(position.x), .y = floorToI64(position.y), .z = floorToI64(position.z)};
}

} // namespace vxe
