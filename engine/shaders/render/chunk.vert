// Chunk quads (render::ChunkRenderer, Section 8.2), drawn by vertex pulling:
// no vertex attributes, the shader reads each quad's 8 bytes from the arena
// and builds its 4 corners. The index buffer repeats 0 1 2 0 2 3 per quad
// and the draw's base vertex is the chunk's first quad times 4, so
// gl_VertexID / 4 is the quad in the arena and gl_VertexID % 4 the corner.
// The chunk's position comes from the metadata buffer at gl_BaseInstance.
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform ChunkParams {
    vec4 params; // x: AO strength (0 none, 1 full), y: daylight (0 to 1), z: darkest brightness, w: face shading.
    vec4 skyLight; // rgb: the color of sky light right now.
    vec4 tints[16]; // Linear color per tint kind; kind 0 is white.
} chunkParams;

VXE_BINDING_STD430(2, 0) readonly buffer Quads {
    uvec2 quads[];
};

// Per texture layer: 0 for a still texture, else the frame count (bits 0..7),
// game ticks per frame (8..15) and the layer of frame 2 (16..31), as
// render::BlockTextureData::animations.
VXE_BINDING_STD430(2, 3) readonly buffer Animations {
    uint animations[];
};

// Per texture layer, voxel::textureLook: bits 0..15 the layer drawn over it
// plus 1 (0 for none), 16..19 its tint, 20..23 the overlay's tint.
VXE_BINDING_STD430(2, 4) readonly buffer TextureLooks {
    uint textureLooks[];
};

struct ChunkInfo {
    ivec4 position; // xyz: chunk position, w: unused.
};

VXE_BINDING_STD430(2, 1) readonly buffer Chunks {
    ChunkInfo chunks[];
};

layout(location = 0) out vec3 vUvLayer; // xy: texture coordinates in blocks, z: texture array layer.
layout(location = 1) out vec3 vShade;   // Face brightness, ambient occlusion and light.
layout(location = 2) out vec3 vRelative; // Camera relative position (translucent fog).
layout(location = 3) flat out vec3 vTint;        // Multiplies the block texture.
layout(location = 4) flat out vec3 vOverlayTint; // Multiplies the overlay texture.
layout(location = 5) flat out uint vOverlayLayer; // The overlay's layer plus 1, 0 for none.

// Per face (+X, -X, +Y, -Y, +Z, -Z) the U and V axes (voxel::faceAxes) and
// the brightness of the classic directional shading (lighting stage A,
// Section 14.1): tops brightest, bottoms darkest.
const vec3 kAxisU[3] = vec3[3](vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));
const vec3 kAxisV[3] = vec3[3](vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0));
const float kFaceShade[6] = float[6](0.8, 0.8, 1.0, 0.5, 0.65, 0.65);
// Brightness for AO 0 (two sides closed) to 3 (open).
const float kPlantShade = 0.9;
const float kPlantShift = 0.125; // Blocks per step of a plant's shift.
const float kBushySize = 1.7;    // Blocks across a bushy leaf plane.
const float kBushyTurn = 0.07;   // Radians per step of a bushy plane's turn, about 28 degrees at most.
const float kAoCurve[4] = float[4](0.45, 0.62, 0.8, 1.0);
// Block light is firelight: a little warm next to white daylight.
const vec3 kBlockLightTint = vec3(1.0, 0.86, 0.68);

// Brightness of a light level from 0 to 15: each level a fifth darker than
// the one above, so 15 is full, 8 about a fifth and 0 nearly black. Eyes
// judge light by ratios, so a straight line from 0 to 15 looks flat.
float lightCurve(float level) {
    return pow(0.8, 15.0 - level);
}

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
    const bool crossed = face >= 6u;

    const vec3 origin = vec3(float(quad.x & 31u), float((quad.x >> 5u) & 31u), float((quad.x >> 10u) & 31u));
    const float width = float(((quad.x >> 15u) & 15u) + 1u);
    const float height = float(((quad.x >> 19u) & 15u) + 1u);
    vec3 local;
    if (crossed) {
        // A diagonal plane through the block: faces 6 and 7 cross. Plants
        // stand on the block's floor with a small shift of their own; bushy
        // leaf planes are larger and centered.
        const bool bushy = (quad.x >> 31u) != 0u;
        const float size = bushy ? kBushySize : 1.0;
        const float shiftX = bushy ? 0.0 : (float((quad.x >> 27u) & 3u) - 1.5) * kPlantShift;
        const float shiftZ = bushy ? 0.0 : (float((quad.x >> 29u) & 3u) - 1.5) * kPlantShift;
        const float along = (cornerU - 0.5) * size;
        const float up = bushy ? 0.5 + (cornerV - 0.5) * size : cornerV;
        float directionX = 1.0;
        float directionZ = face == 6u ? 1.0 : -1.0;
        if (bushy) {
            // Each block turns its planes a little. All at exactly 45 degrees,
            // a whole tree's planes go edge on together as the view turns, and
            // neighbors' planes share one plane and flicker where they overlap.
            const float turn = (float((quad.x >> 27u) & 15u) - 7.5) * kBushyTurn;
            const float c = cos(turn);
            const float s = sin(turn);
            const float x = directionX * c - directionZ * s;
            directionZ = directionX * s + directionZ * c;
            directionX = x;
        }
        local = origin + vec3(0.5 + shiftX + along * directionX, up, 0.5 + shiftZ + along * directionZ);
    } else {
        const uint axis = face >> 1u;
        local = origin + kAxisU[axis] * (cornerU * width) + kAxisV[axis] * (cornerV * height);
    }
    const ivec3 chunk = chunks[gl_BaseInstance].position.xyz;
    vRelative = cameraRelative(chunk, local);
    gl_Position = frame.viewProjection * vec4(vRelative, 1.0);

    // Textures repeat once per block. On +X and -Z faces U runs right to
    // left when seen from outside, so it is mirrored to keep textures the
    // right way around; V is up on every side face.
    const bool mirrorU = face == 0u || face == 5u;
    const float u = (mirrorU ? 1.0 - cornerU : cornerU) * (crossed ? 1.0 : width);
    // Animated textures step through their frames at 20 game ticks per second.
    uint layer = quad.y & 0x3FFFu;
    const uint look = textureLooks[layer];
    vTint = chunkParams.tints[(look >> 16u) & 15u].rgb;
    vOverlayTint = chunkParams.tints[(look >> 20u) & 15u].rgb;
    vOverlayLayer = look & 0xFFFFu;
    const uint animation = animations[layer];
    if ((animation & 0xFFu) > 1u) {
        const uint ticks = uint(frame.time.x * 20.0) / max((animation >> 8u) & 0xFFu, 1u);
        const uint step = ticks % (animation & 0xFFu);
        layer = step == 0u ? layer : (animation >> 16u) + step - 1u;
    }
    vUvLayer = vec3(u, cornerV * (crossed ? 1.0 : height), float(layer));

    const uint ao = (quad.y >> (14u + 2u * corner)) & 3u;
    const float occlusion = mix(1.0, kAoCurve[ao], chunkParams.params.x);
    // The brighter of sky light (scaled by the time of day) and block light.
    const vec3 sky = lightCurve(float((quad.y >> 22u) & 15u)) * chunkParams.skyLight.rgb;
    const vec3 block = lightCurve(float((quad.y >> 26u) & 15u)) * kBlockLightTint;
    const vec3 light = mix(vec3(chunkParams.params.z), vec3(1.0), max(sky, block));
    const float faceShade = crossed ? kPlantShade : kFaceShade[face];
    vShade = mix(1.0, faceShade, chunkParams.params.w) * occlusion * light;
}
