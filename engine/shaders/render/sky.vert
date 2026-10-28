// The sky and fog pass (render::SkyRenderer): one triangle that covers the
// screen. Each pixel gets its clip space position, from which the fragment
// shader rebuilds the view ray with the inverse view projection.

layout(location = 0) out vec2 vClip;

void main() {
    const vec2 position = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    vClip = position;
    gl_Position = vec4(position, 0.0, 1.0);
}
