#ifndef VULKAN_TARGET_H__
#define VULKAN_TARGET_H__

#include "../../../ontology.h"
#include "VulkanInstance.h"
#include "VulkanSurface.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

static constexpr uint32_t RENDERTARGET_FRAMES_IN_FLIGHT = 2;

// VulkanTarget -- TargetBase<VulkanTarget> + DeletableBase<VulkanTarget>.
// Owns the surface/swapchain/render-pass/framebuffers/one fixed
// push-constant pipeline for solid-color 2D rects, and per-frame
// command buffers + sync objects. Spawned as a CHILD of an existing
// WindowProvider::Window (make_typed_child) -- Create() reaches its
// parent via getInterfacePointer("Window")/("Resizable") for the native
// surface handle and initial size, and subscribes to the parent's
// Resizable to recreate the swapchain on resize.
class VulkanTarget : public TargetBase<VulkanTarget>, public DeletableBase<VulkanTarget>
{
public:
    WIRE_TYPE_IDENTITY(VulkanTarget);

    VulkanTarget()  = default;
    ~VulkanTarget() { teardown(); }

    // instance: RID of an already-Create()'d VulkanInstance. shader_dir:
    // cwd-relative directory holding rect.vert.spv/rect.frag.spv (see
    // RenderProvider.h's own comment on why this isn't self-locating yet).
    bool Create(VulkanInstance* instance, const std::string& shader_dir)
    {
        if (m_swapchain) return true; // idempotent, same convention as Window::CreateWindowConcrete
        m_instance = instance;
        if (!m_instance)
        {
            ETCS_LOG("VulkanTarget", "Create called with no VulkanInstance.");
            return false;
        }

        ETCS::Entity* parent = this->getParent();
        if (!parent)
        {
            ETCS_LOG("VulkanTarget", "Create called with no parent -- must be spawned as a child of a Window.");
            return false;
        }
        void* winRaw = parent->getInterfacePointer(ETCS::Buffer("Window"));
        Window_* win = static_cast<Window_*>(winRaw);
        if (!win)
        {
            ETCS_LOG("VulkanTarget", "Parent has no Window interface pointer.");
            return false;
        }
        void* resizableRaw = parent->getInterfacePointer(ETCS::Buffer("Resizable"));
        m_parentResizable = static_cast<Resizable_*>(resizableRaw);
        if (!m_parentResizable)
        {
            ETCS_LOG("VulkanTarget", "Parent has no Resizable interface pointer.");
            return false;
        }

        m_surface = CreateSurfaceFromNativeHandle(m_instance->GetInstance(), win->GetNativeSurfaceHandle());
        if (m_surface == VK_NULL_HANDLE) return false;

        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_instance->GetPhysicalDevice(), m_instance->GetQueueFamily(),
                                              m_surface, &presentSupported);
        if (!presentSupported)
        {
            ETCS_LOG("VulkanTarget", "Selected queue family cannot present to this surface.");
            teardown();
            return false;
        }

        if (!createShaderPipelineIndependentState()) { teardown(); return false; }
        if (!loadPipeline(shader_dir)) { teardown(); return false; }
        if (!createSyncObjects()) { teardown(); return false; }

        // OnResize fires immediately on registration with the CURRENT size
        // (Resizable_::OnResize's existing semantics) -- this doubles as
        // the initial swapchain build, so no separate first-build call.
        m_parentResizable->OnResize([this](WindowSize sz) { this->recreateSwapchain(sz); }, 0);

        this->addTag("active");
        return m_swapchain != VK_NULL_HANDLE;
    }

    // --- Target_ dispatch (TargetBase.h) ---

    void ClearConcrete(float r, float g, float b, float a) override
    {
        m_clearColor = { r, g, b, a };
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) override
    {
        m_pendingRects.push_back({ x, y, w, h, r, g, b, a });
    }

    void PresentConcrete() override
    {
        if (!m_swapchain) return;
        if (m_extent.width == 0 || m_extent.height == 0) { m_pendingRects.clear(); return; } // minimized

        VkFence frameFence = m_inFlight[m_currentFrame];
        vkWaitForFences(m_instance->GetDevice(), 1, &frameFence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(m_instance->GetDevice(), m_swapchain, UINT64_MAX,
                                                  m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapchain({ m_extent.width, m_extent.height });
            m_pendingRects.clear();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        {
            ETCS_LOG("VulkanTarget", "vkAcquireNextImageKHR failed: " << acquire);
            m_pendingRects.clear();
            return;
        }

        vkResetFences(m_instance->GetDevice(), 1, &frameFence);

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);
        recordFrame(cmd, imageIndex);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &m_imageAvailable[m_currentFrame];
        submit.pWaitDstStageMask    = &waitStage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &m_renderFinished[m_currentFrame];
        vkQueueSubmit(m_instance->GetQueue(), 1, &submit, frameFence);

        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &m_renderFinished[m_currentFrame];
        present.swapchainCount     = 1;
        present.pSwapchains        = &m_swapchain;
        present.pImageIndices      = &imageIndex;
        VkResult presentResult = vkQueuePresentKHR(m_instance->GetQueue(), &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            recreateSwapchain({ m_extent.width, m_extent.height });

        m_pendingRects.clear();
        m_currentFrame = (m_currentFrame + 1) % RENDERTARGET_FRAMES_IN_FLIGHT;
    }

    // --- Resizable_ dispatch (ResizableBase, composed into TargetBase) ---

    WindowSize GetSizeConcrete() override
    {
        return { m_extent.width, m_extent.height };
    }

    // --- Deletable_ dispatch ---

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("VulkanTarget", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // Manual-verification convenience ONLY -- not a primitive downstream
    // modules should call. .etcs scripts have no loop construct today
    // (see RenderProvider.h's own comment), so a per-frame Clear/DrawRect/
    // Present cycle has to happen inside one work func for now.
    void RunDemo(ETCS::Entity* windowEntity, uint32_t frameCount)
    {
        Window_* win = static_cast<Window_*>(windowEntity->getInterfacePointer(ETCS::Buffer("Window")));
        if (!win) { ETCS_LOG("VulkanTarget", "RunDemo: parent has no Window interface pointer."); return; }

        for (uint32_t i = 0; i < frameCount && !win->ShouldClose(); ++i)
        {
            win->PollEvents();
            ClearConcrete(0.05f, 0.05f, 0.08f, 1.0f);
            DrawRectConcrete(40, 40, 200, 120, 0.85f, 0.2f, 0.2f, 1.0f);
            DrawRectConcrete(260, 160, 150, 150, 0.2f, 0.7f, 0.3f, 1.0f);
            PresentConcrete();
        }
    }

private:
    struct PendingRect { int32_t x, y; uint32_t w, h; float r, g, b, a; };
    struct RectPushConstants { float rect[4]; float color[4]; };

    VulkanInstance* m_instance         = nullptr;
    Resizable_*     m_parentResizable  = nullptr;

    VkSurfaceKHR   m_surface   = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat       m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D     m_extent    = { 0, 0 };
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    std::array<VkCommandBuffer, RENDERTARGET_FRAMES_IN_FLIGHT> m_commandBuffers{};
    std::array<VkSemaphore, RENDERTARGET_FRAMES_IN_FLIGHT>     m_imageAvailable{};
    std::array<VkSemaphore, RENDERTARGET_FRAMES_IN_FLIGHT>     m_renderFinished{};
    std::array<VkFence, RENDERTARGET_FRAMES_IN_FLIGHT>         m_inFlight{};
    uint32_t m_currentFrame = 0;

    std::array<float, 4>      m_clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    std::vector<PendingRect>  m_pendingRects;

    // --- setup helpers ---

    // Command pool comes from VulkanInstance -- allocated once here.
    bool createShaderPipelineIndependentState()
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = m_instance->GetCommandPool();
        alloc.level               = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = RENDERTARGET_FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(m_instance->GetDevice(), &alloc, m_commandBuffers.data()) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkAllocateCommandBuffers failed.");
            return false;
        }
        return true;
    }

    bool createSyncObjects()
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < RENDERTARGET_FRAMES_IN_FLIGHT; ++i)
        {
            if (vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS ||
                vkCreateFence(m_instance->GetDevice(), &fenceInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanTarget", "Sync object creation failed.");
                return false;
            }
        }
        return true;
    }

    static std::vector<uint32_t> readSpirv(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize size = f.tellg();
        if (size <= 0 || (size % 4) != 0) return {};
        f.seekg(0);
        std::vector<uint32_t> buf(static_cast<size_t>(size) / 4);
        if (!f.read(reinterpret_cast<char*>(buf.data()), size)) return {};
        return buf;
    }

    VkShaderModule loadShaderModule(const std::string& path)
    {
        std::vector<uint32_t> code = readSpirv(path);
        if (code.empty())
        {
            ETCS_LOG("VulkanTarget", "Failed to read SPIR-V: " << path);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode    = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_instance->GetDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkCreateShaderModule failed for " << path);
            return VK_NULL_HANDLE;
        }
        return mod;
    }

    // Loads the fixed rect pipeline's shaders and pipeline layout -- the
    // pipeline OBJECT itself is built (and rebuilt) in
    // buildSwapchainDependentPipeline, since it's tied to the render
    // pass, which is tied to the swapchain's format/extent.
    bool loadPipeline(const std::string& shader_dir)
    {
        std::string dir = shader_dir.empty() ? "shaders/" : shader_dir;
        if (dir.back() != '/') dir += '/';
        m_vertShader = loadShaderModule(dir + "rect.vert.spv");
        m_fragShader = loadShaderModule(dir + "rect.frag.spv");
        if (!m_vertShader || !m_fragShader) return false;

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset     = 0;
        pushRange.size       = sizeof(RectPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;
        if (vkCreatePipelineLayout(m_instance->GetDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkCreatePipelineLayout failed.");
            return false;
        }
        return true;
    }

    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;

    bool chooseSurfaceFormat(VkSurfaceFormatKHR& out)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, formats.data());
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { out = f; return true; }
        out = formats[0];
        return true;
    }

    // Tears down and rebuilds everything downstream of the swapchain
    // (images/views/render pass/framebuffers/pipeline) -- called on
    // initial Create (via the immediate OnResize invocation) and on every
    // real resize thereafter (ontology/Resizable.h's notifyResize fix is
    // what makes the second case actually fire).
    void recreateSwapchain(WindowSize sz)
    {
        if (!m_instance || m_surface == VK_NULL_HANDLE) return;
        if (sz.width == 0 || sz.height == 0) { m_extent = { 0, 0 }; return; } // minimized -- wait for next real resize

        vkDeviceWaitIdle(m_instance->GetDevice());
        destroySwapchainDependents();

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_instance->GetPhysicalDevice(), m_surface, &caps);

        VkSurfaceFormatKHR surfaceFormat{};
        if (!chooseSurfaceFormat(surfaceFormat)) return;
        m_format = surfaceFormat.format;

        VkExtent2D extent;
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent = caps.currentExtent;
        }
        else
        {
            extent.width  = std::clamp(sz.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
            extent.height = std::clamp(sz.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        m_extent = extent;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{};
        ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface          = m_surface;
        ci.minImageCount    = imageCount;
        ci.imageFormat      = surfaceFormat.format;
        ci.imageColorSpace  = surfaceFormat.colorSpace;
        ci.imageExtent      = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR; // universally supported, vsync'd -- good enough for V1
        ci.clipped          = VK_TRUE;

        if (vkCreateSwapchainKHR(m_instance->GetDevice(), &ci, nullptr, &m_swapchain) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkCreateSwapchainKHR failed.");
            m_swapchain = VK_NULL_HANDLE;
            return;
        }

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(m_instance->GetDevice(), m_swapchain, &actualCount, nullptr);
        m_images.resize(actualCount);
        vkGetSwapchainImagesKHR(m_instance->GetDevice(), m_swapchain, &actualCount, m_images.data());

        m_imageViews.resize(actualCount);
        for (uint32_t i = 0; i < actualCount; ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image    = m_images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format   = m_format;
            viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_instance->GetDevice(), &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanTarget", "vkCreateImageView failed for swapchain image " << i);
                return;
            }
        }

        if (!createRenderPass()) return;
        if (!createFramebuffers()) return;
        if (!createGraphicsPipeline()) return;

        // Re-fire on THIS target's own Resizable interface, so anything
        // subscribed to the target (rather than to the window) -- a future
        // PintaProvider canvas, say -- hears the new size too.
        this->notifyResize({ m_extent.width, m_extent.height });
    }

    bool createRenderPass()
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format         = m_format;
        colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments    = &colorAttachment;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;

        if (vkCreateRenderPass(m_instance->GetDevice(), &ci, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkCreateRenderPass failed.");
            return false;
        }
        return true;
    }

    bool createFramebuffers()
    {
        m_framebuffers.resize(m_imageViews.size());
        for (size_t i = 0; i < m_imageViews.size(); ++i)
        {
            VkImageView attachments[] = { m_imageViews[i] };
            VkFramebufferCreateInfo ci{};
            ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            ci.renderPass      = m_renderPass;
            ci.attachmentCount = 1;
            ci.pAttachments    = attachments;
            ci.width           = m_extent.width;
            ci.height          = m_extent.height;
            ci.layers          = 1;
            if (vkCreateFramebuffer(m_instance->GetDevice(), &ci, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanTarget", "vkCreateFramebuffer failed for image " << i);
                return false;
            }
        }
        return true;
    }

    bool createGraphicsPipeline()
    {
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = m_vertShader;
        vertStage.pName  = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = m_fragShader;
        fragStage.pName  = "main";

        VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

        // No vertex buffers -- the quad's 4 corners come from gl_VertexIndex
        // in rect.vert (see shaders/rect.vert), so vertex input is empty.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, m_extent };
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports    = &viewport;
        viewportState.scissorCount  = 1;
        viewportState.pScissors     = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo msaa{};
        msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable    = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &blendAttachment;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertexInput;
        ci.pInputAssemblyState = &inputAssembly;
        ci.pViewportState      = &viewportState;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState   = &msaa;
        ci.pColorBlendState    = &blend;
        ci.layout              = m_pipelineLayout;
        ci.renderPass           = m_renderPass;
        ci.subpass              = 0;

        if (vkCreateGraphicsPipelines(m_instance->GetDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanTarget", "vkCreateGraphicsPipelines failed.");
            return false;
        }
        return true;
    }

    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin);

        VkClearValue clear{};
        clear.color = { { m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3] } };

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = m_renderPass;
        rpBegin.framebuffer       = m_framebuffers[imageIndex];
        rpBegin.renderArea.extent = m_extent;
        rpBegin.clearValueCount   = 1;
        rpBegin.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        if (!m_pendingRects.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            float fw = static_cast<float>(m_extent.width);
            float fh = static_cast<float>(m_extent.height);
            for (const PendingRect& r : m_pendingRects)
            {
                RectPushConstants pc{};
                pc.rect[0] = (static_cast<float>(r.x) / fw) * 2.0f - 1.0f;
                pc.rect[1] = (static_cast<float>(r.y) / fh) * 2.0f - 1.0f;
                pc.rect[2] = (static_cast<float>(r.w) / fw) * 2.0f;
                pc.rect[3] = (static_cast<float>(r.h) / fh) * 2.0f;
                pc.color[0] = r.r; pc.color[1] = r.g; pc.color[2] = r.b; pc.color[3] = r.a;
                vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 4, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    void destroySwapchainDependents()
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (!dev) return;
        if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline = VK_NULL_HANDLE; }
        for (auto fb : m_framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
        m_framebuffers.clear();
        if (m_renderPass)     { vkDestroyRenderPass(dev, m_renderPass, nullptr);         m_renderPass = VK_NULL_HANDLE; }
        for (auto view : m_imageViews) vkDestroyImageView(dev, view, nullptr);
        m_imageViews.clear();
        m_images.clear();
        if (m_swapchain)      { vkDestroySwapchainKHR(dev, m_swapchain, nullptr);        m_swapchain = VK_NULL_HANDLE; }
    }

    void teardown()
    {
        if (m_instance && m_instance->GetDevice()) vkDeviceWaitIdle(m_instance->GetDevice());
        destroySwapchainDependents();
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (dev)
        {
            if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
            if (m_vertShader)     { vkDestroyShaderModule(dev, m_vertShader, nullptr);        m_vertShader = VK_NULL_HANDLE; }
            if (m_fragShader)     { vkDestroyShaderModule(dev, m_fragShader, nullptr);        m_fragShader = VK_NULL_HANDLE; }
            for (uint32_t i = 0; i < RENDERTARGET_FRAMES_IN_FLIGHT; ++i)
            {
                if (m_imageAvailable[i]) vkDestroySemaphore(dev, m_imageAvailable[i], nullptr);
                if (m_renderFinished[i]) vkDestroySemaphore(dev, m_renderFinished[i], nullptr);
                if (m_inFlight[i])       vkDestroyFence(dev, m_inFlight[i], nullptr);
            }
        }
        m_imageAvailable.fill(VK_NULL_HANDLE);
        m_renderFinished.fill(VK_NULL_HANDLE);
        m_inFlight.fill(VK_NULL_HANDLE);
        if (m_instance && m_surface) { vkDestroySurfaceKHR(m_instance->GetInstance(), m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
        this->removeTag("active");
    }
};

#endif
