#ifndef RENDERPROVIDER_VULKANPRESENTER_H__
#define RENDERPROVIDER_VULKANPRESENTER_H__

#include "../../../ontology.h"
#include "DeviceFrame.h"
#include "VulkanInstance.h"
#include "VulkanPlatform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <dlfcn.h>

static constexpr uint32_t VK_PRESENTER_FRAMES_IN_FLIGHT = 2;
// Distinct blit sources one window keeps textures for -- a layer stack, in
// practice. Past it the least recently drawn one is evicted, not the blit
// dropped.
static constexpr uint32_t VK_PRESENTER_MAX_SOURCES = 64;

/*
 * VulkanPresenter -- a window's frames, drawn and presented by a Vulkan
 * device. The device half of the native window surface (HostSurface.h),
 * created when a Device child naming a VulkanInstance is ready and dropped
 * when it goes; the host raster is the floor it falls back to.
 *
 * What used to be VulkanSurface, minus everything that made it an entity:
 * the swapchain, two fixed pipelines (solid rects by push constant, textured
 * quads for blits), per-frame command buffers and sync, and textures keyed
 * by the RID of the source they mirror. Every call is on the frame thread --
 * Present is the only way in -- so nothing here is locked.
 *
 * UNORM, NOT SRGB, end to end -- swapchain and textures both -- so a byte
 * reaches the display unchanged and a float colour means the same byte it
 * means on the host raster (Pixels_::toByte is a plain linear scale). The
 * old sRGB swapchain encoded device rects on the way out, so the same frame
 * came out brighter on the GPU than on the CPU; with a switch between the
 * two at runtime, that difference would be a visible jump.
 */
class VulkanPresenter : public DevicePresenter
{
public:
    VulkanPresenter(VulkanInstance* instance, const NativeSurfaceHandle& native)
        : m_instance(instance), m_native(native) {}
    ~VulkanPresenter() override { teardown(); }

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    bool Create(uint32_t w, uint32_t h, const std::string& shader_dir)
    {
        if (!m_instance || !m_instance->GetDevice())
        {
            ETCS_LOG("VulkanPresenter", "the Instance has no device -- Create it first.");
            return false;
        }
        m_surface = CreateSurfaceFromNativeHandle(m_instance->GetInstance(), m_native);
        if (m_surface == VK_NULL_HANDLE) return false;

        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_instance->GetPhysicalDevice(), m_instance->GetQueueFamily(),
                                             m_surface, &presentSupported);
        if (!presentSupported)
        {
            ETCS_LOG("VulkanPresenter", "the device's queue family cannot present to this window.");
            teardown();
            return false;
        }
        if (!allocateCommandBuffers() || !loadPipeline(shader_dir) || !createSyncObjects())
        {
            teardown();
            return false;
        }
        recreateSwapchain(w, h);
        if (m_swapchain == VK_NULL_HANDLE) { teardown(); return false; }
        return true;
    }

    uint64_t DeviceKey() const override
    { return m_instance ? reinterpret_cast<uint64_t>(m_instance->GetDevice()) : 0; }

    bool Has(ETCS::RID source, uint32_t w, uint32_t h) const override
    {
        auto it = m_textures.find(source);
        return it != m_textures.end() && it->second.everUploaded && it->second.w == w && it->second.h == h;
    }

    bool Present(DeviceFrame& frame) override
    {
        if (m_lost) return false;
        if (frame.width == 0 || frame.height == 0) return true;          // minimised
        if (frame.width != m_want_w || frame.height != m_want_h || m_swapchain == VK_NULL_HANDLE)
        {
            recreateSwapchain(frame.width, frame.height);
            if (m_swapchain == VK_NULL_HANDLE) return !m_lost;
        }
        if (m_extent.width == 0 || m_extent.height == 0) return true;

        ++m_frameNo;
        std::unordered_set<ETCS::RID> used;
        for (const DeviceOp& op : frame.ops)
            if (op.kind == DeviceOp::Kind::Blit) used.insert(op.source);

        VkDevice dev = m_instance->GetDevice();
        std::vector<UploadJob> jobs;
        if (!frame.uploads.empty())
        {
            // Staging is reused per source, and the other frame in flight may
            // still be copying out of it: wait for both before writing.
            vkWaitForFences(dev, VK_PRESENTER_FRAMES_IN_FLIGHT, m_inFlight.data(), VK_TRUE, UINT64_MAX);
            for (const DeviceUpload& up : frame.uploads)
            {
                if (up.w == 0 || up.h == 0 || up.bytes.size() < static_cast<size_t>(up.w) * up.h * 4) continue;
                Texture* tex = ensureTexture(up.source, up.w, up.h, used);
                if (!tex) continue;
                std::memcpy(tex->stagingMapped, up.bytes.data(), static_cast<size_t>(up.w) * up.h * 4);
                jobs.push_back({ tex->image, tex->staging, up.w, up.h, !tex->everUploaded });
                tex->everUploaded = true;
            }
        }

        VkFence fence = m_inFlight[m_currentFrame];
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(dev, m_swapchain, UINT64_MAX,
                                                 m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapchain(m_want_w, m_want_h);
            // The uploads above were not recorded; forget them so the
            // surface sends the bytes again rather than drawing stale ones.
            for (const UploadJob& j : jobs) forgetImage(j.image);
            return true;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        {
            // SURFACE_LOST cannot be rebuilt, and every later acquire fails
            // the same way: finished, and the window goes back to the host.
            if (acquire == VK_ERROR_SURFACE_LOST_KHR || acquire == VK_ERROR_DEVICE_LOST)
            {
                ETCS_LOG("VulkanPresenter", "acquire: " << (acquire == VK_ERROR_DEVICE_LOST
                         ? "DEVICE LOST" : "SURFACE LOST") << " -- this window goes back to the host.");
                m_lost = true;
                return false;
            }
            ETCS_LOG("VulkanPresenter", "vkAcquireNextImageKHR failed: " << acquire << " -- dropping this frame.");
            for (const UploadJob& j : jobs) forgetImage(j.image);
            return true;
        }
        vkResetFences(dev, 1, &fence);

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);
        record(cmd, imageIndex, frame, jobs);

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
        if (vkQueueSubmit(m_instance->GetQueue(), 1, &submit, fence) == VK_ERROR_DEVICE_LOST)
        {
            ETCS_LOG("VulkanPresenter", "submit: DEVICE LOST -- this window goes back to the host.");
            m_lost = true;
            return false;
        }

        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &m_renderFinished[m_currentFrame];
        present.swapchainCount     = 1;
        present.pSwapchains        = &m_swapchain;
        present.pImageIndices      = &imageIndex;
        VkResult pr = vkQueuePresentKHR(m_instance->GetQueue(), &present);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
            recreateSwapchain(m_want_w, m_want_h);
        else if (pr == VK_ERROR_SURFACE_LOST_KHR || pr == VK_ERROR_DEVICE_LOST)
        {
            ETCS_LOG("VulkanPresenter", "present: surface or device lost -- this window goes back to the host.");
            m_lost = true;
            return false;
        }

        m_currentFrame = (m_currentFrame + 1) % VK_PRESENTER_FRAMES_IN_FLIGHT;
        return true;
    }

private:
    struct Texture
    {
        VkImage         image         = VK_NULL_HANDLE;
        VkDeviceMemory  memory        = VK_NULL_HANDLE;
        VkImageView     view          = VK_NULL_HANDLE;
        VkBuffer        staging       = VK_NULL_HANDLE;
        VkDeviceMemory  stagingMem    = VK_NULL_HANDLE;
        void*           stagingMapped = nullptr;       // persistently mapped
        VkDescriptorSet set           = VK_NULL_HANDLE;
        uint32_t        w = 0, h = 0;
        bool            everUploaded  = false;
        uint64_t        lastUsed      = 0;
    };
    struct UploadJob
    {
        VkImage  image;
        VkBuffer staging;
        uint32_t w, h;
        bool     firstUpload;   // UNDEFINED vs SHADER_READ_ONLY as the old layout
    };
    struct RectPush { float rect[4]; float color[4]; };
    struct BlitPush { float rect[4]; float opacity; float _pad[3]; };

    // A texture whose upload was never recorded holds nothing: dropped, so
    // Has() says no and the surface sends the bytes again.
    void forgetImage(VkImage image)
    {
        for (auto it = m_textures.begin(); it != m_textures.end(); ++it)
            if (it->second.image == image && !it->second.everUploaded) { destroyTexture(it->second); m_textures.erase(it); return; }
        for (auto& [rid, t] : m_textures) { (void)rid; if (t.image == image) t.everUploaded = false; }
    }

    // --- setup ---

    bool allocateCommandBuffers()
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = m_instance->GetCommandPool();
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = VK_PRESENTER_FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(m_instance->GetDevice(), &alloc, m_commandBuffers.data()) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkAllocateCommandBuffers failed.");
            return false;
        }
        m_haveCommandBuffers = true;
        return true;
    }

    bool createSyncObjects()
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < VK_PRESENTER_FRAMES_IN_FLIGHT; ++i)
        {
            if (vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS ||
                vkCreateFence(m_instance->GetDevice(), &fenceInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanPresenter", "sync object creation failed.");
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
            ETCS_LOG("VulkanPresenter", "failed to read SPIR-V: " << path);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode    = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_instance->GetDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateShaderModule failed for " << path);
            return VK_NULL_HANDLE;
        }
        return mod;
    }

    /*
     * WHERE THE SHADERS ARE, decided by the module rather than the caller's
     * working directory: a module's shaders ship with it, dladdr gives this
     * .so's own path, and the caller's argument stays a first-choice override.
     * A total miss lists every path tried -- a window that never draws was the
     * symptom of the old cwd-relative lookup.
     */
    static bool shader_dir_has_pipeline(const std::string& dir)
    {
        std::ifstream probe(dir + "rect.vert.spv", std::ios::binary);
        return probe.good();
    }

    static std::string own_module_dir()
    {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&VulkanPresenter::own_module_dir), &info) == 0
            || info.dli_fname == nullptr)
            return "";
        std::string path = info.dli_fname;
        const size_t slash = path.find_last_of('/');
        return (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    }

    static std::string resolve_shader_dir(const std::string& requested)
    {
        std::vector<std::string> tried;
        auto consider = [&tried](std::string dir) -> std::string
        {
            if (dir.empty()) return "";
            if (dir.back() != '/') dir += '/';
            tried.push_back(dir);
            return shader_dir_has_pipeline(dir) ? dir : std::string();
        };
        const std::string so_dir = own_module_dir();
        for (const std::string& candidate : {
                 requested,
                 so_dir + "shaders",
                 so_dir + "../modules/RenderProvider/shaders",
                 std::string("modules/RenderProvider/shaders"),
                 std::string("../modules/RenderProvider/shaders") })
        {
            const std::string hit = consider(candidate);
            if (!hit.empty()) { ETCS_LOG("VulkanPresenter", "shaders: " << hit); return hit; }
        }
        std::string all;
        for (const std::string& t : tried) { all += "\n    "; all += t; }
        ETCS_LOG("VulkanPresenter", "no SPIR-V found. Tried:" << all
                 << "\n  This window stays on the host.");
        return "";
    }

    bool loadPipeline(const std::string& shader_dir)
    {
        const std::string dir = resolve_shader_dir(shader_dir);
        if (dir.empty()) return false;
        VkDevice dev = m_instance->GetDevice();
        m_vertShader     = loadShaderModule(dir + "rect.vert.spv");
        m_fragShader     = loadShaderModule(dir + "rect.frag.spv");
        m_blitVertShader = loadShaderModule(dir + "blit.vert.spv");
        m_blitFragShader = loadShaderModule(dir + "blit.frag.spv");
        if (!m_vertShader || !m_fragShader || !m_blitVertShader || !m_blitFragShader) return false;

        VkPushConstantRange rectPush{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RectPush) };
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &rectPush;
        if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreatePipelineLayout failed.");
            return false;
        }

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(dev, &setLayoutInfo, nullptr, &m_blitSetLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateDescriptorSetLayout failed.");
            return false;
        }

        // NEAREST: a pixel editor's canvas shows the pixels being edited.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_NEAREST;
        samplerInfo.minFilter    = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(dev, &samplerInfo, nullptr, &m_blitSampler) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateSampler failed.");
            return false;
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_PRESENTER_MAX_SOURCES };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = VK_PRESENTER_MAX_SOURCES;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_blitDescriptorPool) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateDescriptorPool failed.");
            return false;
        }

        VkPushConstantRange blitPush{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitPush) };
        VkPipelineLayoutCreateInfo blitLayoutInfo{};
        blitLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        blitLayoutInfo.setLayoutCount         = 1;
        blitLayoutInfo.pSetLayouts            = &m_blitSetLayout;
        blitLayoutInfo.pushConstantRangeCount = 1;
        blitLayoutInfo.pPushConstantRanges    = &blitPush;
        if (vkCreatePipelineLayout(dev, &blitLayoutInfo, nullptr, &m_blitPipelineLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreatePipelineLayout (blit) failed.");
            return false;
        }
        return true;
    }

    // --- textures ---

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer& outBuf, VkDeviceMemory& outMem)
    {
        VkDevice dev = m_instance->GetDevice();
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = size;
        ci.usage       = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &ci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, outBuf, &req);
        const uint32_t typeIndex = m_instance->FindMemoryType(req.memoryTypeBits, props);
        if (typeIndex == UINT32_MAX) return false;
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(dev, &alloc, nullptr, &outMem) != VK_SUCCESS) return false;
        vkBindBufferMemory(dev, outBuf, outMem, 0);
        return true;
    }

    /*
     * The texture mirroring one source at one size, made or remade here.
     * Full, the least recently drawn source not in this frame is evicted --
     * after waiting for the device, since a frame in flight may sample it.
     */
    Texture* ensureTexture(ETCS::RID rid, uint32_t w, uint32_t h, const std::unordered_set<ETCS::RID>& used)
    {
        VkDevice dev = m_instance->GetDevice();
        auto it = m_textures.find(rid);
        if (it != m_textures.end())
        {
            if (it->second.w == w && it->second.h == h) { it->second.lastUsed = m_frameNo; return &it->second; }
            vkDeviceWaitIdle(dev);
            destroyTexture(it->second);
            m_textures.erase(it);
        }
        if (m_textures.size() >= VK_PRESENTER_MAX_SOURCES)
        {
            auto victim = m_textures.end();
            for (auto v = m_textures.begin(); v != m_textures.end(); ++v)
                if (!used.count(v->first) && (victim == m_textures.end() || v->second.lastUsed < victim->second.lastUsed))
                    victim = v;
            if (victim == m_textures.end())
            {
                ETCS_LOG("VulkanPresenter", "more than " << VK_PRESENTER_MAX_SOURCES
                         << " sources in one frame -- dropping RID:" << rid);
                return nullptr;
            }
            vkDeviceWaitIdle(dev);
            destroyTexture(victim->second);
            m_textures.erase(victim);
        }

        Texture tex{};
        tex.w = w; tex.h = h; tex.lastUsed = m_frameNo;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = { w, h, 1 };
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &imageInfo, nullptr, &tex.image) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateImage failed for RID:" << rid);
            destroyTexture(tex);
            return nullptr;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, tex.image, &req);
        const uint32_t typeIndex = m_instance->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = typeIndex;
        if (typeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &tex.memory) != VK_SUCCESS)
        {
            destroyTexture(tex);
            return nullptr;
        }
        vkBindImageMemory(dev, tex.image, tex.memory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = tex.image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &viewInfo, nullptr, &tex.view) != VK_SUCCESS)
        {
            destroyTexture(tex);
            return nullptr;
        }
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          tex.staging, tex.stagingMem))
        {
            ETCS_LOG("VulkanPresenter", "staging allocation failed for RID:" << rid);
            destroyTexture(tex);
            return nullptr;
        }
        vkMapMemory(dev, tex.stagingMem, 0, bytes, 0, &tex.stagingMapped);

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool     = m_blitDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts        = &m_blitSetLayout;
        if (vkAllocateDescriptorSets(dev, &setAlloc, &tex.set) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkAllocateDescriptorSets failed for RID:" << rid);
            destroyTexture(tex);
            return nullptr;
        }
        VkDescriptorImageInfo imgInfo{ m_blitSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = tex.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

        return &(m_textures[rid] = tex);
    }

    void destroyTexture(Texture& tex)
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (!dev) return;
        if (tex.set)           { vkFreeDescriptorSets(dev, m_blitDescriptorPool, 1, &tex.set); tex.set = VK_NULL_HANDLE; }
        if (tex.stagingMapped) { vkUnmapMemory(dev, tex.stagingMem); tex.stagingMapped = nullptr; }
        if (tex.staging)       { vkDestroyBuffer(dev, tex.staging, nullptr);  tex.staging    = VK_NULL_HANDLE; }
        if (tex.stagingMem)    { vkFreeMemory(dev, tex.stagingMem, nullptr);  tex.stagingMem = VK_NULL_HANDLE; }
        if (tex.view)          { vkDestroyImageView(dev, tex.view, nullptr);  tex.view       = VK_NULL_HANDLE; }
        if (tex.image)         { vkDestroyImage(dev, tex.image, nullptr);     tex.image      = VK_NULL_HANDLE; }
        if (tex.memory)        { vkFreeMemory(dev, tex.memory, nullptr);      tex.memory     = VK_NULL_HANDLE; }
    }

    // Transition + copy, before the render pass (a copy inside one is illegal).
    void recordUploads(VkCommandBuffer cmd, const std::vector<UploadJob>& jobs)
    {
        for (const UploadJob& job : jobs)
        {
            VkImageMemoryBarrier toDst{};
            toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toDst.oldLayout           = job.firstUpload ? VK_IMAGE_LAYOUT_UNDEFINED
                                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image               = job.image;
            toDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toDst.srcAccessMask       = job.firstUpload ? 0 : VK_ACCESS_SHADER_READ_BIT;
            toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                                 job.firstUpload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                 : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { job.w, job.h, 1 };
            vkCmdCopyBufferToImage(cmd, job.staging, job.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            VkImageMemoryBarrier toRead = toDst;
            toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toRead);
        }
    }

    // --- swapchain ---

    // UNORM first: see the class comment. Anything else only if the window
    // offers no 8-bit UNORM at all.
    bool chooseSurfaceFormat(VkSurfaceFormatKHR& out)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, formats.data());
        for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
            for (const auto& f : formats)
                if (f.format == want) { out = f; return true; }
        out = formats[0];
        return true;
    }

    void recreateSwapchain(uint32_t w, uint32_t h)
    {
        m_want_w = w; m_want_h = h;
        if (!m_instance || m_surface == VK_NULL_HANDLE) return;
        if (w == 0 || h == 0) { m_extent = { 0, 0 }; return; }

        VkDevice dev = m_instance->GetDevice();
        vkDeviceWaitIdle(dev);
        destroySwapchainDependents();

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_instance->GetPhysicalDevice(), m_surface, &caps) == VK_ERROR_SURFACE_LOST_KHR)
        {
            m_lost = true;
            return;
        }
        VkSurfaceFormatKHR surfaceFormat{};
        if (!chooseSurfaceFormat(surfaceFormat)) return;
        m_format = surfaceFormat.format;

        VkExtent2D extent;
        if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
        else
        {
            extent.width  = std::clamp(w, caps.minImageExtent.width,  caps.maxImageExtent.width);
            extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        m_extent = extent;
        if (extent.width == 0 || extent.height == 0) return;

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
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;   // universally supported
        ci.clipped          = VK_TRUE;
        if (vkCreateSwapchainKHR(dev, &ci, nullptr, &m_swapchain) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateSwapchainKHR failed.");
            m_swapchain = VK_NULL_HANDLE;
            return;
        }

        uint32_t actual = 0;
        vkGetSwapchainImagesKHR(dev, m_swapchain, &actual, nullptr);
        m_images.resize(actual);
        vkGetSwapchainImagesKHR(dev, m_swapchain, &actual, m_images.data());
        m_imageViews.resize(actual);
        for (uint32_t i = 0; i < actual; ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image            = m_images[i];
            viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format           = m_format;
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(dev, &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanPresenter", "vkCreateImageView failed for swapchain image " << i);
                return;
            }
        }
        if (!createRenderPass() || !createFramebuffers()) return;
        if (!buildPipeline(m_vertShader, m_fragShader, m_pipelineLayout, m_pipeline)) return;
        buildPipeline(m_blitVertShader, m_blitFragShader, m_blitPipelineLayout, m_blitPipeline);
    }

    bool createRenderPass()
    {
        VkAttachmentDescription color{};
        color.format         = m_format;
        color.samples        = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments    = &color;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;
        if (vkCreateRenderPass(m_instance->GetDevice(), &ci, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateRenderPass failed.");
            return false;
        }
        return true;
    }

    bool createFramebuffers()
    {
        m_framebuffers.resize(m_imageViews.size());
        for (size_t i = 0; i < m_imageViews.size(); ++i)
        {
            VkFramebufferCreateInfo ci{};
            ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            ci.renderPass      = m_renderPass;
            ci.attachmentCount = 1;
            ci.pAttachments    = &m_imageViews[i];
            ci.width           = m_extent.width;
            ci.height          = m_extent.height;
            ci.layers          = 1;
            if (vkCreateFramebuffer(m_instance->GetDevice(), &ci, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanPresenter", "vkCreateFramebuffer failed for image " << i);
                return false;
            }
        }
        return true;
    }

    // Both pipelines: empty vertex input (corners from gl_VertexIndex), a
    // triangle strip, source-over blending; viewport baked in, so a resize
    // rebuilds them.
    bool buildPipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkPipeline& out)
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, m_extent };
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.pViewports    = &viewport;
        vp.scissorCount  = 1;
        vp.pScissors     = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;
        VkPipelineMultisampleStateCreateInfo msaa{};
        msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        ba.blendEnable         = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp        = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.alphaBlendOp        = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &ba;
        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertexInput;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState      = &vp;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState   = &msaa;
        ci.pColorBlendState    = &blend;
        ci.layout              = layout;
        ci.renderPass          = m_renderPass;
        if (vkCreateGraphicsPipelines(m_instance->GetDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanPresenter", "vkCreateGraphicsPipelines failed.");
            return false;
        }
        return true;
    }

    // In call order, rebinding only when the kind changes; order is the
    // composition, the bind count only the cost.
    void record(VkCommandBuffer cmd, uint32_t imageIndex, const DeviceFrame& frame, const std::vector<UploadJob>& jobs)
    {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin);
        recordUploads(cmd, jobs);

        VkClearValue clear{};
        clear.color = { { frame.clear[0], frame.clear[1], frame.clear[2], frame.clear[3] } };
        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_renderPass;
        rp.framebuffer       = m_framebuffers[imageIndex];
        rp.renderArea.extent = m_extent;
        rp.clearValueCount   = 1;
        rp.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        const float fw = static_cast<float>(m_extent.width);
        const float fh = static_cast<float>(m_extent.height);
        VkPipeline bound = VK_NULL_HANDLE;
        for (const DeviceOp& d : frame.ops)
        {
            const float nx = (static_cast<float>(d.x) / fw) * 2.0f - 1.0f;
            const float ny = (static_cast<float>(d.y) / fh) * 2.0f - 1.0f;
            const float nw = (static_cast<float>(d.w) / fw) * 2.0f;
            const float nh = (static_cast<float>(d.h) / fh) * 2.0f;
            if (d.kind == DeviceOp::Kind::Rect)
            {
                if (bound != m_pipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline); bound = m_pipeline; }
                RectPush pc{ { nx, ny, nw, nh }, { d.c[0], d.c[1], d.c[2], d.c[3] } };
                vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 4, 1, 0, 0);
                continue;
            }
            auto it = m_textures.find(d.source);
            if (it == m_textures.end() || !it->second.everUploaded || m_blitPipeline == VK_NULL_HANDLE) continue;
            it->second.lastUsed = m_frameNo;
            if (bound != m_blitPipeline) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipeline); bound = m_blitPipeline; }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipelineLayout, 0, 1, &it->second.set, 0, nullptr);
            BlitPush pc{ { nx, ny, nw, nh }, d.c[3], { 0.0f, 0.0f, 0.0f } };
            vkCmdPushConstants(cmd, m_blitPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    void destroySwapchainDependents()
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (!dev) return;
        if (m_pipeline)     { vkDestroyPipeline(dev, m_pipeline, nullptr);     m_pipeline = VK_NULL_HANDLE; }
        if (m_blitPipeline) { vkDestroyPipeline(dev, m_blitPipeline, nullptr); m_blitPipeline = VK_NULL_HANDLE; }
        for (auto fb : m_framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
        m_framebuffers.clear();
        if (m_renderPass)   { vkDestroyRenderPass(dev, m_renderPass, nullptr); m_renderPass = VK_NULL_HANDLE; }
        for (auto v : m_imageViews) vkDestroyImageView(dev, v, nullptr);
        m_imageViews.clear();
        m_images.clear();
        if (m_swapchain)    { vkDestroySwapchainKHR(dev, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }

    void teardown()
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (dev) vkDeviceWaitIdle(dev);
        destroySwapchainDependents();
        if (dev)
        {
            for (auto& [rid, tex] : m_textures) { (void)rid; destroyTexture(tex); }
            m_textures.clear();
            if (m_pipelineLayout)     { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);     m_pipelineLayout = VK_NULL_HANDLE; }
            if (m_blitPipelineLayout) { vkDestroyPipelineLayout(dev, m_blitPipelineLayout, nullptr); m_blitPipelineLayout = VK_NULL_HANDLE; }
            for (VkShaderModule* m : { &m_vertShader, &m_fragShader, &m_blitVertShader, &m_blitFragShader })
                if (*m) { vkDestroyShaderModule(dev, *m, nullptr); *m = VK_NULL_HANDLE; }
            if (m_blitDescriptorPool) { vkDestroyDescriptorPool(dev, m_blitDescriptorPool, nullptr); m_blitDescriptorPool = VK_NULL_HANDLE; }
            if (m_blitSetLayout)      { vkDestroyDescriptorSetLayout(dev, m_blitSetLayout, nullptr); m_blitSetLayout = VK_NULL_HANDLE; }
            if (m_blitSampler)        { vkDestroySampler(dev, m_blitSampler, nullptr);               m_blitSampler = VK_NULL_HANDLE; }
            for (uint32_t i = 0; i < VK_PRESENTER_FRAMES_IN_FLIGHT; ++i)
            {
                if (m_imageAvailable[i]) vkDestroySemaphore(dev, m_imageAvailable[i], nullptr);
                if (m_renderFinished[i]) vkDestroySemaphore(dev, m_renderFinished[i], nullptr);
                if (m_inFlight[i])       vkDestroyFence(dev, m_inFlight[i], nullptr);
            }
            if (m_haveCommandBuffers)
                vkFreeCommandBuffers(dev, m_instance->GetCommandPool(), VK_PRESENTER_FRAMES_IN_FLIGHT, m_commandBuffers.data());
        }
        m_haveCommandBuffers = false;
        m_imageAvailable.fill(VK_NULL_HANDLE);
        m_renderFinished.fill(VK_NULL_HANDLE);
        m_inFlight.fill(VK_NULL_HANDLE);
        if (m_instance && m_surface) { vkDestroySurfaceKHR(m_instance->GetInstance(), m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
    }

    VulkanInstance*     m_instance = nullptr;
    NativeSurfaceHandle m_native{};

    VkSurfaceKHR   m_surface   = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat       m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D     m_extent    = { 0, 0 };
    uint32_t       m_want_w = 0, m_want_h = 0;
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass          m_renderPass         = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout     = VK_NULL_HANDLE;
    VkPipeline            m_pipeline           = VK_NULL_HANDLE;
    VkPipelineLayout      m_blitPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_blitPipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blitSetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      m_blitDescriptorPool = VK_NULL_HANDLE;
    VkSampler             m_blitSampler        = VK_NULL_HANDLE;
    VkShaderModule        m_vertShader = VK_NULL_HANDLE, m_fragShader = VK_NULL_HANDLE;
    VkShaderModule        m_blitVertShader = VK_NULL_HANDLE, m_blitFragShader = VK_NULL_HANDLE;
    std::unordered_map<ETCS::RID, Texture> m_textures;

    std::array<VkCommandBuffer, VK_PRESENTER_FRAMES_IN_FLIGHT> m_commandBuffers{};
    std::array<VkSemaphore, VK_PRESENTER_FRAMES_IN_FLIGHT>     m_imageAvailable{};
    std::array<VkSemaphore, VK_PRESENTER_FRAMES_IN_FLIGHT>     m_renderFinished{};
    std::array<VkFence, VK_PRESENTER_FRAMES_IN_FLIGHT>         m_inFlight{};
    bool     m_haveCommandBuffers = false;
    uint32_t m_currentFrame = 0;
    uint64_t m_frameNo      = 0;
    bool     m_lost         = false;
};

#endif // RENDERPROVIDER_VULKANPRESENTER_H__
