#version 450

// Lambert, one fixed light in world space, the same lamp the host
// rasteriser implies when it shades a box's faces -- so a node reads the
// same on both paths, and the mesh's own normals decide the shading.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 outFragColor;

void main()
{
    const vec3 light = normalize(vec3(0.4, 0.8, 0.45));
    float lit = 0.45 + 0.55 * max(dot(normalize(inNormal), light), 0.0);
    outFragColor = vec4(inColor.rgb * lit, inColor.a);
}
