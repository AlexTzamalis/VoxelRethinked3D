// The sky and fog pass (render::SkyRenderer, Section 8.1 pass 6).
//
// Where nothing was drawn (depth 0, the far end of reversed Z) the pixel gets
// the analytical sky: a gradient from the horizon to the zenith, a sun disk
// and a glow around it, all tinted by how high the sun stands. Over geometry
// the pass blends exponential height fog: thick near fogBaseHeight, thinning
// out above it, and towards the horizon color so distant terrain melts into
// the sky. Output alpha is the fog amount, so blending keeps the scene color
// where there is no fog.
#include "include/bindings.glsl"
#include "include/frame.glsl"
#include "include/sky.glsl"

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

void main() {
    // A point on the near plane (depth 1 in reversed Z) gives the view ray.
    const vec4 nearPoint = frame.inverseViewProjection * vec4(vClip, 1.0, 1.0);
    const vec3 direction = normalize(nearPoint.xyz / nearPoint.w);
    const float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (depth <= 0.0) {
        if (sky.flags.x == 0) {
            discard;
        }
        outColor = vec4(skyColor(direction), 1.0);
        return;
    }
    if (sky.flags.y == 0) {
        discard;
    }
    const vec4 point = frame.inverseViewProjection * vec4(vClip, depth, 1.0);
    const float distance = length(point.xyz / point.w);
    const float amount = heightFog(cameraWorldY(), direction, distance);
    outColor = vec4(fogColor(direction), amount);
}
