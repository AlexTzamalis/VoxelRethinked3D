// Flat shaded, colored cube faces. With only 8 shared corners there are no
// per face normals, so normals come from how a position changes across the
// screen (its screen space derivatives): the two derivatives lie in the face,
// and their cross product points out of it. The object space normal picks
// the face color, so each face keeps its color while the cube turns; the
// world space normal lights it.

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vObjectPosition;
layout(location = 0) out vec4 outColor;

const vec3 kLightDirection = vec3(0.4, 0.8, 0.45);
const vec3 kAxisColors[3] = vec3[3](vec3(0.95, 0.45, 0.15), vec3(0.3, 0.85, 0.35), vec3(0.25, 0.5, 1.0));

void main() {
    const vec3 objectNormal = abs(normalize(cross(dFdx(vObjectPosition), dFdy(vObjectPosition))));
    const int axis = objectNormal.x > objectNormal.y ? (objectNormal.x > objectNormal.z ? 0 : 2)
                                                     : (objectNormal.y > objectNormal.z ? 1 : 2);
    const vec3 worldNormal = normalize(cross(dFdx(vWorldPosition), dFdy(vWorldPosition)));
    const float light = 0.35 + 0.65 * max(dot(worldNormal, normalize(kLightDirection)), 0.0);
    outColor = vec4(kAxisColors[axis] * light, 1.0);
}
