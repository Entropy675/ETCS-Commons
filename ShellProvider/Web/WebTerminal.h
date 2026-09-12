#ifndef WEBSHELL_TERMINAL_H__
#define WEBSHELL_TERMINAL_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

// ---------------------------------------------------------------------------
// WebTerminal — browser / emscripten half of a shell.
//
// Same `lsh` seam as LinuxTerminal.h so ShellProvider.cc is platform-agnostic:
// claim_console / read_line / attach / history.
//
// Browser I/O is not termios. The host page (scripts/www/index.html) owns a
// terminal widget and:
//   - routes Module.print / printErr into the widget
//   - pushes completed lines into C++ via etcs_web_shell_push_line()
// read_line waits on that queue (interruptible between waits). Output that
// bypasses iostream can call lsh::term_write so the same widget sees it.
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

#if defined(__EMSCRIPTEN__)

inline std::mutex& line_mu()
{
    static std::mutex m;
    return m;
}
inline std::condition_variable& line_cv()
{
    static std::condition_variable cv;
    return cv;
}
inline std::deque<std::string>& line_q()
{
    static std::deque<std::string> q;
    return q;
}

// Visible to the page: append text to the terminal widget if present.
inline void term_write(const char* s)
{
    if (!s) return;
    EM_ASM({
        var t = UTF8ToString($0);
        if (typeof window.etcsTermWrite === 'function')
            window.etcsTermWrite(t);
        else if (typeof console !== 'undefined')
            console.log(t);
    }, s);
}

inline void term_write(const std::string& s) { term_write(s.c_str()); }

inline void push_line(std::string line)
{
    {
        std::lock_guard<std::mutex> g(line_mu());
        line_q().push_back(std::move(line));
    }
    line_cv().notify_one();
}

/*
 * Line editor: prompt into the page terminal, then wait for a line from JS.
 * Interruptible between wait intervals so Halt still ends the navigator.
 */
inline std::string read_line(const std::string& prompt, ETCS::SignalContext ctx)
{
    if (ctx.isInterrupted() || ctx.isTerminated())
        return {};

    if (!prompt.empty())
    {
        term_write(prompt);
        // Also stdio for Module.print redirection / node.
        std::cout << prompt << std::flush;
    }

    for (;;)
    {
        if (ctx.isInterrupted() || ctx.isTerminated())
            return {};

        std::unique_lock<std::mutex> lock(line_mu());
        if (line_cv().wait_for(lock, std::chrono::milliseconds(50),
                               [] { return !line_q().empty(); }))
        {
            std::string line = std::move(line_q().front());
            line_q().pop_front();
            return line;
        }
    }
}

#else // native fallback (node without page, or accidental non-emscripten include)

inline void term_write(const char* s)
{
    if (s) std::cout << s << std::flush;
}
inline void term_write(const std::string& s) { term_write(s.c_str()); }

inline std::string read_line(const std::string& prompt, ETCS::SignalContext ctx)
{
    if (ctx.isInterrupted() || ctx.isTerminated())
        return {};

    if (!prompt.empty())
        std::cout << prompt << std::flush;

    std::string line;
    if (!std::getline(std::cin, line))
        return {};
    return line;
}

#endif // __EMSCRIPTEN__

inline void attach(const std::string& path, ETCS::SignalContext ctx)
{
    (void)ctx;
    std::cerr << "[WebShell] attach not supported in browser: " << path << "\n";
#if defined(__EMSCRIPTEN__)
    term_write(std::string("[WebShell] attach not supported: ") + path + "\n");
#endif
}

} // namespace lsh

#if defined(__EMSCRIPTEN__)
// Called from the page (Module._etcs_web_shell_push_line / ccall).
extern "C" {

__attribute__((used, visibility("default")))
void etcs_web_shell_push_line(const char* line)
{
    if (!line) return;
    lsh::push_line(std::string(line));
}

__attribute__((used, visibility("default")))
void etcs_web_shell_write(const char* text)
{
    lsh::term_write(text ? text : "");
}

} // extern "C"
#endif

#endif // WEBSHELL_TERMINAL_H__
