// Dear ImGui fragments: vertex color times the bound texture (the font atlas
// for text and shapes, or any engine texture shown with ImGui::Image).
#include "include/bindings.glsl"

VXE_BINDING(2, 1) uniform sampler2D imguiTexture;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor * texture(imguiTexture, vUv);
}
