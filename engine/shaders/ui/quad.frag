// Game UI fragments: a flat color, the color times the atlas (text is white
// glyphs, so the color decides the text color), or the panorama: the view ray
// of the pixel looks up a 360 degree image (equirectangular: longitude across,
// latitude down).
#include "include/bindings.glsl"

VXE_BINDING(2, 1) uniform sampler2D uiTexture;
VXE_BINDING(2, 2) uniform sampler2D panoramaTexture;

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in uint vFlags;
layout(location = 3) flat in vec4 vPanorama;
layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265358979;

// vUv runs from -1 to 1 across the window, y up. vPanorama holds yaw, the
// tangents of half the view angle across and up, and pitch.
vec3 panoramaRay() {
    const float yaw = vPanorama.x;
    const float pitch = vPanorama.w;
    const vec3 view = vec3(vUv.x * vPanorama.y, vUv.y * vPanorama.z, -1.0);
    const vec3 tilted = vec3(view.x, view.y * cos(pitch) + sin(pitch), view.y * sin(pitch) - cos(pitch));
    const vec3 world = vec3(tilted.x * cos(yaw) - tilted.z * sin(yaw), tilted.y,
                            tilted.x * sin(yaw) + tilted.z * cos(yaw));
    return normalize(world);
}

void main() {
    if ((vFlags & 2u) != 0u) {
        const vec3 ray = panoramaRay();
        const float longitude = atan(ray.x, -ray.z);
        const vec2 uv = vec2(longitude / (2.0 * kPi), 0.5 - asin(clamp(ray.y, -1.0, 1.0)) / kPi);
        outColor = vec4(texture(panoramaTexture, uv).rgb, 1.0);
        return;
    }
    vec4 color = vColor;
    if ((vFlags & 1u) != 0u) {
        color *= texture(uiTexture, vUv);
    }
    if (color.a <= 0.0) {
        discard;
    }
    outColor = color;
}
