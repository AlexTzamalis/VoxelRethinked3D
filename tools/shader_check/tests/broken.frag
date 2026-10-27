// A deliberately broken shader: the checker must report line 4.
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(undefinedValue, 1.0);
}
