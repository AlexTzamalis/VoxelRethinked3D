// Sun rays (render::GodRayRenderer), at half resolution, in two parts added
// together:
//
// - Shafts towards the sun on the screen: from each pixel a march towards the
//   sun's place on the screen, adding up the open sky it passes (the scene
//   depth is 0 there, ADR-010) less what the clouds hide. Samples nearer the
//   pixel count more, so a shaft fades away from its gap. Only while the sun
//   is in view.
// - Light in the air, in every direction: the view ray is followed into the
//   scene, and at each step the shadow map, the mountain shadows and the cloud
//   shadows say whether the sun reaches that bit of air. Lit air scatters a
//   little light towards the eye, mostly forwards, so beams through leaves and
//   past ridges show from the side and from behind too, down to where they
//   land.
#include "include/bindings.glsl"
#include "include/frame.glsl"
#include "include/sunlight.glsl"

VXE_BINDING_STD140(1, 0) uniform GodRayParams {
    vec4 sun;    // xy: the sun on screen (0 to 1), z: strength of the screen shafts.
    vec4 color;  // rgb: the color of the light (sun or moon).
    vec4 volume; // x: strength of the light in the air (0 off), y: reach in blocks, z: steps.
} params;

// The sun and moon shadow map (render::ChunkRenderer), as chunk.frag reads it.
VXE_BINDING_STD140(1, 2) uniform ShadowParams {
    mat4 matrices[4];
    vec4 splits;
    vec4 depthRange;
    vec4 radius;
    vec4 toLight;  // xyz towards the light, w strength (0 is off).
    vec4 settings; // x cascades.
} shadow;
VXE_BINDING(2, 5) uniform sampler2D shadowMap;

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;
VXE_BINDING(2, 1) uniform sampler2D cloudLayer;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

const int kSamples = 48;
const float kReach = 0.85; // Of the way to the sun the march covers.
const float kDecay = 0.965;
const float kAirDensity = 0.0011; // Light scattered per block of lit air.
const float kSkyReach = 48.0;     // Blocks of air in front of open sky: more is only haze.
const float kForward = 0.45;       // How strongly the air scatters forwards (Henyey-Greenstein g).

float screenShafts(vec2 uv, float jitter) {
    const vec2 step = (params.sun.xy - uv) * (kReach / float(kSamples));
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
    return shafts * params.sun.z * own;
}

// Whether the shadow map has the light reach this camera relative point.
float shadowMapLit(vec3 point) {
    if (shadow.toLight.w <= 0.0) {
        return 1.0;
    }
    const float distance = length(point);
    const int cascades = int(shadow.settings.x);
    for (int c = 0; c < cascades; ++c) {
        if (distance >= shadow.splits[c]) {
            continue;
        }
        const vec4 light = shadow.matrices[c] * vec4(point, 1.0);
        const vec2 uv = (light.xy * 0.5) + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            return 1.0;
        }
        const vec2 tile = vec2(float(c % 2), float(c / 2));
        return texture(shadowMap, (uv + tile) * 0.5).r > light.z + 0.0005 ? 1.0 - shadow.toLight.w : 1.0;
    }
    return 1.0;
}

float airLight(vec3 direction, float sceneDistance, float jitter) {
    const float reach = min(sceneDistance, params.volume.y);
    const int steps = int(params.volume.z);
    const float stepLength = reach / float(steps);
    float lit = 0.0;
    for (int i = 0; i < steps; ++i) {
        const vec3 point = direction * (stepLength * (float(i) + jitter));
        lit += shadowMapLit(point) * farSunlit(point);
    }
    // Henyey-Greenstein, mixed with some even scattering so the light in the
    // air still shows looking away from the sun; 1 means even.
    const float g = kForward;
    const float cosine = dot(direction, sunlight.toLight.xyz);
    const float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosine, 1.5);
    // Capped forwards: towards a low sun the screen shafts and the sky's glow
    // already carry the brightness, and more would wash the view out.
    const float phase = min(mix(1.0, hg, 0.5), 1.5);
    return lit * stepLength * kAirDensity * phase * params.volume.x;
}

void main() {
    const vec2 uv = (vClip * 0.5) + 0.5;
    // A pixel's own place in each march is jittered, which hides the steps.
    const float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float light = params.sun.z > 0.0 ? screenShafts(uv, jitter) : 0.0;
    if (params.volume.x > 0.0) {
        const vec4 nearPoint = frame.inverseViewProjection * vec4(vClip, 1.0, 1.0);
        const vec3 direction = normalize(nearPoint.xyz / nearPoint.w);
        // This pass has half the pixels of the depth buffer.
        const float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy) * 2, 0).r;
        float sceneDistance = kSkyReach;
        if (depth > 0.0) {
            const vec4 point = frame.inverseViewProjection * vec4(vClip, depth, 1.0);
            sceneDistance = length(point.xyz / point.w);
        }
        light += airLight(direction, sceneDistance, jitter);
    }
    outColor = vec4(params.color.rgb * light, 1.0);
}
