// Chunk quads (render::ChunkRenderer, Section 8.2), drawn by vertex pulling:
// no vertex attributes, the shader reads each quad's 8 bytes from the arena
// and builds its 4 corners. The index buffer repeats 0 1 2 0 2 3 per quad
// and the draw's base vertex is the chunk's first quad times 4, so
// gl_VertexID / 4 is the quad in the arena and gl_VertexID % 4 the corner.
// The chunk's position comes from the metadata buffer at gl_BaseInstance.
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform ChunkParams {
    vec4 params; // x: AO strength (0 none, 1 full), yzw: unused.
} chunkParams;

VXE_BINDING_STD430(2, 0) readonly buffer Quads {
    uvec2 quads[];
};

struct ChunkInfo {
    ivec4 position; // xyz: chunk position, w: unused.
};

VXE_BINDING_STD430(2, 1) readonly buffer Chunks {
    ChunkInfo chunks[];
};

layout(location = 0) out vec3 vUvLayer; // xy: texture coordinates in blocks, z: texture array layer.
layout(location = 1) out float vShade;  // Face brightness times ambient occlusion.

// Per face (+X, -X, +Y, -Y, +Z, -Z) the U and V axes (voxel::faceAxes) and
// the brightness of the classic directional shading (lighting stage A,
// Section 14.1): tops brightest, bottoms darkest.
const vec3 kAxisU[3] = vec3[3](vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));
const vec3 kAxisV[3] = vec3[3](vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0));
const float kFaceShade[6] = float[6](0.8, 0.8, 1.0, 0.5, 0.65, 0.65);
// Brightness for AO 0 (two sides closed) to 3 (open).
const float kAoCurve[4] = float[4](0.45, 0.62, 0.8, 1.0);

void main() {
    const uvec2 quad = quads[uint(gl_VertexID) >> 2u];
    const uint face = (quad.x >> 23u) & 7u;
    uint corner = uint(gl_VertexID) & 3u;
    // Corners c0 to c3 run counterclockwise seen from the side U x V points
    // to. The flip bit moves the shared diagonal from c0-c2 to c1-c3 (AO,
    // Section 10.3); faces whose normal points the other way (+X, +Y, -Z)
    // walk the corners backwards so they stay front facing from outside.
    if (((quad.x >> 26u) & 1u) != 0u) {
        corner = (corner + 1u) & 3u;
    }
    if (face == 0u || face == 2u || face == 5u) {
        corner = (4u - corner) & 3u;
    }
    const float cornerU = (corner == 1u || corner == 2u) ? 1.0 : 0.0;
    const float cornerV = corner >= 2u ? 1.0 : 0.0;

    const vec3 origin = vec3(float(quad.x & 31u), float((quad.x >> 5u) & 31u), float((quad.x >> 10u) & 31u));
    const float width = float(((quad.x >> 15u) & 15u) + 1u);
    const float height = float(((quad.x >> 19u) & 15u) + 1u);
    const uint axis = face >> 1u;
    const vec3 local = origin + kAxisU[axis] * (cornerU * width) + kAxisV[axis] * (cornerV * height);
    const ivec3 chunk = chunks[gl_BaseInstance].position.xyz;
    gl_Position = frame.viewProjection * vec4(cameraRelative(chunk, local), 1.0);

    // Textures repeat once per block. On +X and -Z faces U runs right to
    // left when seen from outside, so it is mirrored to keep textures the
    // right way around; V is up on every side face.
    const bool mirrorU = face == 0u || face == 5u;
    const float u = (mirrorU ? 1.0 - cornerU : cornerU) * width;
    vUvLayer = vec3(u, cornerV * height, float(quad.y & 0x3FFFu));

    const uint ao = (quad.y >> (14u + 2u * corner)) & 3u;
    const float skyLight = float((quad.y >> 22u) & 15u) / 15.0;
    const float occlusion = mix(1.0, kAoCurve[ao], chunkParams.params.x);
    vShade = kFaceShade[face] * occlusion * skyLight;
}
