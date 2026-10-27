// One triangle that covers the whole screen, from gl_VertexID alone. It is
// larger than the screen on purpose: a single triangle avoids the seam and
// the duplicated pixel work of two triangles along a quad's diagonal.

layout(location = 0) out vec2 vUv;

void main() {
    const vec2 position = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    vUv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
