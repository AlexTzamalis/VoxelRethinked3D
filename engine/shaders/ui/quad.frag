// Game UI fragments: a flat color, or the color times the font texture for
// text (white glyphs, so the color decides the text color).
#include "include/bindings.glsl"

VXE_BINDING(2, 1) uniform sampler2D uiTexture;

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in uint vFlags;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 color = vColor;
    if ((vFlags & 1u) != 0u) {
        color *= texture(uiTexture, vUv);
    }
    if (color.a <= 0.0) {
        discard;
    }
    outColor = color;
}
