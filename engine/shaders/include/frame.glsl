#pragma once
// Per frame data (set 0, slot 0), written by render::FrameConstantsBuffer.
// The layout must match render::FrameConstants in frame_constants.hpp.
#include "include/bindings.glsl"

VXE_BINDING_STD140(0, 0) uniform Frame {
    mat4 viewProjection; // Camera relative positions to clip space.
    mat4 view;
    mat4 projection;     // Reversed Z, infinite far plane.
    mat4 inverseViewProjection;
    ivec4 cameraChunk;   // xyz: the camera's chunk.
    vec4 cameraFraction; // xyz: the camera inside its chunk, in blocks.
    vec4 cameraForward;
    vec4 viewport;       // width, height, 1 / width, 1 / height.
    vec4 time;           // seconds (wrapped hourly), delta seconds, frame index.
} frame;

// Places a position given relative to a chunk's origin (in blocks) relative
// to the camera. The chunk difference is integer math, so it is exact at any
// distance from the world origin; only the small result becomes a float
// (Section 8.4, ADR-011).
vec3 cameraRelative(ivec3 chunk, vec3 localPosition) {
    return vec3((chunk - frame.cameraChunk.xyz) * 16) + localPosition - frame.cameraFraction.xyz;
}
