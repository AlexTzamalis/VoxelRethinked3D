// Adds the half resolution god ray layer (render/god_rays.frag) to the scene.
#include "include/bindings.glsl"

VXE_BINDING(2, 0) uniform sampler2D rays;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(rays, (vClip * 0.5) + 0.5).rgb, 0.0);
}
