#pragma once

#include <cstddef>
#include <cstdint>

// Short names for the fixed width types used everywhere in the engine.
// Voxel code is full of exact sizes (8 byte quads, 16 bit state ids), and
// spelling std::uint32_t hundreds of times hides the logic. These aliases are
// exactly the standard types, so they mix freely with code that uses std::.
namespace vxe {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;
using isize = std::ptrdiff_t;

// The engine relies on these sizes for packed GPU data and save files.
static_assert(sizeof(f32) == 4, "vxe requires a 32 bit float");
static_assert(sizeof(f64) == 8, "vxe requires a 64 bit double");
static_assert(sizeof(void*) == 8, "vxe only supports 64 bit platforms");

} // namespace vxe
