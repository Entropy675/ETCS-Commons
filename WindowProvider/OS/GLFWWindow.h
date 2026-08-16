#ifndef GLFW_WINDOWHANDLER_H__
#define GLFW_WINDOWHANDLER_H__

#include "../../../ontology.h"
#include <GLFW/glfw3.h>
#include <iostream>

class GLFWWindow : 
    public WindowBase<GLFWWindow>, public DeletableBase<GLFWWindow>
{
private:
    static void window_pos_callback(GLFWwindow* window, int x, int y)
    {
        auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (handler) {
            ETCS_LOG("WindowPos", "Position changed: " << x << ", " << y);
            // Could store or notify observers here (local event node likely?)
        }
    }

public:
    WIRE_TYPE_IDENTITY(GLFWWindow);
    
    GLFWWindow()
    {
        GLFWallocator glfw_allocator{};

        glfw_allocator.allocate = [](size_t size, void* user) -> void* {
            return static_cast<ETCS::MemoryArena*>(user)->allocateRaw(
                static_cast<long long>(size), 
                alignof(std::max_align_t)
            );
        };

        glfw_allocator.reallocate = [](void* block, size_t size, void* user) -> void* {
            void* new_block = static_cast<ETCS::MemoryArena*>(user)->allocateRaw(
                static_cast<long long>(size), 
                alignof(std::max_align_t)
            );
            if (block && new_block) 
                std::memcpy(new_block, block, size);
            return new_block;
        };

        glfw_allocator.deallocate = [](void* block, void* user) {
            // No-op: arena is block-freed on module unload
        };

        glfw_allocator.user = &getArena();
        glfwInitAllocator(&glfw_allocator);
    }
    
    ~GLFWWindow()
    {
        ETCS_LOG("Cleanup for GLFWWindow called...");
        if (m_window) {
            ETCS_LOG("Destroying window handle: " << m_window);
            glfwDestroyWindow(static_cast<GLFWwindow*>(m_window));
            m_window = nullptr;
        }
        glfwTerminate();    
    }

    void CreateWindowConcrete(const char* title = "invalid window", uint32_t width = 100, uint32_t height = 100)
    {
        if (m_window) return;
        
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        if (!glfwInit()) return;
        
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); 
        
        m_window = (void*)glfwCreateWindow(width, height, title, nullptr, nullptr);

        ETCS_LOG("Creating window with handle: " << m_window << ".");
        if (m_window) 
        {
            glfwShowWindow(static_cast<GLFWwindow*>(m_window));
            glfwFocusWindow(static_cast<GLFWwindow*>(m_window));
            glfwSetWindowUserPointer(GetHandle(), this);
            glfwSetFramebufferSizeCallback(GetHandle(), framebuffer_size_callback);
            glfwSetKeyCallback(GetHandle(), key_callback);
            glfwSetWindowPosCallback(GetHandle(), window_pos_callback);
            this->addTag("active");
            ETCS_LOG("Window active! Instance: " << this->getRID() << ".");
        }
    }
    

    void CloseWindowConcrete() override
    {
        ETCS_LOG("closeWindow: handle=" << m_window);
        
        if (!m_window) {
            ETCS_LOG("closeWindow: Already closed, no-op");
            return;
        }
        
        bool already_closing = (glfwWindowShouldClose(static_cast<GLFWwindow*>(m_window)) == GL_TRUE);
        
        if (!already_closing) {
            // PHASE 1: First call - just signal intent
            // Let the poll loop detect shouldClose() and handle cleanup
            ETCS_LOG("closeWindow: Phase 1 - signaling close");
            glfwSetWindowShouldClose(static_cast<GLFWwindow*>(m_window), GL_TRUE);
            // Do NOT remove tag or destroy window here!
        } else {
            // PHASE 2: Second call - poll loop has exited, do actual cleanup
            ETCS_LOG("closeWindow: Phase 2 - performing cleanup");
            this->removeTag("active");
            glfwDestroyWindow(static_cast<GLFWwindow*>(m_window));
            m_window = nullptr;
        }
    }
    
    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("closeWindow: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }
    
    bool ShouldCloseConcrete() override
    {
        if (!m_window) return true;
        return glfwWindowShouldClose(static_cast<GLFWwindow*>(m_window)) == GL_TRUE;
    }
    
    void PollEventsConcrete() override
    {
        glfwPollEvents();
    }
    
    WindowPosition GetPositionConcrete() override
    {
        if (!m_window) return {0, 0};
        
        int x, y;
        glfwGetWindowPos(static_cast<GLFWwindow*>(m_window), &x, &y);
        return { static_cast<int32_t>(x), static_cast<int32_t>(y) };
    }

    void SetPositionConcrete(int32_t x, int32_t y) override
    {
        if (!m_window) return;
        
        glfwSetWindowPos(static_cast<GLFWwindow*>(m_window), x, y);
        ETCS_LOG("SetPosition", "Window moved to: " << x << ", " << y);
    }
    
    
    WindowSize GetSizeConcrete() override
    {
        if (!m_window) return {0, 0};
        
        int w, h;
        glfwGetFramebufferSize(static_cast<GLFWwindow*>(m_window), &w, &h);
        m_size = { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
        return m_size;
    }
    
    GLFWwindow* GetHandle() { return static_cast<GLFWwindow*>(m_window); }

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
    static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
};


void GLFWWindow::framebuffer_size_callback(GLFWwindow* window, int width, int height) 
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (handler) {
        handler->notifyResize({ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    }
}

void GLFWWindow::key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
#ifdef ETCS_VERBOSE_INPUT_EVENTS
    ETCS_LOG("GLFWWindow:Global", "GLFW user callback ptr: " << window << " handler: " << handler << " key: " << key);
#endif
    if (!handler) return;

    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {   
#ifdef ETCS_VERBOSE_INPUT_EVENTS
        ETCS_LOG("GLFWWindow:Global", "Detected key down: " << (char)key);
#endif
        handler->pushKeyDown(key);
    }
    else
    {
#ifdef ETCS_VERBOSE_INPUT_EVENTS
        ETCS_LOG("GLFWWindow:Global", "Detected key up: " << (char)key);
#endif
        handler->pushKeyUp(key);
    }
}

#endif
