#version 450

// Solid-color 2D rect. No vertex buffer: the quad's four corners come
// from gl_VertexIndex, drawn as a triangle strip (see VulkanTarget.h's
// createGraphicsPipeline -- vertex input state is deliberately empty).
// Both rect and color arrive as push constants, so a frame's worth of
// rects is one bind plus one push+draw each, no descriptor sets.

layout(push_constant) uniform Push {
    vec4 rect;   // x, y, w, h -- already in NDC, converted CPU-side in recordFrame
    vec4 color;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 corners[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 pos = pc.rect.xy + corners[gl_VertexIndex] * pc.rect.zw;
    gl_Position = vec4(pos, 0.0, 1.0);
    outColor = pc.color;
}
