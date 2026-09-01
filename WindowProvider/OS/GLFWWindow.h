#ifndef GLFW_WINDOWHANDLER_H__
#define GLFW_WINDOWHANDLER_H__

#include "../../../ontology.h"
#include <GLFW/glfw3.h>

// Native-handle extraction (NativeSurfaceHandle, ontology/Window.h) needs
// GLFW's platform-native accessors. Exposed here, inside WindowProvider's
// own compiled code, against the one GLFW copy that actually called
// glfwInit() below -- see ontology/Window.h's own comment on why this
// must never be computed anywhere else.
//
// Wrapped in its own namespace, not included at file scope: glfw3native.h
// with GLFW_EXPOSE_NATIVE_X11 pulls in the real X11/Xlib.h to declare
// glfwGetX11Display/glfwGetX11Window, and X11/X.h typedefs `Window` as
// `XID` (an unsigned long) at global scope -- a hard "conflicting
// declaration" against this module's own `typedef GLFWWindow Window`
// (Contract_WindowProvider.h), confirmed by actually hitting that error.
// A #include's top-level declarations become members of whatever
// namespace it textually sits inside, so this confines X11's `Window`
// (and everything else Xlib.h brings in) to glfw_native, where nothing
// else in this module ever looks for it.
namespace glfw_native {
#if defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
#else
    #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
}

#include <iostream>

class GLFWWindow :
    public WindowBase<GLFWWindow>, public InputSourceBase<GLFWWindow>,
    public ResizableBase<GLFWWindow>, public DeletableBase<GLFWWindow>
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
            glfwSetCursorPosCallback(GetHandle(), cursor_callback);
            glfwSetWindowPosCallback(GetHandle(), window_pos_callback);
            populateNativeSurfaceHandle();
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
    static void cursor_callback(GLFWwindow* window, double xpos, double ypos);

public:
    /*
 * Mouse capture: hide the cursor and free it from the screen's edges, so
 * pointer deltas keep arriving however far the user keeps moving.
 *
 * It is a MODE, not a a per-event decision, which is why it is a call rather
 * than a flag on the events: the cursor either has a position on screen or it
 * does not, and a look control needs it not to. Raw motion is requested where
 * the platform has it -- it is the unaccelerated delta, which is what a view
 * angle wants, as against the pointer-ballistics curve a desktop cursor wants.
 *
 * The first delta after capture is DISCARDED (m_cursorPrimed). Enabling
 * capture warps the cursor, so the first callback reports the jump from
 * wherever it was to the centre -- a large bogus delta that would snap the
 * view a quarter turn on the first frame. Found immediately on trying it.
 */
    void SetMouseCapture(bool on)
    {
        if (!GetHandle()) return;
        glfwSetInputMode(GetHandle(), GLFW_CURSOR,
                         on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#ifdef GLFW_RAW_MOUSE_MOTION
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(GetHandle(), GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
#endif
        m_cursorPrimed = false;
        m_mouseCaptured = on;
    }
    bool MouseCaptured() const { return m_mouseCaptured; }

    // Deltas are derived here rather than by the callback so the "no previous
    // position yet" case lives in one place with the state it concerns.
    void noteCursor(double x, double y)
    {
        if (!m_cursorPrimed)
        {
            m_cursorX = x; m_cursorY = y; m_cursorPrimed = true;
            return;
        }
        const double dx = x - m_cursorX;
        const double dy = y - m_cursorY;
        m_cursorX = x; m_cursorY = y;
        pushPointerDelta(static_cast<int>(dx), static_cast<int>(dy));
    }

private:
    double m_cursorX = 0.0;
    double m_cursorY = 0.0;
    bool   m_cursorPrimed  = false;
    bool   m_mouseCaptured = false;
public:

    // Runs inside WindowProvider.so, against the GLFW copy that called
    // glfwInit() above -- see ontology/Window.h's NativeSurfaceHandle
    // comment for why this must not be duplicated anywhere else.
    void populateNativeSurfaceHandle()
    {
        if (!m_window) return;
#if defined(_WIN32)
        m_nativeSurface.platform = NativeSurfacePlatform::Win32;
        m_nativeSurface.win32.hwnd = glfw_native::glfwGetWin32Window(GetHandle());
        m_nativeSurface.win32.hinstance = GetModuleHandle(nullptr);
#else
        m_nativeSurface.platform = NativeSurfacePlatform::X11;
        m_nativeSurface.x11.display = glfw_native::glfwGetX11Display();
        m_nativeSurface.x11.window  = static_cast<unsigned long>(glfw_native::glfwGetX11Window(GetHandle()));
#endif
    }
};


void GLFWWindow::framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (handler) {
        handler->notifyResize({ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    }
}

void GLFWWindow::cursor_callback(GLFWwindow* window, double xpos, double ypos)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;
    handler->noteCursor(xpos, ypos);
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
