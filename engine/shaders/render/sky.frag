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

VXE_BINDING_STD140(1, 0) uniform SkyParams {
    vec4 sunDirection; // xyz: towards the sun, w: cosine of the sun disk radius.
    vec4 zenithColor;
    vec4 horizonColor;
    vec4 sunColor;     // rgb: sun disk and glow color, a: glow strength.
    vec4 fog;          // start (blocks), density (per block), height falloff (per block), base height (world y).
    ivec4 flags;       // x: sky on, y: fog on.
} sky;

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

vec3 skyColor(vec3 direction) {
    // Gradient: horizon color at and below the horizon, zenith color above,
    // with a curve that keeps a band of horizon haze.
    const float height = clamp(direction.y, 0.0, 1.0);
    vec3 color = mix(sky.horizonColor.rgb, sky.zenithColor.rgb, pow(height, 0.45));
    // Below the horizon (where terrain will normally be) the haze darkens
    // towards a ground tone, so empty space under the horizon does not look
    // like a second sky.
    color = mix(color, sky.horizonColor.rgb * 0.45, smoothstep(0.0, 0.35, -direction.y));
    const float towardsSun = dot(direction, sky.sunDirection.xyz);
    // Glow: wide and soft, strongest near the horizon (longer path through air).
    color += sky.sunColor.rgb * sky.sunColor.a * pow(max(towardsSun, 0.0), 8.0) * (1.0 - 0.6 * height);
    // Disk with a slightly soft edge.
    const float edge = sky.sunDirection.w;
    color += sky.sunColor.rgb * smoothstep(edge, edge + (1.0 - edge) * 0.25, towardsSun);
    return color;
}

// The fraction of light lost to fog along a ray from the camera, for fog
// whose density falls off exponentially with height. The integral of
// density * exp(-falloff * (y - base)) along the ray has a closed form, so no
// marching is needed; the small slope case uses its limit to avoid 0 / 0.
float heightFog(float cameraY, vec3 direction, float distance) {
    const float start = sky.fog.x;
    const float density = sky.fog.y;
    const float falloff = sky.fog.z;
    const float base = sky.fog.w;
    const float travelled = max(distance - start, 0.0);
    if (travelled <= 0.0) {
        return 0.0;
    }
    const float startY = cameraY + direction.y * start - base;
    const float slope = falloff * direction.y * travelled;
    const float shape = abs(slope) > 1e-4 ? (1.0 - exp(-slope)) / slope : 1.0;
    const float opticalDepth = density * travelled * exp(-falloff * startY) * shape;
    return clamp(1.0 - exp(-opticalDepth), 0.0, 1.0);
}

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
    const float cameraY = float(frame.cameraChunk.y * 16) + frame.cameraFraction.y;
    const float amount = heightFog(cameraY, direction, distance);
    // Fog takes the sky's color near the horizon in that direction, so far
    // terrain fades into exactly the sky behind it.
    outColor = vec4(skyColor(normalize(vec3(direction.x, max(direction.y, 0.0) * 0.25, direction.z))), amount);
}
