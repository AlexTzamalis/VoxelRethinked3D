#pragma once

// glm configuration and the engine's vector aliases.
//
// GLM_FORCE_DEPTH_ZERO_TO_ONE and GLM_FORCE_INTRINSICS are set as public
// compile definitions of vxe_math (engine/CMakeLists.txt), never here: every
// translation unit must see the same glm configuration, or two files could
// disagree on the layout of glm types.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vxe {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using DVec3 = glm::dvec3; // Entity positions (Section 9.1): double keeps precision far from the origin.
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using UVec3 = glm::uvec3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

} // namespace vxe
