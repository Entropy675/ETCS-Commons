#version 450

// The source texture and the swapchain are both UNORM, so a layer's bytes
// are sampled as they are and written as they are -- the host raster's
// convention, and why a frame looks the same on the device as off it
// (OS/VulkanPresenter.h).
//
// Opacity multiplies alpha only: the pipeline blends source-over, so
// scaling alpha is what makes a layer translucent without darkening it.

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push {
    vec4  rect;
    float opacity;
} pc;

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outFragColor;

void main()
{
    vec4 c = texture(src, inUV);
    outFragColor = vec4(c.rgb, c.a * pc.opacity);
}
