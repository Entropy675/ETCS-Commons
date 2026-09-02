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
#include <cstdlib>
#include <cmath>

class GLFWWindow :
    public WindowBase<GLFWWindow>, public InputSourceBase<GLFWWindow>,
    public ResizableBase<GLFWWindow>, public DeletableBase<GLFWWindow>
{
private:
    // Position changes matter to nothing here any more -- see noteCursor --
    // but the window's own placement is still worth logging.
    static void window_pos_callback(GLFWwindow* window, int x, int y)
    {
        auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (handler) {
            ETCS_LOG("WindowPos", "Position changed: " << x << ", " << y);
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
            // Focus is tracked so capture can be re-applied on regaining it;
            // enter/leave only reports whether the pointer is over the frame.
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
        flushPointerPosition();
        // After the poll, so the window has had every chance to become
        // viewable and focused before the mode is applied. See
        // SetMouseCapture for why this cannot be done where it is requested.
        applyMouseCapture();
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
 * Nothing has to be discarded on the transition: a position is valid the
 * moment it is read, whatever the cursor was doing before.
 */
    /*
 * CAPTURE IS AN INTENT, APPLIED BY THE PUMP, and what it means is narrower
 * than it sounds: hide the cursor, and take responsibility for recycling it
 * It is deliberately NOT "ask the platform to lock the pointer" -- see
 * applyMouseCapture for why that mode cannot be used even where it works.
 *
 * WHY THE PUMP AND NOT HERE. A script calls this immediately after Create,
 * which is the only sensible place to put it, and at that moment the window
 * has been asked to map but is not yet viewable, no glfwPollEvents has run
 * because the pump is detached later in the script, and this is the SCRIPT
 * thread while the pump will run on a pool worker. Input modes belong to
 * whichever thread pumps the event queue, and they are set on a window the
 * server has had a chance to map. So m_capture_want is what the script asked
 * for and never changes underneath anyone, m_capture_dirty says the platform
 * has not been told yet, and PollEventsConcrete carries it out. Focus changes
 * re-raise it: a window manager handing focus over later is the ordinary case
 * rather than an edge one.
 *
 * m_mouseCaptured tracks what was actually APPLIED, not what was asked. The
 * distinction is kept because a mode believed-but-not-applied is exactly the
 * kind of state that produces a control nobody can explain.
 */
    void SetMouseCapture(bool on)
    {
        m_capture_want  = on;
        m_capture_dirty = true;
        if (on)
        {
            /*
         * THE SESSION IS LOGGED WITH THE REQUEST because it is the first
         * thing worth knowing when a capture does not take, and it cannot be
         * asked for after the fact.
         *
         * This module pins GLFW to X11 (glfwInitHint above), so on a Wayland
         * desktop everything here runs through XWayland -- where a pointer
         * grab is subject to the compositor rather than to the X server, and
         * confining a cursor by warping it is exactly the operation XWayland
         * restricts. A capture that works on X11 and silently does nothing on
         * XWayland is not a bug in this code, but it IS indistinguishable
         * from one in a log that does not say which it was on.
         */
            const char* wl = std::getenv("WAYLAND_DISPLAY");
            ETCS_LOG("GLFWWindow", "cursor capture requested -- the pump applies it once the "
                     "window is viewable. Session: "
                     << (wl && *wl ? "WAYLAND detected (WAYLAND_DISPLAY set) -- this module "
                                     "pins GLFW to X11, so the grab goes through XWayland, "
                                     "which may refuse to confine the pointer"
                                   : "X11"));
        }
        else
            ETCS_LOG("GLFWWindow", "cursor release requested.");
        applyMouseCapture();   // may be too early; the pump will retry
    }

    // The only place the platform is actually told. Called from the pump.
    void applyMouseCapture()
    {
        if (!m_capture_dirty || !GetHandle()) return;

        /*
     * GLFW_CURSOR_HIDDEN, NOT GLFW_CURSOR_DISABLED, and this is the whole of
     * the Qubes fix.
     *
     * DISABLED asks GLFW to confine the pointer, which it implements by
     * warping the cursor back to the window centre after every poll and
     * reporting a VIRTUAL position it accumulates itself. Look at what that
     * accumulation assumes (x11_window.c): GLFW sets its own lastCursorPos to
     * the centre and THEN issues the warp. If the warp does not land, the next
     * motion event reports the real position and GLFW computes
     *
     *     dx = real_position - window_centre
     *
     * which is an OFFSET, not a delta. Every event then contributes the
     * pointer's distance from the centre, so holding the pointer off-centre
     * turns the view continuously and moving it a little near the edge turns
     * the view a lot. The entire screen becomes a joystick, the gain looks
     * enormous, and the camera keeps turning after the hand stops -- which is
     * what "inertia" was.
     *
     * The warp not landing is not exotic. A compositor that proxies windows
     * from another domain (Qubes), XWayland, and remote displays all decline
     * to teleport a pointer on a client's say-so. There is no way to ask
     * whether it worked, and the failure mode is not degraded input, it is
     * input that means something else entirely.
     *
     * HIDDEN asks for none of that: no warping, no virtual accumulator, and
     * glfwGetCursorPos reports the true content-area position. The deltas are
     * then OURS, differenced in noteCursor from positions we can trust, and
     * they stay correct whether or not the platform will move a pointer.
     * And with an ABSOLUTE control there is nothing left to confine: the
     * pointer's position over the frame is the angle, so it never needs to
     * keep going past an edge. Hiding it is the whole of what capture does.
     */
        glfwSetInputMode(GetHandle(), GLFW_CURSOR,
                         m_capture_want ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
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
     * the camera's frame is measured in, already through whatever acceleration
     * curve the desktop applies. That is the right unit, because the rate is
     * defined as view pixels per pointer pixel (Scene3D::SetSensitivity) --
     * a question with an exact answer in screen units and no answer at all in
     * counts without knowing the hardware.
     */

        // Only now is it true. The frame noteCursor measures in follows this
        // flag, so it must mean "applied", never "asked for".
        const bool was = m_mouseCaptured;
        m_mouseCaptured = m_capture_want;
        m_capture_dirty = false;

        if (was != m_mouseCaptured)
            ETCS_LOG("GLFWWindow", (m_mouseCaptured
                     ? "cursor captured -- pointer deltas are unbounded."
                     : "cursor released."));
    }

    bool MouseCaptured() const { return m_mouseCaptured; }

    // Ask the pump to re-issue the current intent, for the moments when the
    // mode is known to have been dropped or to have become settable.
    void RequestCaptureReapply() { m_capture_dirty = true; }

    /*
 * NOTHING RECENTRES THE POINTER, and that is a decision rather than a gap. An
 * absolute control has no reason to move the cursor: the position IS the
 * angle, so there is nowhere it needs to "keep going" to. Gone with the
 * recentring is the warp, the check that the warp landed, the fallback for
 * when it did not, and the whole failure mode where a declined warp turns
 * offsets into deltas.
 */

    /*
 * The pointer's position, recorded and nothing else -- no differencing, no
 * state to keep consistent across a warp, a focus change or a window move.
 * See the block below on what that removes, and ontology/InputSource.h on why
 * the primitive changed.
 *
 * Window-relative is exactly the frame the angle wants: the look is defined
 * against the camera's own frame (Scene3D::PointerPosition), so a window that
 * moves changes nothing.
 */
    void noteCursor(double x, double y)
    {
        notePointerAt(static_cast<int>(x), static_cast<int>(y));
    }

    /*
 * GONE WITH THE DELTA: primeCursor, warpRejected, noteWindowOrigin and the
 * originX/originY pair.
 *
 * Every one of them existed to keep a DIFFERENCE trustworthy. A delta is the
 * gap between two positions, so any position change that was not the user's
 * hand had to be found and suppressed -- a capture toggle, a focus change,
 * the cursor re-entering, the window being dragged (which shifts a
 * content-area reading by the whole displacement), the platform recentring.
 * Five different routes to the same wrongness, each needing its own detector.
 *
 * A position needs none of it. It is where the pointer is, whatever put it
 * there. The window can move, focus can change, the cursor can leave and come
 * back, and the reading is still exactly the angle the user is asking for.
 */
private:
    // What was ASKED for, and whether the platform has been told yet. Kept
    // apart from m_mouseCaptured, which is what actually took -- see
    // SetMouseCapture.
    bool   m_capture_want   = false;
    bool   m_capture_dirty  = false;
    bool   m_mouseCaptured  = false;
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

    // NOTHING TO CHECK HERE ANY MORE. A pointer leaving the window used to be
    // evidence that a grab had been refused, because a relative control depends
    // on the pointer being confined. An absolute one does not: the angle simply
    // holds at the last position inside the frame, which is the correct answer
    // to "the pointer is not over the view". Leaving is now ordinary.
}

// Focus is the one that bites under capture: losing focus releases the
// pointer, and regaining it re-grabs and recentres -- a position change with
// no movement behind it, arriving as one large delta.
void GLFWWindow::window_focus_callback(GLFWwindow* window, int focused)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;
    // A grab does not survive losing focus, and a window manager handing focus
    // over is usually the FIRST moment the grab could have succeeded -- so
    // both edges re-raise the request rather than only the one that looks like
    // a recovery.
    handler->RequestCaptureReapply();
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
