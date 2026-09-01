#version 450

// Textured 2D quad for Surface.Blit. Same no-vertex-buffer trick as
// rect.vert -- four corners from gl_VertexIndex as a triangle strip --
// with the corner doubling as the UV, since the quad is axis-aligned and
// the source is drawn unrotated.

layout(push_constant) uniform Push {
    vec4  rect;     // x, y, w, h -- already in NDC, converted CPU-side in recordFrame
    float opacity;
} pc;

layout(location = 0) out vec2 outUV;

void main()
{
    vec2 corners[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
    vec2 c = corners[gl_VertexIndex];
    gl_Position = vec4(pc.rect.xy + c * pc.rect.zw, 0.0, 1.0);
    outUV = c;
}
