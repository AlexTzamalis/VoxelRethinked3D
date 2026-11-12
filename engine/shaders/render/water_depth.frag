// Copies the scene depth before the translucent chunks draw (render::WaterRenderer),
// so liquid surfaces can read how far the solid scene lies behind them while
// the depth itself stays attached for their depth test.
#include "include/bindings.glsl"

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outDepth;

void main() {
    outDepth = vec4(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r, 0.0, 0.0, 1.0);
}
