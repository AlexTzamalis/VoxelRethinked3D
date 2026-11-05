// God rays (render::GodRayRenderer), at half resolution: from each pixel a
// march towards the sun on the screen, adding up the open sky it passes (the
// scene depth is 0 there, ADR-010) less what the clouds hide. Samples nearer
// the pixel count more, so a shaft fades away from its gap.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 0) uniform GodRayParams {
    vec4 sun;   // xy: the sun on screen (0 to 1), z: strength.
    vec4 color; // rgb: the sun's color.
} params;

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;
VXE_BINDING(2, 1) uniform sampler2D cloudLayer;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

const int kSamples = 48;
const float kReach = 0.85; // Of the way to the sun the march covers.
const float kDecay = 0.965;

void main() {
    const vec2 uv = (vClip * 0.5) + 0.5;
    const vec2 step = (params.sun.xy - uv) * (kReach / float(kSamples));
    // A pixel's own place in the march is jittered, which hides the steps.
    const float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec2 at = uv + (step * jitter);
    float light = 0.0;
    float weight = 1.0;
    float total = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        if (any(lessThan(at, vec2(0.0))) || any(greaterThan(at, vec2(1.0)))) {
            break;
        }
        const float sky = texture(sceneDepth, at).r <= 0.0 ? 1.0 : 0.0;
        light += sky * (1.0 - texture(cloudLayer, at).a) * weight;
        total += weight;
        weight *= kDecay;
        at += step;
    }
    const float shafts = total > 0.0 ? light / total : 0.0;
    // Shafts show where light streams past something: open sky around the
    // sun is already bright, so a pixel of sky adds little of its own.
    const float own = texture(sceneDepth, uv).r <= 0.0 ? 0.35 : 1.0;
    outColor = vec4(params.color.rgb * shafts * params.sun.z * own, 1.0);
}
