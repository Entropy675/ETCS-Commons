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
}

DEFINE_STREAM_FUNC_PRODUCE(Window, ProduceEvents)
{
    (void)data;

    while (!self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) return;
        std::this_thread::yield();
    }

    uint8_t id = self.RegisterObserver();
    ETCS_LOG("ProduceEvents", "Registered observer id: " << (int)id);
    if (id == INPUT_INVALID_OBSERVER) return;

    ETCS::Buffer slot;
    bool stream_alive = true;

    while (self.IsActive() && stream_alive)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        bool emitted = false;
        while (self.ReadNextRingEvent(id, slot))
        {
            InputEvent ev{};
            slot.readRaw(&ev, sizeof(InputEvent));
            ETCS_LOG("ProduceEvents", "Emitting: key=" << ev.key 
                        << " action=" << (int)ev.action);

            if (!stream.writeRaw(slot))
            {
                ETCS_LOG("ProduceEvents", "writeRaw failed — stream closed");
                stream_alive = false;
                break;
            }
            emitted = true;
        }

        if (id == INPUT_INVALID_OBSERVER)
            id = self.RegisterObserver();

        if (!emitted)
            std::this_thread::yield();
    }

    if (stream.isOpen())
        stream.closeWrite();

    self.UnregisterObserver(id);
}

DEFINE_STREAM_FUNC_CONSUME(Window, ConsumeEvents)
{
    (void)data;
    (void)self;

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.key == 0 && ev.action == 0) continue;

        if (ev.action == 1)
        {
            ETCS_LOG("ConsumeEvents", "KEY DOWN: " << ev.key 
                        << " (" << (char)ev.key << ")");
        }
        else
        {
            ETCS_LOG("ConsumeEvents", "KEY UP:   " << ev.key 
                        << " (" << (char)ev.key << ")");
        }
    }
    
    ETCS_LOG("ConsumeEvents", "Stream is closed.");
}

DEFINE_WORK_FUNC_TYPED(Window, Run, (uint32_t, x), (uint32_t, y), (std::string, title))
{
    if (!x || !y) { ETCS_LOG("Run", "Invalid window size."); return; }

    ETCS_LOG("Run", "Creating window: " << title << " (" << x << "x" << y << ")");
    self.CreateWindow(title.c_str(), x, y);

    if (!self.IsActive())
    {
        ETCS_LOG("Run", "Window creation failed.");
        if (ctx.interrupt) *ctx.interrupt = 1;
        return;
    }

    ETCS_LOG("Run", "Entering poll loop on calling thread.");

    while (self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        self.PollEvents();

        if (self.ShouldClose())
        {
            ETCS_LOG("Run", "Window close requested -- signalling interrupt.");
            if (ctx.interrupt) *ctx.interrupt = 1;
            break;
        }
    }

    self.CloseWindow();
    ETCS_LOG("Run", "Poll loop exited, window closed.");
}

DEFINE_WORK_FUNC(Window, PollEvents)
{
    (void)data;
    self.PollEvents();
    if (self.ShouldClose())
    {
        if (ctx.interrupt) *ctx.interrupt = 1;
        self.CloseWindow();
    }
}

DEFINE_WORK_FUNC(Window, Close)
{
    (void)data;
    (void)ctx;
    self.CloseWindow();
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
