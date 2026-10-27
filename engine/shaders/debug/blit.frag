// Copies a texture to the screen, for showing offscreen targets.
#include "include/bindings.glsl"

VXE_BINDING(2, 0) uniform sampler2D sourceTexture;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sourceTexture, vUv);
}
