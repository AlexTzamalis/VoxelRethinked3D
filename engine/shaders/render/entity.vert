// Entities (render::EntityRenderer): triangles built on the CPU, camera
// relative, pulled from a buffer by gl_VertexID (three vertices a triangle).
//
// With VXE_ENTITY_FIRST_PERSON the hand and its item are pushed to the front
// of the depth range: reversed Z (ADR-010) puts the nearest at 1, and the hand
// keeps its own order inside the last hundredth, so it is never hidden by a
// wall it would reach into.
#include "include/bindings.glsl"
#include "include/frame.glsl"

struct EntityVertex {
    vec4 position; // xyz camera relative, w brightness.
    vec4 uv;       // xy texture, z layer.
};

VXE_BINDING_STD430(2, 0) readonly buffer EntityVertices {
    EntityVertex vertices[];
} entities;

layout(location = 0) out vec3 vUvLayer;
layout(location = 1) out float vLight;

void main() {
    const EntityVertex vertex = entities.vertices[gl_VertexID];
    gl_Position = frame.viewProjection * vec4(vertex.position.xyz, 1.0);
#ifdef VXE_ENTITY_FIRST_PERSON
    gl_Position.z = (1.0 - (1.0 - gl_Position.z / gl_Position.w) * 0.01) * gl_Position.w;
#endif
    // Images start at their top row, textures at their bottom one (ADR-016).
    vUvLayer = vec3(vertex.uv.x, 1.0 - vertex.uv.y, vertex.uv.z);
    vLight = vertex.position.w;
}
