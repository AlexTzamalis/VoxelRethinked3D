// Far terrain quads (render::LodRenderer, Section 13.5), drawn by vertex
// pulling like the chunk quads: gl_VertexID / 4 is the quad in the arena
// (12 bytes, voxel::LodQuad) and gl_VertexID % 4 its corner. The tile comes
// from the per frame tile buffer at gl_BaseInstance, with its position
// already relative to the camera's chunk (exact at any distance, ADR-011).
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD430(2, 0) readonly buffer Quads {
    uint words[];
};

struct TileInfo {
    ivec4 origin; // x, z: the tile's corner minus the camera chunk's corner; y: base Y likewise; w: cell size.
};

VXE_BINDING_STD430(2, 1) readonly buffer Tiles {
    TileInfo tiles[];
};

VXE_BINDING_STD430(2, 2) readonly buffer Materials {
    uint materials[]; // RGBA8 per material, sRGB.
};

VXE_BINDING_STD140(1, 0) uniform LodParams {
    vec4 params; // x: daylight, y: darkest brightness, z: coverage (0 off, 1 discard, 2 show), w: color by level.
} lodParams;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec3 vRelative; // Camera relative position, for the coverage map.

// Brightness per face (+X, -X, +Y, -Y, +Z, -Z), as on chunks.
const float kFaceShade[6] = float[6](0.8, 0.8, 1.0, 0.5, 0.65, 0.65);
const vec3 kLevelColors[6] = vec3[6](vec3(1.0, 0.3, 0.3), vec3(1.0, 0.7, 0.2), vec3(0.9, 1.0, 0.3),
                                     vec3(0.3, 1.0, 0.4), vec3(0.3, 0.7, 1.0), vec3(0.7, 0.4, 1.0));

float lightCurve(float level) {
    return pow(0.8, 15.0 - level);
}

void main() {
    const uint quad = uint(gl_VertexID) >> 2u;
    const uint corner = uint(gl_VertexID) & 3u;
    const uint w0 = words[quad * 3u];
    const uint w1 = words[quad * 3u + 1u];
    const uint w2 = words[quad * 3u + 2u];
    const TileInfo tile = tiles[gl_BaseInstance];
    const float cell = float(tile.origin.w);

    const float x = float(w0 & 127u) * cell;
    const float z = float((w0 >> 7u) & 127u) * cell;
    const float sizeU = float(((w0 >> 14u) & 63u) + 1u) * cell;
    const float sizeV = float(((w0 >> 20u) & 63u) + 1u) * cell;
    const uint face = (w0 >> 26u) & 7u;
    const float y = float(w1 & 0xFFFFFFu);
    const float height = float(w2 & 0xFFFFFu);

    // Corner 0 and two edges whose cross product points out of the face, so
    // corners 0 1 2 3 run counterclockwise seen from outside.
    vec3 origin = vec3(x, y, z);
    vec3 edgeA;
    vec3 edgeB;
    if (face == 2u) {
        edgeA = vec3(0.0, 0.0, sizeV);
        edgeB = vec3(sizeU, 0.0, 0.0);
    } else if (face == 3u) {
        edgeA = vec3(sizeU, 0.0, 0.0);
        edgeB = vec3(0.0, 0.0, sizeV);
    } else if (face == 0u) {
        origin.x += cell;
        edgeA = vec3(0.0, height, 0.0);
        edgeB = vec3(0.0, 0.0, sizeU);
    } else if (face == 1u) {
        edgeA = vec3(0.0, 0.0, sizeU);
        edgeB = vec3(0.0, height, 0.0);
    } else if (face == 4u) {
        origin.z += cell;
        edgeA = vec3(sizeU, 0.0, 0.0);
        edgeB = vec3(0.0, height, 0.0);
    } else {
        edgeA = vec3(0.0, height, 0.0);
        edgeB = vec3(sizeU, 0.0, 0.0);
    }
    const vec3 local = origin + edgeA * float(corner == 1u || corner == 2u) + edgeB * float(corner >= 2u);
    const vec3 relative = vec3(tile.origin.xyz) + local - frame.cameraFraction.xyz;
    vRelative = relative;
    gl_Position = frame.viewProjection * vec4(relative, 1.0);

    vec3 albedo = pow(unpackUnorm4x8(materials[w2 >> 20u]).rgb, vec3(2.2));
    if (lodParams.params.w > 0.5) {
        // The cell size is 2^level.
        albedo = kLevelColors[clamp(findMSB(uint(tile.origin.w)) - 1, 0, 5)];
    }
    const float sky = lightCurve(float((w1 >> 24u) & 15u)) * lodParams.params.x;
    vColor = albedo * kFaceShade[face] * mix(lodParams.params.y, 1.0, sky);
}
