// The clouds (render::CloudRenderer): a volume marched at half resolution into
// its own layer, which render/clouds_blit.frag then lays over the scene.
//
// A density map says where clouds stand, and how much cloud stands there sets
// how far it reaches up and down, so each cloud is a heap with a flatter base.
// Two layers of noise wear its edges away. The ray takes steps through the
// layer, and at each step inside a cloud it looks a few steps towards the sun
// or the moon to find how much cloud is in the way. That is what shades them:
// lit tops, dark bellies and bright edges, from the light alone.
//
// The ray ends at the scene depth, so mountains stand in and through the clouds.
// Each pixel starts its ray at a slightly different point, which turns the
// bands of a small number of steps into fine grain, and the upscale smooths that.
#include "include/bindings.glsl"
#include "include/frame.glsl"
#include "include/sky.glsl"

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;
VXE_BINDING(2, 1) uniform sampler2D cloudMap; // Density by cell, repeats every kMapBlocks.
VXE_BINDING(2, 2) uniform sampler2D detail;   // Pack noise: red smooth, green finer.

VXE_BINDING_STD140(1, 1) uniform CloudParams {
    vec4 shape;  // Height of the layer's floor, its thickness, the density where
                 // clouds start, light lost per block inside a cloud.
    vec4 motion; // Wind offset x and z in blocks, where they start and end fading.
    vec4 style;  // x: blockiness 0 to 1, y: steps per ray, z: 1 with detail noise.
} clouds;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

const float kMapBlocks = 4096.0;   // render::kCloudMapSize * kCloudCellBlocks.
const float kCellBlocks = 8.0;     // render::kCloudCellBlocks.
const int kLightSteps = 3;
const float kLightReach[kLightSteps] = float[](10.0, 28.0, 64.0);

// How much cloud fills a point, 0 to 1. `blocks` is in the map's wrapping space.
float densityAt(vec2 blocks, float y, float level) {
    const float floorY = clouds.shape.x;
    const float thickness = clouds.shape.y;
    const float threshold = clouds.shape.z;
    // Reading the map at the middle of its cell gives flat sided columns; a part
    // of the way there keeps a hint of the world's blocks in the cloud's shape.
    const vec2 cellMiddle = (floor(blocks / kCellBlocks) + 0.5) * kCellBlocks;
    const vec2 at = mix(blocks, cellMiddle, clouds.style.x);
    const float map = textureLod(cloudMap, at / kMapBlocks, level).r;
    if (map <= threshold) {
        return 0.0;
    }
    const float height = (y - floorY) / thickness;
    // The root makes clouds rise quickly from their edges, like heaps.
    const float amount = sqrt((map - threshold) / (1.0 - threshold));
    const float top = 0.3 + 0.7 * amount;
    const float bottom = 0.3 - 0.3 * amount;
    const float body = smoothstep(bottom, bottom + 0.1, height) * (1.0 - smoothstep(top * 0.62, top, height));
    if (body <= 0.0) {
        return 0.0;
    }
    float density = body * (0.5 + amount);
    if (clouds.style.z > 0.5) {
        const float lean = height * 0.37;
        const float wear = textureLod(detail, at / 150.0 + vec2(lean), 0.0).r * 0.55 +
                           textureLod(detail, at / 33.0 - vec2(lean * 2.0), 0.0).g * 0.45;
        density -= (wear - 0.26) * 1.25 * (1.0 - clouds.style.x * 0.6);
    }
    return clamp(density, 0.0, 1.0);
}

void main() {
    const vec4 nearPoint = frame.inverseViewProjection * vec4(vClip, 1.0, 1.0);
    const vec3 direction = normalize(nearPoint.xyz / nearPoint.w);
    // This pass has half the pixels of the depth buffer.
    const float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy) * 2, 0).r;
    float sceneDistance = 1.0e9;
    if (depth > 0.0) {
        const vec4 point = frame.inverseViewProjection * vec4(vClip, depth, 1.0);
        sceneDistance = length(point.xyz / point.w);
    }

    const float floorY = clouds.shape.x;
    const float thickness = clouds.shape.y;
    const float threshold = clouds.shape.z;
    const float eyeY = cameraWorldY();
    float enter = 0.0;
    float leave = 1.0e9;
    if (abs(direction.y) > 1.0e-5) {
        const float toFloor = (floorY - eyeY) / direction.y;
        const float toTop = (floorY + thickness - eyeY) / direction.y;
        enter = max(min(toFloor, toTop), 0.0);
        leave = max(toFloor, toTop);
    } else if (eyeY < floorY || eyeY > floorY + thickness) {
        outColor = vec4(0.0);
        return;
    }
    leave = min(leave, min(sceneDistance, clouds.motion.w));
    if (enter >= leave) {
        outColor = vec4(0.0);
        return;
    }

    // Whole chunks of the camera wrap with the map, which is exact at any
    // distance from the world origin.
    const vec2 start = vec2((frame.cameraChunk.xz & ivec2(255)) * 16) + frame.cameraFraction.xz + clouds.motion.xy;

    // The light of the moment: the sun, or the moon when the sun is down.
    const bool sunUp = sky.sunDirection.y > -0.04;
    const vec3 toLight = sunUp ? sky.sunDirection.xyz : sky.moonDirection.xyz;
    const vec3 direct = sunUp ? sky.sunColor.rgb * clamp(sky.sunDirection.y + 0.3, 0.0, 1.0) * 1.2
                              : sky.skyLight.rgb * 0.5;
    const vec3 ambient = mix(sky.horizonColor.rgb, sky.zenithColor.rgb, 0.4);

    const int steps = int(clouds.style.y);
    const float stepLength = max((leave - enter) / float(steps), 6.0);
    // A different start in each pixel, from a noise with no visible pattern.
    const float grain = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float t = enter + stepLength * grain;

    vec3 color = vec3(0.0);
    float alpha = 0.0;
    for (int i = 0; i < steps && t < leave && alpha < 0.97; ++i) {
        const vec2 blocks = start + direction.xz * t;
        const float y = eyeY + direction.y * t;
        // Empty sky is crossed in long strides, using the map's averaged levels.
        if (textureLod(cloudMap, blocks / kMapBlocks, 3.0).r < threshold - 0.1) {
            t += stepLength * 2.5;
            continue;
        }
        const float level = t < 1500.0 ? 0.0 : 2.0;
        const float density = densityAt(blocks, y, level);
        if (density > 0.004) {
            float blocked = 0.0;
            for (int light = 0; light < kLightSteps; ++light) {
                const float reach = kLightReach[light];
                blocked += densityAt(blocks + toLight.xz * reach, y + toLight.y * reach, level);
            }
            const float through = exp(-blocked * clouds.shape.w * 40.0);
            // Thin edges scatter less light back: the dark rim real clouds have
            // when looked at from the lit side.
            const float gathered = 1.0 - exp(-density * 4.0);
            const float height = clamp((y - floorY) / thickness, 0.0, 1.0);
            vec3 lit = ambient * mix(0.5, 0.95, height) + direct * through * gathered;
            // Far clouds melt into the haze of the horizon.
            lit = mix(lit, sky.horizonColor.rgb, 1.0 - exp(-t * 0.00022));
            const float fade = 1.0 - smoothstep(clouds.motion.z, clouds.motion.w, t);
            const float amount = (1.0 - exp(-density * stepLength * clouds.shape.w * 3.0)) * fade;
            color += lit * amount * (1.0 - alpha);
            alpha += amount * (1.0 - alpha);
        }
        t += stepLength;
    }
    outColor = vec4(color, alpha);
}
