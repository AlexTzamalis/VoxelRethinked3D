// Post processing (render::PostProcess, Section 8.1 pass 9): the HDR scene
// to the window. Exposure, a tonemapping curve, optional FXAA on the
// tonemapped image, and sRGB encoding, because the window shows values as
// written (ADR-016) while the scene is lit in linear HDR.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 0) uniform PostParams {
    vec4 settings; // x: exposure, y: tonemapper (0 none, 1 Reinhard, 2 ACES), z: FXAA on, w: unused.
    vec4 texel;    // xy: one texel of the HDR scene in UV units.
} post;

VXE_BINDING(2, 0) uniform sampler2D hdrScene;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 tonemap(vec3 color) {
    color *= post.settings.x;
    const int curve = int(post.settings.y);
    if (curve == 1) {
        return color / (1.0 + color); // Reinhard: soft, never clips.
    }
    if (curve == 2) {
        // ACES filmic curve fitted by Krzysztof Narkowicz: rich contrast and a
        // gentle shoulder, the usual game default. The fit expects its input
        // scaled by 0.6 to match the reference ACES exposure; without it mid
        // tones come out too bright and washed out.
        color *= 0.6;
        return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
    }
    return clamp(color, 0.0, 1.0);
}

vec3 encodeSrgb(vec3 linear) {
    const vec3 low = linear * 12.92;
    const vec3 high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear, vec3(0.0031308)));
}

vec3 sampleDisplay(vec2 uv) {
    return tonemap(textureLod(hdrScene, uv, 0.0).rgb);
}

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

// FXAA in its compact form (Timothy Lottes): find the edge direction from the
// luma of the four diagonal neighbors, blur along it with two or four
// bilinear taps, and keep the four tap result only if it stays within the
// local luma range (otherwise it crossed into a different surface).
vec3 fxaa(vec2 uv) {
    const vec2 texel = post.texel.xy;
    const vec3 rgbM = sampleDisplay(uv);
    const float lumaNW = luma(sampleDisplay(uv + vec2(-1.0, 1.0) * texel));
    const float lumaNE = luma(sampleDisplay(uv + vec2(1.0, 1.0) * texel));
    const float lumaSW = luma(sampleDisplay(uv + vec2(-1.0, -1.0) * texel));
    const float lumaSE = luma(sampleDisplay(uv + vec2(1.0, -1.0) * texel));
    const float lumaM = luma(rgbM);
    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    // Flat areas need no work: leave them sharp.
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) {
        return rgbM;
    }
    vec2 direction = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    const float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, vec2(-8.0), vec2(8.0)) * texel;
    const vec3 rgbA = 0.5 * (sampleDisplay(uv + direction * (1.0 / 3.0 - 0.5)) +
                             sampleDisplay(uv + direction * (2.0 / 3.0 - 0.5)));
    const vec3 rgbB = rgbA * 0.5 + 0.25 * (sampleDisplay(uv - direction * 0.5) + sampleDisplay(uv + direction * 0.5));
    const float lumaB = luma(rgbB);
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

void main() {
    const vec3 display = post.settings.z != 0.0 ? fxaa(vUv) : sampleDisplay(vUv);
    outColor = vec4(encodeSrgb(display), 1.0);
}
