#ifndef WINDOWPROVIDER_H__
#define WINDOWPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_WindowProvider.h"

#include <vector>
#include <sstream>
#include <chrono>
#include <thread>

// ONE place decides what closing a window means, because there are four ways
// to arrive here -- the cross (seen by PollEvents or by Run), an explicit
// Close(), and a failed create -- and they did not agree. Only the two poll
// paths signalled at all, and what they raised was `*ctx.interrupt`, which is
// this CALL's scope flag (Scope::registerContext, Bundles.h): it dies with
// the call, so nothing the script detached ever heard it. Close() signalled
// nothing whatsoever, which is why a script could close its window and then
// delete entities a detached frame edge was still presenting through.
//
// A window closing ends the closure that opened it -- every RID that closure
// granted stops being valid together -- so the raise goes to the closure, not
// to the frame. run_script drains on it before the next line (CommandExecutor.h).
template <typename W>
static void window_closed(W& self, const ETCS::SignalContext& ctx, const char* why)
{
    ETCS_LOG("Window", why << " -- ending the closure that opened it.");
    if (!ctx.raiseClosureInterrupt())
        ETCS_LOG("Window", "nothing on the active chain holds interrupt authority -- "
                 "the close was not observable by anything outside this call.");
    self.CloseWindow();
}

// Simple position setter
DEFINE_WORK_FUNC_TYPED(Window, SetPosition, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    ETCS_LOG("SetPosition", "Setting window position: (" << x << ", " << y << ")");
    self.SetPosition(x, y);
}

// Move by offset (uses GetPosition internally)
DEFINE_WORK_FUNC_TYPED(Window, MoveBy, (int32_t, dx), (int32_t, dy))
{
    (void)ctx;
    auto pos = self.GetPosition();
    int32_t new_x = pos.x + dx;
    int32_t new_y = pos.y + dy;
    ETCS_LOG("MoveBy", "Moving window by (" << dx << ", " << dy 
             << ") to (" << new_x << ", " << new_y << ")");
    self.SetPosition(new_x, new_y);
}

// Center on primary monitor
DEFINE_WORK_FUNC(Window, CenterOnMonitor)
{
    (void)data;
    (void)ctx;
    
    if (!self.GetHandle()) return;
    
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (!monitor) return;
    
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    int monitor_x, monitor_y;
    glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
    
    auto size = self.GetSize();
    
    int32_t x = monitor_x + (mode->width - static_cast<int>(size.width)) / 2;
    int32_t y = monitor_y + (mode->height - static_cast<int>(size.height)) / 2;
    
    ETCS_LOG("CenterOnMonitor", "Centering window at: " << x << ", " << y);
    self.SetPosition(x, y);
}

DEFINE_WORK_FUNC_TYPED(Window, Create, (uint32_t, x), (uint32_t, y), (std::string, word))
{
    (void)ctx;
    if (!x || !y) return;
    ETCS_LOG("Create", "Window.Create got size: (" << x << "," << y << ") " << word);
    self.CreateWindow(word.c_str(), x, y);

    if (!self.IsActive())
    {
        ETCS_LOG("Create", "Window creation failed.");
        ctx.raiseClosureInterrupt();
    }
}

DEFINE_WORK_FUNC_TYPED(Window, Run, (uint32_t, x), (uint32_t, y), (std::string, title))
{
    if (!x || !y) { ETCS_LOG("Run", "Invalid window size."); return; }

    ETCS_LOG("Run", "Creating window: " << title << " (" << x << "x" << y << ")");
    self.CreateWindow(title.c_str(), x, y);

    if (!self.IsActive())
    {
        ETCS_LOG("Run", "Window creation failed.");
        ctx.raiseClosureInterrupt();
        return;
    }

    ETCS_LOG("Run", "Window active, blocking execution thread."); 

    // Block the script execution thread while the detached stream edge (ProduceEvents)
    // handles the OS message pump and input events.
    while (self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps idle sleep
    }

    self.CloseWindow();
    ETCS_LOG("Run", "Window closed, execution resumed.");
}

/*
 * THIS BODY IS THE WINDOW'S ONLY EVENT PUMP. Window.Run does not poll -- it
 * sleeps on IsActive -- so if this loop stops going round the window stops
 * being serviced entirely: no input, no resize, no close, and on X11 no
 * presentation either, because a client that does not drain its connection
 * stalls its own swapchain.
 *
 * SO NOTHING BETWEEN TWO POLLS MAY WAIT ON A CONSUMER. writeRaw blocks when the
 * reader is behind, so an unbounded drain lets a fast producer keep this loop
 * from ever reaching the next poll. The drain is bounded to what was in the
 * ring at the top of the pass.
 *
 * KEYS ONLY. Pointer traffic has its own ring and its own edge
 * (ProducePointer), so a keystroke never waits behind a burst of positions.
 */
DEFINE_STREAM_FUNC_PRODUCE(Window, ProduceEvents)
{
    (void)data;

    while (!self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) return;
        std::this_thread::yield();
    }

    uint8_t id = self.RegisterKeyObserver();
    ETCS_LOG("ProduceEvents", "key edge open, observer " << (int)id);
    if (id == INPUT_INVALID_OBSERVER) return;

    ETCS::Buffer slot;
    bool stream_alive = true;

    while (self.IsActive() && stream_alive)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        self.PollEvents();
        if (self.ShouldClose())
        {
            window_closed(self, ctx, "ProduceEvents: window close requested");
            break;
        }

        bool emitted = false;
        uint32_t budget = 128;
        while (budget-- && self.ReadNextKeyEvent(id, slot))
        {
            if (!stream.writeRaw(slot)) { stream_alive = false; break; }
            emitted = true;
        }

        if (id == INPUT_INVALID_OBSERVER) id = self.RegisterKeyObserver();

        // Sleep, not yield: input is applied once per frame and cannot tell 1ms
        // from 0, while a yield loop holds a pool worker at 100% for the
        // window's whole life -- taken straight out of the frame edge.
        if (!emitted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (stream.isOpen()) stream.closeWrite();
    self.UnregisterKeyObserver(id);
}

/*
 * The pointer's own edge. Positions only, and it does NOT pump -- ProduceEvents
 * owns the poll loop, because two threads calling into the platform's event
 * queue is undefined on every backend worth supporting.
 *
 * A consumer that wants both channels binds both. That is the cost of not
 * making a keystroke queue behind a thousand positions, and it is the right
 * trade: correlation between the two is needed only where a press means
 * "at the current position", and an absolute position answers that from the
 * last sample without any ordering guarantee at all.
 */
DEFINE_STREAM_FUNC_PRODUCE(Window, ProducePointer)
{
    (void)data;

    while (!self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) return;
        std::this_thread::yield();
    }

    uint8_t id = self.RegisterPointerObserver();
    ETCS_LOG("ProducePointer", "pointer edge open, observer " << (int)id);
    if (id == INPUT_INVALID_OBSERVER) return;

    ETCS::Buffer slot;
    bool stream_alive = true;

    while (self.IsActive() && stream_alive)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        bool emitted = false;
        uint32_t budget = 16;
        while (budget-- && self.ReadNextPointerEvent(id, slot))
        {
            if (!stream.writeRaw(slot)) { stream_alive = false; break; }
            emitted = true;
        }

        if (id == INPUT_INVALID_OBSERVER) id = self.RegisterPointerObserver();
        if (!emitted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (stream.isOpen()) stream.closeWrite();
    self.UnregisterPointerObserver(id);
}

// Demo consumers. Keys are printed per event because a keyboard is slow enough
// to read; positions are counted and sampled because a pointer is not, and
// per-event formatted I/O on that channel makes the reader the bottleneck.
DEFINE_STREAM_FUNC_CONSUME(Window, ConsumeEvents)
{
    (void)data; (void)self;

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.key == 0) continue;

        ETCS_LOG("ConsumeEvents", (ev.action == INPUT_DOWN ? "KEY DOWN: " : "KEY UP:   ")
                 << ev.key << " (" << (char)ev.key << ")");
    }

    ETCS_LOG("ConsumeEvents", "key edge closed.");
}

DEFINE_STREAM_FUNC_CONSUME(Window, ConsumePointer)
{
    (void)data; (void)self;

    uint64_t count = 0;
    int      last_x = 0, last_y = 0;
    auto     reported = std::chrono::steady_clock::now();

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action != INPUT_MOTION) continue;

#ifdef ETCS_VERBOSE_INPUT_EVENTS
        ETCS_LOG("ConsumePointer", "POINTER:  (" << ev.x << ", " << ev.y << ")");
#endif
        ++count; last_x = ev.x; last_y = ev.y;

        const auto now = std::chrono::steady_clock::now();
        if (now - reported >= std::chrono::seconds(1))
        {
            // The LAST position, not a sum: these are positions and the latest
            // supersedes the rest. The count says how hard the pointer reports.
            ETCS_LOG("ConsumePointer", count << " reports in the last second, now at ("
                     << last_x << ", " << last_y << ").");
            count = 0;
            reported = now;
        }
    }

    ETCS_LOG("ConsumePointer", "pointer edge closed.");
}

// CaptureMouse <0|1> -- hide the cursor and unbind it from the screen, so
// pointer deltas keep arriving however far the user moves. What a look control
// needs, and a mode rather than a per-event choice: the cursor either has a
// screen position or it does not.
DEFINE_WORK_FUNC_TYPED(Window, CaptureMouse, (int32_t, on))
{
    (void)ctx;
    self.SetMouseCapture(on != 0);   // logs the transition itself
}

DEFINE_WORK_FUNC(Window, PollEvents)
{
    (void)data;
    self.PollEvents();
    if (self.ShouldClose())
        window_closed(self, ctx, "PollEvents: window close requested");
}

DEFINE_WORK_FUNC(Window, Close)
{
    (void)data;
    window_closed(self, ctx, "Close: closed by the script");
}

DEFINE_WORK_FUNC(Window, Delete)
{
    (void)data;
    (void)ctx;
    self.DeleteConcrete();
}

/*
** Legacy test functions preserved for reference — not compiled.
** All MirrorBuffer usage here predates the typed makePair interface.
** Do not uncomment without updating to makePair<Strategy, Page> form.
*/

#endif
