// Lays the half resolution cloud layer (render/clouds.frag) over the scene. The
// layer's color is already weighted by its alpha, so it scales up without dark
// fringes, and blending adds it to what the clouds leave of the scene.
#include "include/bindings.glsl"

VXE_BINDING(2, 0) uniform sampler2D cloudLayer;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(cloudLayer, vClip * 0.5 + 0.5);
}
