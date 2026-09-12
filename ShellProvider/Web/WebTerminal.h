#ifndef WEBSHELL_TERMINAL_H__
#define WEBSHELL_TERMINAL_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// WebTerminal — browser / emscripten half of a shell.
//
// Same `lsh` seam as LinuxTerminal.h so ShellProvider.cc is platform-agnostic:
// claim_console / read_line / attach / history. No termios, no poll, no unix
// sockets -- emscripten's stdio (or the browser console under MAIN_MODULE)
// is the character source.
//
// pthread is available under -pthread; a Shell is still ThreadBase and may
// Detach child shells onto worker threads the same way the host does.
// ---------------------------------------------------------------------------

namespace lsh {

inline std::atomic<uint64_t> g_console_owner{0};

inline bool claim_console(uint64_t rid)
{
    uint64_t expected = 0;
    return g_console_owner.compare_exchange_strong(expected, rid,
                                                   std::memory_order_acq_rel);
}

inline void release_console(uint64_t rid)
{
    uint64_t expected = rid;
    g_console_owner.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}

inline bool owns_console(uint64_t rid)
{
    return g_console_owner.load(std::memory_order_acquire) == rid;
}

inline void load_history() {}
inline void save_history() {}

/*
 * Line editor: prompt + getline. Under node/emrun this is the process console;
 * under a browser MAIN_MODULE it is whatever emscripten wired to Module.print
 * / stdin. Interruptible via SignalContext between characters is not available
 * without a JS async input bridge -- we check the context once around the
 * blocking read so Halt still ends the navigator loop.
 */
inline std::string read_line(const std::string& prompt, ETCS::SignalContext ctx)
{
    if (ctx.isInterrupted() || ctx.isTerminated())
        return {};

    if (!prompt.empty())
    {
        std::cout << prompt << std::flush;
    }

    std::string line;
    if (!std::getline(std::cin, line))
    {
        // EOF / closed stdin -- navigator treats empty + caller "-" protocol
        return {};
    }
    return line;
}

inline void attach(const std::string& path, ETCS::SignalContext ctx)
{
    (void)ctx;
    // No unix-socket relay on the web path. Log and refuse cleanly.
    std::cerr << "[WebShell] attach not supported in browser: " << path << "\n";
}

} // namespace lsh

#endif // WEBSHELL_TERMINAL_H__
