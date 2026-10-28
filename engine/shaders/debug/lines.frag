// Debug lines (render::DebugDraw): the vertex color, alpha blended.
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
