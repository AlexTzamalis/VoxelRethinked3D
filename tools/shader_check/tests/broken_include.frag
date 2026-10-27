// Includes a file with an error; the checker must name the include file.
#include "include/broken.glsl"

layout(location = 0) out vec4 outColor;

void main() {
    outColor = brokenColor();
}
