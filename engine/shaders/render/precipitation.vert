// Falling rain and snow (render::PrecipitationRenderer): one quad per block
// column around the camera, gl_VertexID / 6 the column and gl_VertexID % 6 its
// corner. The quad turns to face the camera around the upright axis, reaches
// from the column's highest block up past the camera, and the streak image
// scrolls down it.
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform PrecipitationParams {
    vec4 shape;   // x: amount 0 to 1, y: 1 for snow, z: seconds.
    vec4 light;   // rgb: the color of sky light.
    ivec4 corner; // x, z: the grid's corner block, relative to the camera's chunk.
} params;

// The world height of each column's highest block, relative to the camera's chunk.
VXE_BINDING(2, 0) uniform sampler2D tops;

layout(location = 0) out vec2 vUv;
layout(location = 1) out float vFade;

const int kSide = 33;       // render::kPrecipitationSide.
const float kReach = 16.0;  // render::kPrecipitationReach.
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                 vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(0.0, 0.0));

uint hashOf(ivec2 cell) {
    uint hash = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u;
    hash = (hash ^ (hash >> 13u)) * 1274126177u;
    return hash ^ (hash >> 16u);
}

void hide() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // Outside the view: the triangle is dropped.
    vUv = vec2(0.0);
    vFade = 0.0;
}

void main() {
    const int column = gl_VertexID / 6;
    const ivec2 cell = ivec2(column % kSide, column / kSide);
    const ivec2 block = params.corner.xz + cell;
    // From the world position, so a column keeps its streaks as the camera moves.
    const uint hash = hashOf(block + frame.cameraChunk.xz * 16);
    if (float(hash & 1023u) / 1023.0 >= params.shape.x) {
        hide();
        return;
    }
    const vec2 middle = vec2(block) + 0.5 - frame.cameraFraction.xz;
    const float distance = length(middle);
    const float eye = frame.cameraFraction.y;
    const float bottom = max(texelFetch(tops, cell, 0).r + 1.0, eye - kReach);
    const float top = eye + kReach;
    if (distance > kReach || top <= bottom) {
        hide();
        return;
    }
    // Half a block to each side, across the line of sight.
    const vec2 across = distance > 0.01 ? vec2(-middle.y, middle.x) / distance * 0.5 : vec2(0.5, 0.0);
    const vec2 corner = kCorners[gl_VertexID % 6];
    const float height = mix(bottom, top, corner.y);
    const vec2 place = middle + across * (corner.x * 2.0 - 1.0);
    gl_Position = frame.viewProjection * vec4(place.x, height - eye, place.y, 1.0);

    // The image repeats every 16 blocks up, so only the chunk's place in that
    // repeat matters, which keeps the number small.
    const bool snow = params.shape.y > 0.5;
    const float worldHeight = height + float((frame.cameraChunk.y & 15) * 16);
    const float offset = float((hash >> 12u) & 255u) / 255.0;
    const float streak = float((hash >> 10u) & 3u);
    const float seconds = params.shape.z;
    const float drift = snow ? sin(seconds * 0.6 + offset * 6.2831) * 0.08 : 0.0;
    const float fall = snow ? 0.12 : 1.4; // Image heights per second.
    vUv = vec2((corner.x + streak) * 0.25 + drift, worldHeight / 16.0 + seconds * fall + offset);
    vFade = 1.0 - smoothstep(kReach * 0.5, kReach, distance);
}
