// The clouds (render::CloudRenderer), one fullscreen pass after the sky.
//
// Clouds are columns on a flat grid: a density map says how much cloud stands
// on each cell, and that sets how far the cell reaches up and down from the
// layer's lower part, in whole steps, so they keep the blocky look of the world.
// Each ray walks from cell to cell through the layer, and in each cell it finds
// how far it runs inside the cloud. Light is lost along that path, so thick
// bodies turn solid while thin edges stay soft, which is what makes them look
// like volumes and not like painted boxes. Walking cells is far cheaper than
// marching through a 3D volume, since a ray only stops at cell borders.
//
// The ray ends at the scene depth, so mountains stand in and through the clouds.
// Far cells use coarser mip levels of the map: a ray at the horizon would
// otherwise cross hundreds of cells to draw something a few pixels tall.
#include "include/bindings.glsl"
#include "include/frame.glsl"
#include "include/sky.glsl"

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;
VXE_BINDING(2, 1) uniform sampler2D cloudMap; // Density by cell, wraps every kMapSize cells.

VXE_BINDING_STD140(1, 1) uniform CloudParams {
    vec4 shape;  // Height of the layer's floor, its thickness, the density where
                 // clouds start, light lost per block inside a cloud.
    vec4 motion; // Wind offset x and z in blocks, where they start and end fading.
} clouds;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

const int kMapSize = 512;     // render::kCloudMapSize.
const float kCellSize = 8.0;  // render::kCloudCellBlocks.
const float kStepHeight = 4.0; // Cloud heights come in whole steps of this many blocks.
const int kMaxCells = 220;

// The map level for a distance along the ray: 8, 32 and 128 block cells.
int levelAt(float distance) {
    return distance < 700.0 ? 0 : (distance < 2400.0 ? 2 : 4);
}

float densityAt(ivec2 cell, int level) {
    return texelFetch(cloudMap, cell & ivec2((kMapSize >> level) - 1), level).r;
}

void main() {
    const vec4 nearPoint = frame.inverseViewProjection * vec4(vClip, 1.0, 1.0);
    const vec3 direction = normalize(nearPoint.xyz / nearPoint.w);
    const float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
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
        discard;
    }
    leave = min(leave, min(sceneDistance, clouds.motion.w));
    if (enter >= leave) {
        discard;
    }

    // Whole chunks of the camera wrap with the map, which is exact at any
    // distance from the world origin.
    const vec2 start = vec2((frame.cameraChunk.xz & ivec2(kMapSize * 8 / 16 - 1)) * 16) +
                       frame.cameraFraction.xz + clouds.motion.xy;

    const vec3 sun = sky.sunDirection.xyz;
    const vec3 ambient = mix(sky.horizonColor.rgb, sky.zenithColor.rgb, 0.35) * 0.95;
    const vec3 direct = sky.sunColor.rgb * clamp(sun.y + 0.25, 0.0, 1.0) * 1.15;
    const ivec2 toSun = ivec2(sign(sun.x), sign(sun.z));

    vec3 color = vec3(0.0);
    float alpha = 0.0;
    float t = enter;
    for (int i = 0; i < kMaxCells && t < leave && alpha < 0.985; ++i) {
        const int level = levelAt(t);
        const float size = kCellSize * float(1 << level);
        const vec2 at = start + direction.xz * t;
        const vec2 corner = floor(at / size);
        const ivec2 cell = ivec2(corner);
        // Distance to the next cell border along each axis.
        const vec2 border = corner * size + step(vec2(0.0), direction.xz) * size;
        const float runX = abs(direction.x) > 1.0e-6 ? (border.x - at.x) / direction.x : 1.0e9;
        const float runZ = abs(direction.z) > 1.0e-6 ? (border.y - at.y) / direction.z : 1.0e9;
        const float next = min(t + max(min(runX, runZ), 0.01), leave);

        const float density = densityAt(cell, level);
        if (density > threshold) {
            // More cloud reaches further, up more than down, in whole steps.
            // The root makes them rise quickly from their edges, like heaps
            // and not like flat sheets.
            const float amount = sqrt((density - threshold) / (1.0 - threshold));
            const float reach = ceil(amount * thickness / kStepHeight) * kStepHeight;
            const float bottom = floorY + thickness * 0.3 - reach * 0.3;
            const float top = floorY + thickness * 0.3 + reach * 0.7;
            // The part of this cell's stretch of ray inside the cloud.
            float inside = next - t;
            float first = t;
            if (abs(direction.y) > 1.0e-5) {
                const float a = (bottom - eyeY) / direction.y;
                const float b = (top - eyeY) / direction.y;
                first = clamp(min(a, b), t, next);
                inside = clamp(max(a, b), t, next) - first;
            } else if (eyeY < bottom || eyeY > top) {
                inside = 0.0;
            }
            if (inside > 0.0) {
                const float y = eyeY + direction.y * (first + inside * 0.5);
                // Bright on top, dim below, and dimmer where cloud stands
                // between this cell and the sun.
                const float height = clamp((y - bottom) / max(top - bottom, 1.0), 0.0, 1.0);
                const float shade = densityAt(cell + toSun, level) > threshold ? 0.68 : 1.0;
                // Walls take less light than tops, and more on the sunny side.
                const bool wall = first <= t + 0.001 && t > enter;
                const float side = wall ? (dot(normalize(direction.xz), sun.xz) < 0.0 ? 0.92 : 0.74) : 1.0;
                vec3 lit = (ambient * mix(0.5, 1.0, height) + direct * mix(0.15, 1.0, height) * shade) * side;
                // Far clouds melt into the haze of the horizon.
                const float haze = 1.0 - exp(-first * 0.00022);
                lit = mix(lit, sky.horizonColor.rgb, haze);
                const float fade = 1.0 - smoothstep(clouds.motion.z, clouds.motion.w, first);
                const float amountOfLight = (1.0 - exp(-inside * clouds.shape.w)) * fade;
                color += lit * amountOfLight * (1.0 - alpha);
                alpha += amountOfLight * (1.0 - alpha);
            }
        }
        t = next;
    }
    if (alpha < 0.002) {
        discard;
    }
    outColor = vec4(color / alpha, alpha);
}
