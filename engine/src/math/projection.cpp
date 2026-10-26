#include "vxe/math/projection.hpp"

#include <vxe/core/assert.hpp>

#include <glm/ext/matrix_transform.hpp>

#include <cmath>

namespace vxe {

Mat4 perspectiveReversedInfinite(float fovY, float aspect, float zNear) noexcept {
    VXE_ASSERT(fovY > 0.0F && aspect > 0.0F && zNear > 0.0F, "Invalid projection parameters");
    const float f = 1.0F / std::tan(fovY * 0.5F);
    // glm matrices are column major: m[column][row].
    Mat4 m(0.0F);
    m[0][0] = f / aspect;
    m[1][1] = f;
    // Clip z is the constant zNear and clip w is -z_view, so after the divide
    // depth = zNear / -z_view: exactly 1 at the near plane, falling towards 0
    // as the distance grows, and never reaching it.
    m[3][2] = zNear;
    m[2][3] = -1.0F;
    return m;
}

Mat4 cameraRelativeView(const Vec3& forward, const Vec3& up) noexcept {
    return glm::lookAt(Vec3(0.0F), forward, up);
}

CameraPosition CameraPosition::fromWorld(const DVec3& position) noexcept {
    const BlockPos block = blockAt(position);
    const ChunkPos chunk = toChunk(block);
    const BlockPos origin = chunkOrigin(chunk);
    // The subtraction happens in double while both values are large; the
    // difference is at most 16, so converting it to float loses nothing that
    // matters (float has about 7 digits, which is micrometers at this size).
    return CameraPosition{
        .chunk = chunk,
        .fraction = Vec3(static_cast<float>(position.x - static_cast<f64>(origin.x)),
                         static_cast<float>(position.y - static_cast<f64>(origin.y)),
                         static_cast<float>(position.z - static_cast<f64>(origin.z))),
    };
}

Vec3 relativeToCamera(const ChunkPos& chunk, const CameraPosition& camera) noexcept {
    // int64, so chunks at opposite ends of the grid cannot overflow.
    const i64 dx = (static_cast<i64>(chunk.x) - camera.chunk.x) * kChunkSize;
    const i64 dy = (static_cast<i64>(chunk.y) - camera.chunk.y) * kChunkSize;
    const i64 dz = (static_cast<i64>(chunk.z) - camera.chunk.z) * kChunkSize;
    return Vec3(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)) - camera.fraction;
}

Vec3 relativeToCamera(const BlockPos& block, const CameraPosition& camera) noexcept {
    const BlockPos origin = chunkOrigin(camera.chunk);
    return Vec3(static_cast<float>(block.x - origin.x), static_cast<float>(block.y - origin.y),
                static_cast<float>(block.z - origin.z)) -
           camera.fraction;
}

} // namespace vxe
