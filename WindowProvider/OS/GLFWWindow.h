#ifndef GLFW_WINDOWHANDLER_H__
#define GLFW_WINDOWHANDLER_H__

#include "../../../ontology.h"
#include <GLFW/glfw3.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#include <memory>
#include <type_traits>

// GLFW'S ENTIRE STATE LIVES IN ONE THREAD'S JS SCOPE.
//
// emscripten implements GLFW in JavaScript (library_glfw.js), and its window
// list, hint table and callback table are plain properties of a `GLFW` object
// in the calling thread's scope. A pthread worker gets its own copy of the glue,
// so that object is there but EMPTY: glfwInit on the main thread does not
// initialise the worker's, and the worker's calls then read and write state no
// canvas is behind. The failure is not a missing feature --
//
//     glfwWindowHint  -> TypeError: Cannot set properties of null
//     glfwSetKeyCallback, glfwGetFramebufferSize, ... -> silently on nothing
//
// -- and the DOM is only reachable from the main thread regardless. ETCS control
// threads are workers, so every GLFW entry point this module reaches is routed
// through here.
namespace glfw_web {

// Run `f` on the main runtime thread and wait for it. The thunk is a plain
// function pointer because that is what the proxy queue carries; `f` itself
// stays on this thread's stack, which is shared memory, so a lambda may capture
// by reference and hand a result back through it.
//
// emscripten_proxy_sync, NOT emscripten_sync_run_in_main_runtime_thread. The
// latter is the legacy em_queued_call API: it mallocs a call record, encodes the
// arguments through a signature enum, and dispatches them back out through a
// switch -- three layers over the same queue this uses directly, for a call that
// is always void(void*).
template <typename F>
inline void on_main(F&& f)
{
    if (emscripten_is_main_runtime_thread()) { f(); return; }
    using Fn = typename ::std::remove_reference<F>::type;
    void (*thunk)(void*) = [](void* p) { (*static_cast<Fn*>(p))(); };
    emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                          emscripten_main_runtime_thread_id(),
                          thunk, static_cast<void*>(::std::addressof(f)));
}

} // namespace glfw_web
#endif // __EMSCRIPTEN__

// ETCS_GLFW_MAIN(statements) -- where GLFW is allowed to be touched from.
//
// One spelling for both substrates, so the call sites below read as ordinary
// GLFW code: a no-op wrapper on the desktop, a synchronous hop to the main
// runtime thread in the browser (see glfw_web above for why there is no choice).
// Statements, not an expression -- a value comes back through a local the
// lambda captures, which is also what keeps GLFW's out-parameters working.
#if defined(__EMSCRIPTEN__)
    #define ETCS_GLFW_MAIN(...) ::glfw_web::on_main([&]() { __VA_ARGS__; })
#else
    #define ETCS_GLFW_MAIN(...) do { __VA_ARGS__; } while (0)
#endif

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
    #include <GLFW/glfw3native.h>
#elif defined(__EMSCRIPTEN__)
    // No X11/Win32 native handles under emscripten. Canvas / WebGL is the
    // surface; NativeSurfaceHandle stays None (see populateNativeSurfaceHandle).
#else
    #define GLFW_EXPOSE_NATIVE_X11
    #include <GLFW/glfw3native.h>
#endif
}

#include <iostream>
#include <cstdlib>
#include <cmath>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <vector>

class GLFWWindow;

/*
 * GLFW IS ONE THING PER PROCESS. THIS IS THAT ONE THING.
 *
 * glfwInit, glfwTerminate and glfwPollEvents are library-wide, not
 * per-window: the second init is a no-op, the first terminate destroys
 * EVERY window, and one poll drains the queue for all of them. GLFW has
 * always treated them that way and says so. We were the ones pretending
 * otherwise -- init inside a window's create, terminate inside a window's
 * destructor, and a poll verb on each window that drained the shared queue
 * and then only did the post-poll work for itself.
 *
 * That pretence has three costs, and the two windows the paint editor wants
 * hit all of them. A second window's create ran glfwInit again for nothing.
 * A second window pumping in parallel ran glfwPollEvents concurrently, which
 * is not re-entrant. And the first window destroyed took the whole library
 * down with it, including the display connection the other one's Vulkan
 * swapchain still held -- a hang in the surface teardown, not a crash, which
 * is why it read as unrelated for so long.
 *
 * So the shape here is what GLFW already decided:
 *
 *   acquire/release   refcounted, so the library outlives every window and
 *                     dies with the last one.
 *   enroll/withdraw   the registry, so ONE poll pass does the post-poll work
 *                     for EVERY window -- which is what makes a second window
 *                     need no pump of its own.
 *   poll              try_lock, NOT lock. A second pumper arriving mid-pass
 *                     wants the queue drained, and it has been; making it
 *                     wait to drain an empty queue turns a frame loop into a
 *                     queue of frame loops. It returns false and gets on with
 *                     its frame.
 */
class GLFWPump
{
public:
    // Refcounted glfwInit. Hints and the error handler are set here because
    // they belong to the library's lifetime, not to any window's.
    static bool acquire();

    // Refcounted glfwTerminate. The LAST holder terminates -- see the
    // teardown hang above for what happens when the first one does.
    static void release();

    static void enroll(GLFWWindow* w);
    static void withdraw(GLFWWindow* w);

    // Drain the shared queue and run every enrolled window's post-poll work.
    // False means another thread is mid-pass, which is not a failure: the
    // queue this caller wanted drained is being drained right now.
    static bool poll();

private:
    inline static std::mutex                s_registry;
    inline static std::mutex                s_pumping;
    inline static std::vector<GLFWWindow*>  s_windows;
    inline static int                       s_refs = 0;
};

class GLFWWindow :
    public WindowBase<GLFWWindow>, public InputSourceBase<GLFWWindow>,
    public PointerBase<GLFWWindow>,
    public ResizableBase<GLFWWindow>, public DeletableBase<GLFWWindow>
{
private:
    // Position changes matter to nothing here any more -- see noteCursor --
    // but the window's own placement is still worth logging.
    static void scroll_callback(GLFWwindow* window, double dx, double dy);

    static void window_pos_callback(GLFWwindow* window, int x, int y)
    {
        auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (handler) {
            ETCS_LOG("WindowPos", "Position changed: " << x << ", " << y);
        }
    }

public:
    WIRE_TYPE_IDENTITY(GLFWWindow);

    /*
     * NOTHING IS INSIDE GLFW WITH THIS HANDLE WHEN IT IS DESTROYED.
     *
     * The tag transitions below settle WHO destroys and where that sits in the
     * module's order. They do not settle WHEN: the pump can be inside
     * glfwPollEvents on this very window at the moment another thread wins the
     * close, and GLFW's window functions are not re-entrant across threads.
     *
     * So every call into the platform takes its handle through here, and the
     * count is how many are inside. The destroy claims the handle FIRST and
     * only then waits the count out -- which terminates by construction,
     * because a caller arriving after the claim reads null and never raises
     * it. Both sides are seq_cst (raise, then load the handle here; store the
     * handle, then load the count there), which is what makes "I saw a live
     * handle" and "I saw nobody inside" mutually exclusive.
     *
     * A counter rather than a lock, because the callbacks GLFW runs during a
     * poll call back into this object -- a lock would have to be re-entrant
     * and would serialise the pump against every geometry query.
     */
    struct PlatformUse
    {
        GLFWWindow& owner;
        GLFWwindow* handle;

        explicit PlatformUse(GLFWWindow& w) : owner(w), handle(nullptr)
        {
            owner.m_platform_users.fetch_add(1);
            handle = static_cast<GLFWwindow*>(owner.m_window.load());
            if (!handle) owner.m_platform_users.fetch_sub(1);
        }
        ~PlatformUse() { if (handle) owner.m_platform_users.fetch_sub(1); }

        explicit operator bool() const { return handle != nullptr; }
        GLFWwindow* operator*()  const { return handle; }

        PlatformUse(const PlatformUse&)            = delete;
        PlatformUse& operator=(const PlatformUse&) = delete;
    };

    GLFWWindow()
    {
#if !defined(__EMSCRIPTEN__)
        // Custom arena allocator requires GLFW 3.4+ GLFWallocator. Emscripten's
        // port is older and has no such type -- use the default allocator.
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

        glfw_allocator.deallocate = [](void* /*block*/, void* /*user*/) {
            // No-op: arena is block-freed on module unload
        };

        glfw_allocator.user = &getArena();
        glfwInitAllocator(&glfw_allocator);
#endif
    }

    // No tag operation here, deliberately. This runs during teardown, where
    // the module's stream may already be stopping and a refused enqueue
    // answers "not yours" -- which would leak the window at the one moment
    // nothing is left to leak it to. Nothing races a destructor, so the plain
    // exchange is the whole of what is needed.
    /*
     * THE ORDER IS THE WHOLE OF IT.
     *
     * withdraw first, so no pump pass can still be reaching into this window
     * (it blocks out any pass already inside one). Then the handle. Then the
     * library share, LAST and refcounted -- terminating here rather than at
     * close time is what stopped a second window's Vulkan teardown hanging on
     * a display connection that had been pulled out from under it.
     */
    ~GLFWWindow()
    {
        ETCS_LOG("Cleanup for GLFWWindow called...");
        GLFWPump::withdraw(this);
        if (void* mine = m_window.exchange(nullptr)) drainAndDestroy(mine);
        if (m_holds_glfw) { m_holds_glfw = false; GLFWPump::release(); }
    }

    // The only glfwDestroyWindow in this file. Takes a handle its caller has
    // ALREADY claimed out of m_window, so nothing new can be inside it, and
    // waits out whatever was. Terminating by construction -- see PlatformUse.
    void drainAndDestroy(void* claimed)
    {
        while (m_platform_users.load() != 0) std::this_thread::yield();
        ETCS_LOG("GLFWWindow", "destroying handle " << claimed << ".");
        ETCS_GLFW_MAIN(glfwDestroyWindow(static_cast<GLFWwindow*>(claimed)));
    }

    /*
     * THE LIFECYCLE IS TWO FLAGS, AND THE FLAGS ARE THE CLAIMS.
     *
     *     opening   a create is in progress -- nothing about the window is
     *               settled yet
     *     active    the window is up, and IsActive() is this
     *
     * A tag operation is ordered on the module's own stream and now answers
     * whether it MOVED the surface (Entity::addTag/removeTag), so "did the
     * state change" and "was I the one who changed it" are one question with
     * one answer. That is all a claim ever was, and it needs no mechanism of
     * its own: the state surface already had to record the transition, and
     * recording it is what decides the winner.
     *
     * ENTRY IS A CLAIM because CreateWindow is called TWICE for one window in
     * the ordinary case -- `main.Create(...)` from the script and Run's own
     * CreateWindow, which every scene script relies on being idempotent --
     * from two different threads. `if (m_window) return` is a test and an act
     * with a gap both callers fit into: two glfwCreateWindow calls, one of the
     * two windows immediately unreachable.
     */
    void CreateWindowConcrete(const char* title = "invalid window", uint32_t width = 100, uint32_t height = 100)
    {
        /*
         * TWO FLAGS BECAUSE THERE ARE TWO QUESTIONS, and `opening` alone
         * cannot answer both. It is transient by design -- cleared the moment
         * the create settles -- so a caller arriving afterwards would claim it
         * again and build a SECOND window over the first, which is worse than
         * the race it replaced: the original handle leaks, the surface stays
         * bound to it, and the crash lands later somewhere else entirely.
         *
         * `active` is the settled answer, `opening` the in-progress one. This
         * pair of tests is not a test-and-act with a gap in it, because the
         * claim below is what serialises the creators: whoever holds `opening`
         * is the only one building, and the re-check after winning it closes
         * the one ordering another creator could have finished in.
         */
        if (this->hasTag("active"))
        {
            ETCS_LOG("CreateWindow", "already open -- no-op.");
            return;
        }

        if (!this->addTag("opening"))
        {
            /*
             * LOSING THE CLAIM MEANS WAITING FOR THE OUTCOME, not returning
             * from the middle of someone else's create. Run's very next line
             * is `if (!IsActive()) -> creation failed -> raise the closure
             * interrupt`, so a second caller coming back while the window is
             * still being built reports a failure that did not happen and
             * takes the whole script down with it. "Idempotent" has to mean
             * the caller returns to a SETTLED window.
             *
             * `opening` clears either way -- active, or back to nothing -- so
             * this ends on both outcomes. Bounded anyway: a create wedged
             * inside the platform is worth a line in the log rather than a
             * thread that never comes back.
             */
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (this->hasTag("opening"))
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    ETCS_LOG("CreateWindow", "another caller has been opening this window "
                             "for 10s and has not finished -- returning without waiting.");
                    break;
                }
                std::this_thread::yield();
            }
            ETCS_LOG("CreateWindow", "already open -- no-op.");
            return;
        }

        // Won the claim -- but another creator may have finished between the
        // test above and it. Asking again under the claim is the whole of what
        // makes the pair sound.
        if (this->hasTag("active"))
        {
            this->removeTag("opening");
            ETCS_LOG("CreateWindow", "already open -- no-op.");
            return;
        }

        // The library, refcounted. m_holds_glfw is this window's share of it
        // and is what the destructor gives back -- taken here rather than in
        // the constructor because a window that never opened never took one.
        if (!GLFWPump::acquire()) { this->removeTag("opening"); return; }
        m_holds_glfw = true;

        /*
         * NO CLIENT API ON EITHER PLATFORM, and in the browser that is a hard
         * constraint rather than a preference.
         *
         * The desktop draws through Vulkan, which does not want a GL context. A
         * canvas has exactly one context for its LIFETIME: whoever calls
         * getContext first decides the kind, and every later request for a
         * different kind returns null. Asking GLFW for GLFW_OPENGL_ES_API takes
         * a WebGL context here, which permanently denies the 2D one -- and
         * RenderProvider's browser surface presents by putImageData
         * (RenderProvider/OS/CanvasSurface.h), so the page could never draw.
         *
         * Nothing is lost by declining it. Event delivery, sizing and input are
         * independent of the client API, which is the half of GLFW this module
         * uses. A device-backed browser surface would take the context itself,
         * from the thread that will draw with it.
         *
         * The hint and the create are ONE hop: the hint table is per-thread
         * state, so setting it anywhere but where glfwCreateWindow reads it
         * would set it on a table nobody consults.
         */
        GLFWwindow* created = nullptr;
        ETCS_GLFW_MAIN(
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            created = glfwCreateWindow(static_cast<int>(width),
                                       static_cast<int>(height),
                                       title, nullptr, nullptr)
        );
        m_window.store(created);

        ETCS_LOG("Creating window with handle: " << created << ".");
        if (created)
        {
#if defined(__EMSCRIPTEN__)
            // Host page needs <canvas id="canvas"> (or Module.canvas). GLFW's
            // emscripten backend targets "#canvas" by default.
            //
            // No MakeContextCurrent/SwapInterval: there is no context to make
            // current (GLFW_NO_API above), and a context is owned by the thread
            // that created it -- binding one here, on whichever thread opened the
            // window, is not the thread that would draw with it anyway.
            glfw_web::on_main([&]() {
                emscripten_set_canvas_element_size("#canvas",
                    static_cast<int>(width), static_cast<int>(height));
                emscripten_set_element_css_size("#canvas",
                    static_cast<double>(width), static_cast<double>(height));
            });
#endif
            // One hop for the whole registration, not one per call. Every one of
            // these writes the window record the poll will later read, so they
            // belong in the same scope as the poll -- and a browser round trip
            // per line would be eleven of them.
            ETCS_GLFW_MAIN(
                glfwShowWindow(created);
                glfwFocusWindow(created);
                glfwSetWindowUserPointer(created, this);
                glfwSetFramebufferSizeCallback(created, framebuffer_size_callback);
                glfwSetKeyCallback(created, key_callback);
                glfwSetCursorPosCallback(created, cursor_callback);
                // Buttons were never wired at all, so anything wanting "press
                // here" had to borrow the keyboard -- see InputSource.h.
                glfwSetMouseButtonCallback(created, mouse_button_callback);
                // Registered beside the other pointer callbacks: a wheel is a
                // pointer event (ontology/Pointer.h), not a key one.
                glfwSetScrollCallback(created, scroll_callback);
                // Focus is tracked so capture can be re-applied on regaining it;
                // enter/leave only reports whether the pointer is over the frame.
                glfwSetCursorEnterCallback(created, cursor_enter_callback);
                glfwSetWindowFocusCallback(created, window_focus_callback);
                glfwSetWindowPosCallback(created, window_pos_callback)
            );
#if defined(__EMSCRIPTEN__)
            // Seeded, because on the web GetSizeConcrete reads the mirror rather
            // than asking GLFW, and the first reader runs before any resize
            // callback has had a reason to fire.
            m_size = { width, height };
#endif
            populateNativeSurfaceHandle();
            // Enrolled before `active` goes on, so the first pump pass that
            // can see this window as open already has its post-poll work.
            GLFWPump::enroll(this);
            this->addTag("active");
            ETCS_LOG("Window active! Instance: " << this->getRID() << ".");
        }

        // LAST, so that `opening` going off means everything else already
        // went on: it is what the loser above is waiting to see, and a waiter
        // released before `active` landed would read the window as failed.
        this->removeTag("opening");
    }


    /*
     * TWO PHASES, AND THE SECOND IS A TAG TRANSITION.
     *
     * Phase 1 states the intent and returns, because the pump has to SEE the
     * close to leave its loop -- that is the only reason closing has two
     * halves. Not a claim, and it does not need to be: the flag belongs to the
     * platform and setting it twice is setting it once.
     *
     * Phase 2 is reached by three threads for ONE cross press -- the pump, the
     * script's own Close(), and Run's exit -- so it cannot be a step each of
     * them performs. What they used to find was a live handle they all
     * destroyed, and destroying a window twice walks the toolkit's window list
     * off its end (Window_::m_window on why that is fatal rather than merely
     * redundant).
     *
     * `removeTag("active")` does BOTH jobs at once, which is why the claim
     * belongs on the tag surface and not beside it. It records the state --
     * the window is no longer active, and IsActive() answers no to the pump
     * and to Run before the thing they are looping on stops existing -- and
     * its answer names the one caller that made that transition. One
     * operation, ordered on the same stream as every other state change in
     * this module, and no second mechanism to keep in step with it.
     */
    void CloseWindowConcrete() override
    {
        {
            PlatformUse w(*this);
            if (!w)
            {
                ETCS_LOG("closeWindow", "already closed -- no-op.");
                return;
            }

#if defined(__EMSCRIPTEN__)
            // The browser's own flag, never GLFW's -- see ShouldCloseConcrete.
            if (!m_should_close.exchange(true))
            {
                ETCS_LOG("closeWindow", "phase 1 -- signalling close on handle " << *w << ".");
                return;
            }
#else
            int should = 0;
            ETCS_GLFW_MAIN(should = glfwWindowShouldClose(*w));
            if (should != GL_TRUE)
            {
                ETCS_LOG("closeWindow", "phase 1 -- signalling close on handle " << *w << ".");
                ETCS_GLFW_MAIN(glfwSetWindowShouldClose(*w, GL_TRUE));
                return;
            }
#endif
        }

        if (!this->removeTag("active"))
        {
            ETCS_LOG("closeWindow", "phase 2 went to another caller -- nothing to do.");
            return;
        }

        // Publication, not exclusion -- the transition above already settled
        // that this call is the only one here. Readers stop seeing the handle
        // before it stops existing.
        void* mine = m_window.exchange(nullptr);
        if (!mine) return;
        ETCS_LOG("closeWindow", "phase 2 -- claimed handle " << mine << ".");
        drainAndDestroy(mine);
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
        PlatformUse w(*this);
        if (!w) return true;
#if defined(__EMSCRIPTEN__)
        /*
         * ANSWERED HERE, NOT ASKED OF GLFW, and the browser is the one platform
         * where that is also the MORE correct answer. GLFW's `shouldClose` is
         * how a window manager reports the cross being pressed; a canvas in a
         * page has no cross and nothing else ever sets the flag, so the only
         * writer is CloseWindowConcrete -- us. Reading it back out of JS asks
         * the main thread a question only this object knows the answer to.
         *
         * And it is asked every pass of the pump, which is what makes it worth
         * removing rather than merely tidying: a synchronous hop to the main
         * thread at the pump's own rate is the heaviest traffic this module
         * generates, for a value that cannot have changed unless we changed it.
         */
        return m_should_close.load(::std::memory_order_acquire);
#else
        int should = 0;
        ETCS_GLFW_MAIN(should = glfwWindowShouldClose(*w));
        return should == GL_TRUE;
#endif
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
    void PollEventsConcrete() override { GLFWPump::poll(); }

    /*
     * This window's share of a poll pass, run by whichever thread won the
     * pump -- which is very often not this window's frame loop, and that is
     * the point: one pass serves every window.
     *
     * The PlatformUse is held across it because the pump is the one caller
     * GLFW would certainly be inside when another thread wins the close.
     */
    void afterPoll()
    {
        PlatformUse w(*this);
        if (!w) return;
#if !defined(__EMSCRIPTEN__)
        // The browser flushes at the callback instead, because that is where its
        // producer thread is -- see noteCursor. Doing it here as well would put a
        // second writer on a single-producer ring.
        flushPointerPosition();
#endif
        // The resize countdown, for the same reason and in the same place as
        // the pointer flush above it: a burst arrives as a burst of callbacks,
        // and what a consumer wants is the one value they settle on. This pass
        // is the only clock either of them needs -- the pump was already going
        // round. Costs an int read on a window nobody follows.
        settleResize();
        // After the poll, so the window has had every chance to become
        // viewable and focused before the mode is applied. See
        // SetMouseCapture for why this cannot be done where it is requested.
        applyMouseCapture(*w);
    }

    WindowPosition GetPositionConcrete() override
    {
        PlatformUse w(*this);
        if (!w) return {0, 0};

        int x = 0, y = 0;
        ETCS_GLFW_MAIN(glfwGetWindowPos(*w, &x, &y));
        return { static_cast<int32_t>(x), static_cast<int32_t>(y) };
    }

    void SetPositionConcrete(int32_t x, int32_t y) override
    {
        PlatformUse w(*this);
        if (!w) return;

        ETCS_GLFW_MAIN(glfwSetWindowPos(*w, x, y));
        ETCS_LOG("SetPosition", "Window moved to: " << x << ", " << y);
    }


    WindowSize GetSizeConcrete() override
    {
        PlatformUse w(*this);
        if (!w) return {0, 0};

#if defined(__EMSCRIPTEN__)
        /*
         * FROM THE MIRROR, for ShouldCloseConcrete's reason one step further on.
         * m_size is already what framebuffer_size_callback records (Resizable's
         * notifyResize), and that callback is the ONLY thing that can change a
         * canvas's framebuffer size -- so glfwGetFramebufferSize would hand back
         * the value this object just wrote, at the cost of a synchronous hop per
         * frame from whichever thread is presenting.
         */
        (void)w;
        return m_size;
#else
        int fw = 0, fh = 0;
        ETCS_GLFW_MAIN(glfwGetFramebufferSize(*w, &fw, &fh));
        m_size = { static_cast<uint32_t>(fw), static_cast<uint32_t>(fh) };
        return m_size;
#endif
    }

    /*
     * ASK the window manager. It is a request, not a setting -- a tiled or
     * fullscreen window is simply not resized, and the WM says so by not
     * sending the configure. So nothing is recorded here: m_size and every
     * listener move when framebuffer_size_callback fires, which is the only
     * place that knows what actually happened.
     *
     * True means the request went out, which is the most a window can
     * honestly claim (ontology/Resizable.h). The size in a window's own
     * coordinates is what glfwSetWindowSize takes, and it is the framebuffer
     * size we hand out -- they differ on a scaled display, and reconciling
     * that belongs with whoever introduces scaling rather than here.
     */
    bool ResizeTo(WindowSize s) override
    {
        if (s.width == 0 || s.height == 0) return false;
        PlatformUse w(*this);
        if (!w) return false;
        ETCS_GLFW_MAIN(glfwSetWindowSize(*w, static_cast<int>(s.width),
                                             static_cast<int>(s.height)));
        return true;
    }

    GLFWwindow* GetHandle() { return static_cast<GLFWwindow*>(m_window.load()); }

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
        int mm_w = 0, mm_h = 0, px_w = 0;
        ETCS_GLFW_MAIN(
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            if (mon)
            {
                glfwGetMonitorPhysicalSize(mon, &mm_w, &mm_h);
                if (const GLFWvidmode* mode = glfwGetVideoMode(mon)) px_w = mode->width;
            }
        );
        if (mm_w <= 0 || px_w <= 0) return 0.0f;
        return static_cast<float>(px_w) / (static_cast<float>(mm_w) / 25.4f);
    }

private:
    static void framebuffer_size_callback(GLFWwindow* window, int width, int height);
    static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void cursor_callback(GLFWwindow* window, double xpos, double ypos);
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
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
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
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
    static void install_x_error_handler() {}   // no X server (Win32 / emscripten)
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
        // May be too early -- no handle yet, or not viewable. The pump retries.
        { PlatformUse w(*this); if (w) applyMouseCapture(*w); }
    }

    // The only place the platform is actually told. Called from the pump,
    // which hands in the handle it is already holding open (PlatformUse).
    void applyMouseCapture(GLFWwindow* handle)
    {
        if (!m_capture_dirty || !handle) return;

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
        ETCS_GLFW_MAIN(glfwSetInputMode(handle, GLFW_CURSOR,
                         m_capture_want ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL));
#ifdef GLFW_RAW_MOUSE_MOTION
        // Explicitly OFF, not merely unrequested: GLFW leaves the mode as it
        // found it, so a previous capture that enabled it would otherwise
        // persist and silently switch the units under the turn rate.
        ETCS_GLFW_MAIN(glfwSetInputMode(handle, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE));
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
    /*
 * ── Pointer_ dispatch ────────────────────────────────────────────────────
 *
 * "Where is it now", as against InputSource's "what happened". Both are true of
 * a window and they do not overlap: a frame loop asks this every frame and
 * never misses anything by not listening; a tool that only acts on input reads
 * the ring. The family's own header makes the argument at length.
 *
 * Position is the last one RECORDED rather than the ring's tail, deliberately:
 * a poller must not consume events a stream reader is also owed, and the ring
 * is single-producer-multi-reader for exactly that reason.
 *
 * SCROLL IS CONSUMED BY READING, which is the honest behaviour for a delta and
 * is why ReadPointer returns a value instead of exposing fields -- two readers
 * of one pointer will not both see the same notch, and pretending otherwise
 * would apply it twice.
 */
    PointerState ReadPointerConcrete() override
    {
        PointerState st{};
        st.x = this->currentPointerX();
        st.y = this->currentPointerY();
        st.buttons  = m_buttons.load(::std::memory_order_acquire);
        st.scroll_x = this->takeScrollX();
        st.scroll_y = this->takeScrollY();
        return st;
    }

    /*
 * Whether the cursor is over this window's own region -- which the coordinates
 * cannot answer, since a position clamped to the edge and a position just
 * outside it read the same. Maintained by the enter/leave callback, because
 * that is the only thing that knows.
 */
    bool PointerInsideConcrete() override
    {
        return m_pointer_inside.load(::std::memory_order_acquire);
    }

    // Set from the button callback, read by Pointer_::ReadPointer. Bit 0 is the
    // primary button; the rest are the platform's numbering unchanged.
    void noteButtonState(int button, bool down)
    {
        if (button < 0 || button > 31) return;
        const uint32_t bit = 1u << button;
        if (down) m_buttons.fetch_or(bit, ::std::memory_order_release);
        else      m_buttons.fetch_and(~bit, ::std::memory_order_release);
    }

    void notePointerInside(bool inside)
    {
        m_pointer_inside.store(inside, ::std::memory_order_release);
    }

    void noteCursor(double x, double y)
    {
        notePointerAt(static_cast<int>(x), static_cast<int>(y));
#if defined(__EMSCRIPTEN__)
        /*
         * FLUSHED HERE, ON THE THREAD THE CALLBACK ARRIVED ON.
         *
         * InputSource's coalescing pair is single-producer by construction --
         * "record from inside the callback, flush once the queue is drained",
         * both on the thread that pumps the OS queue. In the browser those are
         * two DIFFERENT threads: emscripten's GLFW runs the callback from a DOM
         * listener on the main thread, while afterPoll runs on the detached pump
         * worker. Leaving the flush there put two writers on a single-producer
         * ring and raced m_pending* between them, which is not a lost sample --
         * it is a torn event that the consumer then acts on.
         *
         * What it costs is the coalescing: one position per DOM event instead of
         * one per pass. That is the cheap half to give up -- a position
         * supersedes the one before it and the ring laps rather than blocks, so
         * a consumer reading at frame rate sees the same position either way.
         */
        flushPointerPosition();
#endif
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

    // How many threads are inside GLFW with this window's handle. PlatformUse
    // is the only thing allowed to move it.
    std::atomic<int> m_platform_users{ 0 };

    // Does this window hold a share of the library? Taken on a create that
    // reached glfwInit, given back once in the destructor. Not an atomic
    // because only the create and the destructor touch it, and a create that
    // lost its claim returned before here.
    bool m_holds_glfw = false;
#if defined(__EMSCRIPTEN__)
    // The browser's close flag. See ShouldCloseConcrete for why GLFW's is not
    // the authority here.
    ::std::atomic<bool> m_should_close{ false };
#endif
    /*
 * WHAT Pointer_ NEEDS THAT THE RING DOES NOT CARRY.
 *
 * The ring is a log of edges; these two are the current state, and neither can
 * be recovered from the log by a reader that started late or missed a lap. Bit
 * 0 is the primary button on every device that has one, which ontology/
 * Pointer.h fixes and nothing else about the mask.
 */
    ::std::atomic<uint32_t> m_buttons{ 0 };
    ::std::atomic<bool>     m_pointer_inside{ false };
public:

    // Runs inside WindowProvider.so, against the GLFW copy that called
    // glfwInit() above -- see ontology/Window.h's NativeSurfaceHandle
    // comment for why this must not be duplicated anywhere else.
    void populateNativeSurfaceHandle()
    {
        PlatformUse w(*this);
        if (!w) return;
#if defined(_WIN32)
        m_nativeSurface.platform = NativeSurfacePlatform::Win32;
        m_nativeSurface.win32.hwnd = glfw_native::glfwGetWin32Window(*w);
        m_nativeSurface.win32.hinstance = GetModuleHandle(nullptr);
#elif defined(__EMSCRIPTEN__)
        // No OS window handle to hand to Vulkan/X11 interop. Canvas is owned
        // by GLFW's emscripten backend; leave platform = None.
        m_nativeSurface.platform = NativeSurfacePlatform::None;
#else
        m_nativeSurface.platform = NativeSurfacePlatform::X11;
        m_nativeSurface.x11.display = glfw_native::glfwGetX11Display();
        m_nativeSurface.x11.window  = static_cast<unsigned long>(glfw_native::glfwGetX11Window(*w));
#endif
    }
};

// ── GLFWPump ────────────────────────────────────────────────────────────────
//
// Out of line because the registry holds GLFWWindow*, which is incomplete
// where the class above is declared -- the same reason this file already
// defines its callbacks down here.

inline bool GLFWPump::acquire()
{
    std::lock_guard<std::mutex> g(s_registry);
    if (s_refs > 0) { ++s_refs; return true; }

    int up = 0;
#if defined(__EMSCRIPTEN__)
    // GLFW selects its emscripten platform for itself; there is only one.
    ETCS_GLFW_MAIN(up = glfwInit());
#else
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    up = glfwInit();
#endif
    if (!up) return false;
    // Straight after glfwInit, which is where the display connection exists
    // and before anything can issue a request against it (no-op on Win32 /
    // emscripten).
    GLFWWindow::install_x_error_handler();
    s_refs = 1;
    ETCS_LOG("GLFWPump", "GLFW up.");
    return true;
}

inline void GLFWPump::release()
{
    std::lock_guard<std::mutex> g(s_registry);
    if (s_refs == 0) return;
    if (--s_refs > 0) return;
    ETCS_LOG("GLFWPump", "last window gone -- GLFW down.");
    ETCS_GLFW_MAIN(glfwTerminate());
}

inline void GLFWPump::enroll(GLFWWindow* w)
{
    std::lock_guard<std::mutex> g(s_registry);
    for (GLFWWindow* e : s_windows) if (e == w) return;
    s_windows.push_back(w);
}

// Blocks out a pass already inside afterPoll for this window, which is the
// point: after this returns, no pump can be holding the pointer.
inline void GLFWPump::withdraw(GLFWWindow* w)
{
    std::lock_guard<std::mutex> g(s_registry);
    for (size_t i = 0; i < s_windows.size(); ++i)
        if (s_windows[i] == w) { s_windows.erase(s_windows.begin() + i); return; }
}

inline bool GLFWPump::poll()
{
    std::unique_lock<std::mutex> pumping(s_pumping, std::try_to_lock);
    if (!pumping.owns_lock()) return false;   // somebody else has the queue

    /*
     * NOT through ETCS_GLFW_MAIN, and this is the one GLFW call that must not
     * be. emscripten's glfwPollEvents is `() => 0` -- literally nothing. Its
     * backend registers DOM listeners at window creation and runs the callbacks
     * as the events arrive, so there is no queue for a poll to drain. Proxying
     * it would buy a synchronous round trip to the main thread PER PUMP PASS,
     * thousands a second, to call an empty function.
     *
     * The call stays rather than being #if'd away: it is what makes the pump the
     * same pump on both substrates, and on the desktop it is the whole of it.
     */
#if defined(__EMSCRIPTEN__)
    glfwPollEvents();
#else
    ETCS_GLFW_MAIN(glfwPollEvents());
#endif

    // Under the registry lock rather than over a snapshot: a copied vector of
    // raw pointers is a list of windows that were alive when it was taken.
    // Holding it means withdraw() waits for this pass, which is bounded --
    // afterPoll drops out immediately once the handle has been claimed.
    std::lock_guard<std::mutex> g(s_registry);
    for (GLFWWindow* w : s_windows) w->afterPoll();
    return true;
}


void GLFWWindow::framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (handler) {
        handler->notifyResize({ static_cast<uint32_t>(width), static_cast<uint32_t>(height) });
    }
}

/*
 * OPEN FOR BUSINESS, and in the browser that is a real question rather than a
 * formality.
 *
 * On the desktop a callback can only run from inside glfwPollEvents, so the pump
 * being round at all proves the window is up. emscripten's GLFW registers its DOM
 * listeners at glfwCreateWindow and the page delivers events from then on -- so a
 * pointer moving over the canvas while the rest of the session is still coming up
 * (modules loading, the REPL starting, the edges not yet stated) reaches this code
 * with the window half-built and nothing observing the rings it writes.
 *
 * `active` is the flag that says the window finished opening
 * (OpenWindowConcrete sets it LAST for exactly this kind of reason), so it is
 * what the input callbacks wait for. Costs a tag read per event and makes the
 * browser's delivery behave like the platform's: events arrive once there is
 * somebody to receive them.
 */
static inline GLFWWindow* input_ready(GLFWwindow* window)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return nullptr;
#if defined(__EMSCRIPTEN__)
    if (!handler->IsActive()) return nullptr;
#endif
    return handler;
}

/*
 * THE WHEEL, which had no home until Pointer_ turned out to have one.
 *
 * It travels the pointer ring beside the buttons, because a notch happens AT a
 * position and means different things depending on where -- and it accumulates
 * into the deltas Pointer_::ReadPointer hands back, because a per-frame consumer
 * wants "how much since I last looked" rather than a replay of notches. Both
 * readers, one source (ontology/InputSource.h::pushScroll).
 *
 * GLFW reports a wheel as a two-axis offset already normalised to notches, so
 * there is no unit conversion to do here -- which is the shape the ontology
 * asked for: a device converts once, at its own edge, and this device's units
 * are already the right ones.
 */
void GLFWWindow::scroll_callback(GLFWwindow* window, double dx, double dy)
{
    auto handler = input_ready(window);
    if (!handler) return;
    handler->pushScroll(static_cast<float>(dx), static_cast<float>(dy));
}

void GLFWWindow::cursor_callback(GLFWwindow* window, double xpos, double ypos)
{
    auto handler = input_ready(window);
    if (!handler) return;
    handler->noteCursor(xpos, ypos);
}

/*
 * A CLICK CARRIES THE POSITION IT HAPPENED AT, asked for here rather than
 * taken from the last motion callback.
 *
 * glfwGetCursorPos is the platform's current answer, and it is the right one:
 * a press can arrive in the same poll pass as the movement that led to it, and
 * reading our own last-recorded position would then be one event stale -- a
 * stroke starting a few pixels behind the cursor, which is the exact class of
 * bug the absolute-position model exists to remove.
 */
void GLFWWindow::mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
{
    (void)mods;
    auto handler = input_ready(window);
    if (!handler) return;
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    // The MASK is state, the ring entry is an edge -- see m_buttons. Updated
    // here because this is the only place that knows which way the edge went.
    handler->noteButtonState(button, action == GLFW_PRESS);
    handler->pushButton(button, action == GLFW_PRESS,
                        static_cast<int>(mx), static_cast<int>(my));
}

// Leaving is as important as entering: the cursor moves while it is away, and
// the first report after it comes back would otherwise measure against where
// it was when it left. Both edges re-prime for the same reason.
void GLFWWindow::cursor_enter_callback(GLFWwindow* window, int entered)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;

    // The one thing Pointer_::PointerInside can be answered from. A position
    // clamped to the edge and a position just outside it are the same numbers,
    // so the coordinates genuinely cannot say -- only this edge can.
    handler->notePointerInside(entered != 0);

    // NOTHING TO CHECK HERE ANY MORE. A pointer leaving the window used to be
    // evidence that a grab had been refused, because a relative control depends
    // on the pointer being confined. An absolute one does not: the angle simply
    // holds at the last position inside the frame, which is the correct answer
    // to "the pointer is not over the view". Leaving is now ordinary.
}

// Focus is the one that bites under capture: losing focus releases the
// pointer, and regaining it re-grabs and recentres -- a position change with
// no movement behind it, arriving as one large delta.
void GLFWWindow::window_focus_callback(GLFWwindow* window, int /*focused*/)
{
    auto handler = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
    if (!handler) return;
    // A grab does not survive losing focus, and a window manager handing focus
    // over is usually the FIRST moment the grab could have succeeded -- so
    // both edges re-raise the request rather than only the one that looks like
    // a recovery.
    handler->RequestCaptureReapply();
}

void GLFWWindow::key_callback(GLFWwindow* window, int key, int /*scancode*/, int action, int mods)
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
    // The chord above is deliberately NOT gated -- getting a captured pointer
    // back cannot depend on the session being ready. Everything below feeds the
    // key ring, so it is.
    if (!input_ready(window)) return;

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
