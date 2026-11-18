#version 450

// One scene node. The vertex buffer is a Mesh's interleaved position and
// normal (RenderProvider/Mesh.h, DeviceMeshUpload); everything about the
// node arrives as push constants, so a scene is one bind per mesh and one
// push+draw per node, no descriptor sets -- the rect and blit pipelines'
// shape, carried into 3D. The projection hands over y up and depth in
// [0,1] (DeviceFrame.h); Vulkan's clip has y down, so the flip is here,
// once, and the WebGPU mirror has none.
//
// The model's three columns ride as vec4s: xyz the column (rotation times
// extent), and the three w's together the soft share -- the eye's forward
// times the node's glow or smoke strength (DeviceFrame.h, device_soft), all
// zero for a lit solid.
// 128 bytes, the push constant size every device has.

layout(push_constant) uniform Push {
    mat4 mvp;      // projection * view * model
    vec4 col0;     // model column 0, and forward.x * glow
    vec4 col1;     // model column 1, and forward.y * glow
    vec4 col2;     // model column 2, and forward.z * glow
    vec4 color;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;
layout(location = 2) flat out vec4 outGlow;   // forward, and the strength

void main()
{
    gl_Position   = pc.mvp * vec4(inPos, 1.0);
    gl_Position.y = -gl_Position.y;
    const vec3 g  = vec3(pc.col0.w, pc.col1.w, pc.col2.w);
    const float s = length(g);
    if (s > 0.0)
    {
        // A glow's edge is where the true surface turns away, so its normal
        // goes through the inverse transpose of rotation * extent: each
        // column over its own length squared (Scene3D::glowMesh).
        outNormal = normalize(pc.col0.xyz * (inNormal.x / dot(pc.col0.xyz, pc.col0.xyz))
                            + pc.col1.xyz * (inNormal.y / dot(pc.col1.xyz, pc.col1.xyz))
                            + pc.col2.xyz * (inNormal.z / dot(pc.col2.xyz, pc.col2.xyz)));
        outGlow   = vec4(g / s, s);
    }
    else
    {
        outNormal = normalize(mat3(pc.col0.xyz, pc.col1.xyz, pc.col2.xyz) * inNormal);
        outGlow   = vec4(0.0);
    }
    outColor = pc.color;
}
