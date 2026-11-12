// Entities (render::EntityRenderer): the skin or item texture times the light
// the CPU worked out for the face, with holes cut where the texture is clear
// (the second layer of a skin).
#include "include/bindings.glsl"

VXE_BINDING(2, 1) uniform sampler2DArray entityTextures;

layout(location = 0) in vec3 vUvLayer;
layout(location = 1) in float vLight;
layout(location = 0) out vec4 outColor;

void main() {
    const vec4 texel = texture(entityTextures, vUvLayer);
    if (texel.a < 0.5) {
        discard;
    }
    outColor = vec4(texel.rgb * vLight, 1.0);
}
