#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <string_view>

namespace vxe {

// The six faces of a block, in the order used everywhere in the engine: block
// render info (faceTextureLayer), the 3 bit face field of packed quads and the
// face groups of chunk meshes (Sections 6.3, 8.2 and 10.4).
enum class Face : u8 {
    PosX,
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ,
};

inline constexpr usize kFaceCount = 6;
inline constexpr std::array<Face, kFaceCount> kAllFaces{Face::PosX, Face::NegX, Face::PosY,
                                                        Face::NegY, Face::PosZ, Face::NegZ};

// The face on the other side: +X and -X, +Y and -Y, +Z and -Z. The order above
// pairs them, so flipping the lowest bit is enough.
[[nodiscard]] constexpr Face opposite(Face face) noexcept {
    return static_cast<Face>(static_cast<u8>(face) ^ 1U);
}

// The axis a face points along: 0 for X, 1 for Y, 2 for Z.
[[nodiscard]] constexpr int axisOf(Face face) noexcept {
    return static_cast<int>(static_cast<u8>(face) >> 1U);
}

// +1 for the positive faces, -1 for the negative ones.
[[nodiscard]] constexpr int signOf(Face face) noexcept {
    return (static_cast<u8>(face) & 1U) == 0 ? 1 : -1;
}

// The unit vector a face points to, for example (0, 1, 0) for PosY.
[[nodiscard]] inline IVec3 normalOf(Face face) noexcept {
    IVec3 normal(0);
    normal[axisOf(face)] = signOf(face);
    return normal;
}

// The face of a block that points along `axis` with the given sign.
[[nodiscard]] constexpr Face faceFromAxis(int axis, int sign) noexcept {
    VXE_ASSERT(axis >= 0 && axis < 3 && (sign == 1 || sign == -1), "Invalid axis or sign");
    return static_cast<Face>((axis << 1) | (sign < 0 ? 1 : 0));
}

// Returns the name of a face, for example "PosX". Not called toString because
// doctest uses that name through ADL to print values.
[[nodiscard]] constexpr std::string_view nameOf(Face face) noexcept {
    constexpr std::array<std::string_view, kFaceCount> kNames{"PosX", "NegX", "PosY", "NegY", "PosZ", "NegZ"};
    return kNames[static_cast<usize>(face)];
}

} // namespace vxe
