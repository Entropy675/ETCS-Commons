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
        // Straight after glfwInit, which is where the display connection
        // exists and before anything can issue a request against it.
        install_x_error_handler();

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
            // Both exist ONLY to re-prime the delta base -- see primeCursor.
            // Neither reports anything; they mark the moments at which the
            // cursor's position changes without the user having moved it.
            glfwSetCursorEnterCallback(GetHandle(), cursor_enter_callback);
            glfwSetWindowFocusCallback(GetHandle(), window_focus_callback);
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

    /*
 * The pump, and the only place motion becomes an event.
 *
 * glfwPollEvents runs every pending callback, so a burst of pointer movement
 * arrives as a burst of noteCursor calls that only ACCUMULATE. Flushing after
 * the pump turns however many the OS had queued into the one delta they sum
 * to -- the sampling rate becomes the rate something can actually consume,
 * rather than the rate the hardware reports.
 *
 * The flush is AFTER, not inside, because a delta is only complete once the
 * queue is drained; flushing per callback would be the uncoalesced behaviour
 * with extra steps.
 */
    void PollEventsConcrete() override
    {
        glfwPollEvents();
        flushPointerDelta();
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

    /*
 * The screen's real pixel density, from the monitor's physical size.
 *
 * Half of what turns mouse counts into an angle: counts/DPI is how far the
 * hand moved, and pixels/screen-DPI is how far that should carry across the
 * frame. GLFW knows this one because the EDID carries it.
 *
 * Zero when there is no monitor to ask -- a headless server, an unplugged
 * display -- and zero is the honest answer rather than a plausible default,
 * so a caller can tell "I could not find out" from "it is 96".
 */
    float GetScreenDpi()
    {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        if (!mon) return 0.0f;
        int mm_w = 0, mm_h = 0;
        glfwGetMonitorPhysicalSize(mon, &mm_w, &mm_h);
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        if (!mode || mm_w <= 0) return 0.0f;
        return static_cast<float>(mode->width) / (static_cast<float>(mm_w) / 25.4f);
    }

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
    static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void cursor_callback(GLFWwindow* window, double xpos, double ypos);
    static void cursor_enter_callback(GLFWwindow* window, int entered);
    static void window_focus_callback(GLFWwindow* window, int focused);

public:
    /*
 * Stop Xlib from calling exit() on us.
 *
 * Xlib's DEFAULT error handler prints and then calls exit() directly -- so a
 * single X protocol error, of the kind you get when a window is destroyed out
 * from under a poll loop, terminates the process from wherever the poll
 * happened to be. Here that is a ThreadPool worker, which means the static
 * destructors run on a pool thread and the pool tries to join the thread it is
 * running on; the visible symptom was "Resource deadlock avoided" and an abort
 * with nothing about X11 anywhere near it.
 *
 * A handler that logs and RETURNS makes the error what it actually is: a
 * request that failed. The window is already gone in that case, so the poll
 * loop sees ShouldClose on its next pass and the closure ends the ordinary
 * way. Installed once, process-wide, because that is the granularity Xlib
 * offers -- there is one error handler per process, not one per display.
 *
 * The I/O error handler is deliberately NOT replaced: it fires when the
 * connection to the server is gone entirely, and it is required not to
 * return. There is nothing to keep running at that point.
 */
#if !defined(_WIN32)
    // Xlib's own types live in glfw_native (see this file's header comment on
    // why X11's `Window` typedef is quarantined there), so the handler is
    // declared in those terms -- there is no second Xlib visible to name.
    static int x_error_handler(glfw_native::Display* dpy, glfw_native::XErrorEvent* ev)
    {
        char buf[256] = {0};
        glfw_native::XGetErrorText(dpy, ev->error_code, buf, sizeof(buf) - 1);
        ETCS_LOG("GLFWWindow", "X error (non-fatal): " << buf
                 << " -- request " << static_cast<int>(ev->request_code)
                 << "." << static_cast<int>(ev->minor_code)
                 << ". The window is likely gone; the poll loop will see it close.");
        return 0;
    }

    static void install_x_error_handler()
    {
        static bool installed = false;
        if (installed) return;
        installed = true;
        glfw_native::XSetErrorHandler(&GLFWWindow::x_error_handler);
    }
#else
    static void install_x_error_handler() {}   // no X server to protect against
#endif

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
        // Explicitly OFF, not merely unrequested: GLFW leaves the mode as it
        // found it, so a previous capture that enabled it would otherwise
        // persist and silently switch the units under the turn rate.
        glfwSetInputMode(GetHandle(), GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
#endif

        /*
     * RAW MOTION IS DELIBERATELY NOT REQUESTED, and this is the settled
     * answer after getting it wrong in both directions.
     *
     * Raw motion reports DEVICE COUNTS. Counts are a function of the mouse's
     * DPI, which no platform exposes, so any angle derived from them needs a
     * number the application has to be told -- and worse, DPI is a setting the
     * user changes to make their mouse faster or slower. Dividing it back out
     * cancels exactly the thing they adjusted it for.
     *
     * Without raw motion the deltas are VIRTUAL SCREEN UNITS: the same units
     * the window's width is measured in, already through whatever
     * acceleration curve the desktop applies. That is the right unit, because
     * the thing being calibrated is "how far across the window did the pointer
     * travel" -- a question with an exact answer in screen units and no answer
     * at all in counts without knowing the hardware.
     *
     * So DPI leaves the model entirely: the turn rate is 2*pi * turns divided
     * by the frame width, in the frame's own units, and a faster mouse is
     * simply a faster mouse (Scene3D::SetTurnsPerPass).
     */
        primeCursor("capture toggled");
        m_mouseCaptured = on;
        // Logged HERE rather than at the work function, because there are two
        // ways in -- a script calling CaptureMouse and the user pressing
        // Ctrl+Tab -- and a mode you cannot see the second route change is a
        // mode you debug by guessing.
        ETCS_LOG("GLFWWindow", (on ? "cursor captured -- pointer deltas are unbounded."
                                   : "cursor released."));
    }
    bool MouseCaptured() const { return m_mouseCaptured; }

    /*
 * Deltas are derived here rather than by the callback so the "no previous
 * position yet" case lives in one place with the state it concerns.
 *
 * ACCUMULATES RATHER THAN PUSHES. This runs inside glfwPollEvents, once per
 * queued movement, and the pump flushes the sum once the queue is drained
 * (PollEventsConcrete) -- so a thousand reports a second become one event per
 * pass with the same total displacement.
 *
 * m_cursorX/m_cursorY AND THE ACCUMULATOR ARE UNSYNCHRONISED, deliberately:
 * both are touched only from inside glfwPollEvents and the flush that follows
 * it, which is one thread by construction -- the OS event queue has exactly
 * one pump (Window.ProduceEvents), and every windowing backend requires that
 * anyway.
 */
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

        // THE FAIL STATE, not a filter -- see warpRejected.
        if (warpRejected(dx, dy)) return;

        accumulatePointerDelta(static_cast<int>(dx), static_cast<int>(dy));
    }

    /*
 * A WARP IS NOT A MOVEMENT, and telling the two apart is the whole of this.
 *
 * The cursor's position changes for two unrelated reasons: the user moved the
 * mouse, and something moved the cursor. Only the first is input. The second
 * happens on capture toggling, on the window taking or losing focus, on the
 * pointer re-entering the window, and -- the one that is invisible from here
 * -- on the platform recentring a disabled cursor to keep its virtual
 * position in range. Every one of those produces a position that is
 * discontinuous with the last, and subtracting the last position from it
 * yields a delta that is arithmetically correct and physically meaningless.
 *
 * It is the single worst input this path can produce. A flood of small deltas
 * makes the view feel over-sensitive; ONE warp snaps it somewhere else
 * entirely, in a single frame, and it reads as the control being broken
 * rather than fast. In a log it is unmistakable next to its neighbours --
 * (-29, -150) between two (1, 0)s -- and unmistakable is the point: it does
 * not average out, so no amount of tuning the turn rate touches it.
 */
    void primeCursor(const char* why)
    {
        if (!m_cursorPrimed) return;    // already waiting for a fresh base
        m_cursorPrimed = false;
        ETCS_LOG("GLFWWindow", "pointer delta base dropped (" << why
                 << ") -- the next report re-bases instead of reporting.");
    }

private:
    /*
 * The backstop for a warp that arrives with no callback to announce it, which
 * is what a platform-side recentre does.
 *
 * THE BOUND IS THE FRAME'S OWN WIDTH, and it is derived rather than tuned: a
 * pass across the frame is the full rotation the look control is defined
 * against (Scene3D::radiansPerPixel), so a single report claiming more than
 * that is claiming the user crossed their whole screen between two samples of
 * a device that reports hundreds of times a second. There is no hand movement
 * on the other side of that line, only a warp -- which is why a threshold is
 * admissible here and would not be at any smaller value.
 *
 * REJECTED LOUDLY AND WITH A RE-PRIME. Loudly, because a silently dropped
 * input event is indistinguishable from a bug in everything downstream. With
 * a re-prime, because whatever moved the cursor has left m_cursorX stale, so
 * the NEXT delta would be wrong too -- dropping only the first would turn one
 * bad sample into two.
 */
    bool warpRejected(double dx, double dy)
    {
        const WindowSize sz = GetSize();
        const double limit = (sz.width > 0)
            ? static_cast<double>(sz.width)
            : 4096.0;   // no extent yet: fall back to something no hand crosses

        if (dx > -limit && dx < limit && dy > -limit && dy < limit)
            return false;

        ETCS_LOG("GLFWWindow", "pointer delta (" << dx << ", " << dy
                 << ") exceeds the frame's " << limit << " unit width in one report"
                 " -- that is a cursor warp, not a movement. Dropped, and the delta"
                 " base re-primed.");
        m_cursorPrimed = false;
        return true;
    }

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

// Leaving is as important as entering: the cursor moves while it is away, and
// the first report after it comes back would otherwise measure against where
// it was when it left. Both edges re-prime for the same reason.
void GLFWWindow::cursor_enter_callback(GLFWwindow* window, int entered)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;
    handler->primeCursor(entered ? "cursor entered the window" : "cursor left the window");
}

// Focus is the one that bites under capture: losing focus releases the
// pointer, and regaining it re-grabs and recentres -- a position change with
// no movement behind it, arriving as one large delta.
void GLFWWindow::window_focus_callback(GLFWwindow* window, int focused)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;
    handler->primeCursor(focused ? "window regained focus" : "window lost focus");
}

void GLFWWindow::key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));

    /*
 * CTRL+TAB IS THE WAY OUT OF CAPTURE, and it is handled here rather than
 * being forwarded to whoever is consuming input.
 *
 * Escaping a captured cursor is a property of the WINDOW, not of what the
 * input happens to be driving. A scene consuming the stream can be busy, can
 * be a foreign module, can have crashed -- and in every one of those cases
 * the user still has to be able to get their pointer back. Routing the
 * release through the consumer would make the one control that must always
 * work depend on everything else working.
 *
 * It is also why the chord is a chord. Capture has to be MANDATORY for a look
 * control to be usable at all -- an uncaptured cursor hits the edge of the
 * screen and stops producing deltas, or warps and produces enormous false
 * ones, which is the flying camera this replaced -- so the exit cannot be a
 * key a scene might reasonably want to bind.
 *
 * Swallowed rather than forwarded: the consumer never sees the Tab, so
 * nothing downstream is left holding a key that was never released.
 */
    if (handler && key == GLFW_KEY_TAB && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL))
    {
        handler->SetMouseCapture(!handler->MouseCaptured());
        return;
    }

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
