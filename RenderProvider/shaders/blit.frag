#version 450

// The source texture is R8G8B8A8_SRGB, so this sample is already decoded
// to linear and the sRGB swapchain re-encodes on write -- the round trip
// is why a layer's bytes reach the display unchanged (see ensureTexture's
// own comment in OS/VulkanSurface.h).
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
