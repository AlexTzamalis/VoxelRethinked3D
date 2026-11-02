// Falling rain and snow (render::PrecipitationRenderer): the streak image, lit
// by the sky, fading out towards the edge of the grid.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 0) uniform PrecipitationParams {
    vec4 shape;   // x: amount 0 to 1, y: 1 for snow, z: seconds.
    vec4 light;   // rgb: the color of sky light.
    ivec4 corner;
} params;

// Rain in layer 0, snow in layer 1.
VXE_BINDING(2, 1) uniform sampler2DArray streaks;

layout(location = 0) in vec2 vUv;
layout(location = 1) in float vFade;
layout(location = 0) out vec4 outColor;

void main() {
    const bool snow = params.shape.y > 0.5;
    const vec4 texel = texture(streaks, vec3(vUv, snow ? 1.0 : 0.0));
    const float alpha = texel.a * vFade * (snow ? 0.95 : 0.7);
    if (alpha < 0.02) {
        discard;
    }
    outColor = vec4(texel.rgb * params.light.rgb, alpha);
}
