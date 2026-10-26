#pragma once

#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>

namespace vxe {

// A perspective projection with reversed Z and an infinite far plane (ADR-010).
//
// Normal projections map the near plane to depth 0 and the far plane to 1.
// Floating point numbers are densest near 0, and the projection already
// spends most of its precision near the camera, so far terrain z-fights.
// Reversing the mapping (near = 1, infinitely far = 0) puts the dense floats
// where the projection is sparse, which gives almost uniform precision out to
// any distance. With a 32 bit float depth buffer this holds terrain tens of
// thousands of blocks away without a far plane at all.
//
// Right handed view space (the camera looks down -Z), clip depth 0 to 1
// (GLM_FORCE_DEPTH_ZERO_TO_ONE). Use with a GREATER depth test and clear depth
// to 0. `fovY` is the full vertical field of view in radians.
[[nodiscard]] Mat4 perspectiveReversedInfinite(float fovY, float aspect, float zNear) noexcept;

// The view matrix of a camera at the origin looking along `forward`.
// Rendering is camera relative (ADR-011): the camera is always at the
// origin and the world moves around it, so the view is a pure rotation.
[[nodiscard]] Mat4 cameraRelativeView(const Vec3& forward, const Vec3& up = Vec3(0.0F, 1.0F, 0.0F)) noexcept;

// The camera position split into the chunk it is in and its offset inside
// that chunk (0 to 16 on every axis). Everything the GPU sees is relative to
// this, so the floats sent to shaders stay small however far the camera is
// from the world origin (ADR-011).
struct CameraPosition {
    ChunkPos chunk;
    Vec3 fraction{0.0F}; // Position inside `chunk`, in blocks, 0 <= fraction < 16.

    // Splits a double precision world position exactly: the chunk comes from
    // the integer part, and only the small remainder becomes a float.
    [[nodiscard]] static CameraPosition fromWorld(const DVec3& position) noexcept;
};

// Where a chunk's minimum corner is relative to the camera, in blocks. The
// chunk difference is computed with integers first, so the result is exact as
// long as it is within float range of the camera (always true for anything
// rendered).
[[nodiscard]] Vec3 relativeToCamera(const ChunkPos& chunk, const CameraPosition& camera) noexcept;

// Where a block's minimum corner is relative to the camera, in blocks.
[[nodiscard]] Vec3 relativeToCamera(const BlockPos& block, const CameraPosition& camera) noexcept;

} // namespace vxe
