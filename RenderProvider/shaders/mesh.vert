#version 450

// One scene node. The vertex buffer is a Mesh's interleaved position and
// normal (RenderProvider/Mesh.h, DeviceMeshUpload); everything about the
// node arrives as push constants, so a scene is one bind per mesh and one
// push+draw per node, no descriptor sets -- the rect and blit pipelines'
// shape, carried into 3D. The projection hands over y up and depth in
// [0,1] (DeviceFrame.h); Vulkan's clip has y down, so the flip is here,
// once, and the WebGPU mirror has none.

layout(push_constant) uniform Push {
    mat4 mvp;      // projection * view * model
    mat3 rot;      // the model's rotation alone, for the normal (std430 pads it to 12 floats)
    vec4 color;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;

void main()
{
    gl_Position   = pc.mvp * vec4(inPos, 1.0);
    gl_Position.y = -gl_Position.y;
    outNormal     = normalize(pc.rot * inNormal);
    outColor      = pc.color;
}
