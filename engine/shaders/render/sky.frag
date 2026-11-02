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
VXE_BINDING(2, 1) uniform sampler2DArray bodies; // The sun, then the moon's phases.
VXE_BINDING(2, 2) uniform sampler2D galaxy;      // The whole sphere of the night sky.

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

// A direction in the turning night sky.
vec3 inNightSky(vec3 direction) {
    const vec3 axis = sky.stars.xyz;
    const float c = cos(sky.stars.w);
    const float s = sin(sky.stars.w);
    return direction * c + cross(axis, direction) * s + axis * dot(axis, direction) * (1.0 - c);
}

// Small hard stars, from a hash of the cells of a grid around the viewer: few
// cells hold one, and it stays in its place as the sky turns.
float starAt(vec3 direction) {
    const vec3 position = direction * 170.0;
    const ivec3 cell = ivec3(floor(position));
    uint hash = uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u;
    hash = (hash ^ (hash >> 13u)) * 1274126177u;
    hash ^= hash >> 16u;
    if ((hash & 1023u) > 2u) {
        return 0.0;
    }
    const vec3 inside = abs(fract(position) - 0.5);
    if (max(inside.x, max(inside.y, inside.z)) > 0.2) {
        return 0.0;
    }
    return 0.25 + float((hash >> 10u) & 255u) / 255.0 * 0.75;
}

// The stars and the galaxy, added to the sky and gone towards the horizon, where
// the air is thickest.
vec3 withNight(vec3 color, vec3 direction) {
    if (sky.night.x <= 0.0 || direction.y <= 0.0) {
        return color;
    }
    const float air = smoothstep(0.0, 0.3, direction.y);
    const vec3 turned = inNightSky(direction);
    vec3 light = vec3(starAt(turned)) * sky.night.x;
    if (sky.night.y > 0.0) {
        const vec2 uv = vec2(atan(turned.z, turned.x) / 6.28318530718 + 0.5, acos(clamp(turned.y, -1.0, 1.0)) / 3.14159265359);
        light += textureLod(galaxy, uv, 0.0).rgb * sky.night.y;
    }
    return color + light * air;
}

// The image of a body in the sky at a direction: a flat square facing the
// viewer, with its top along the body's path, so it never spins as it moves.
vec4 bodyImage(vec3 direction, vec3 body, float halfSize, int layer) {
    const float along = dot(direction, body);
    if (along <= 0.0) {
        return vec4(0.0);
    }
    const vec3 right = normalize(cross(body, vec3(0.0, 0.0, 1.0)));
    const vec3 up = cross(right, body);
    const vec2 at = vec2(dot(direction, right), dot(direction, up)) / (along * halfSize);
    if (abs(at.x) >= 1.0 || abs(at.y) >= 1.0) {
        return vec4(0.0);
    }
    return texture(bodies, vec3(vec2(at.x, -at.y) * 0.5 + 0.5, float(layer)));
}

// The sun and moon in front of the sky color.
vec3 withBodies(vec3 color, vec3 direction) {
    const float towardsSun = dot(direction, sky.sunDirection.xyz);
    if (sky.flags.z == 0) {
        // Until the images load: a disk with a slightly soft edge.
        const float edge = sky.sunDirection.w;
        return color + sky.sunColor.rgb * smoothstep(edge, edge + (1.0 - edge) * 0.25, towardsSun);
    }
    const float halfSize = sky.moonDirection.w;
    const vec4 sun = bodyImage(direction, sky.sunDirection.xyz, halfSize, 0);
    color += sun.rgb * sun.a * sky.sunColor.rgb * 3.0;
    const vec4 moon = bodyImage(direction, sky.moonDirection.xyz, halfSize, sky.flags.w);
    color = mix(color, moon.rgb * sky.moonColor.rgb * 1.2, moon.a * sky.moonColor.a);
    return color;
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
        outColor = vec4(withBodies(withNight(skyColor(direction), direction), direction), 1.0);
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
