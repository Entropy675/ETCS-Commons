#ifndef LINUXTERMINAL_H__
#define LINUXTERMINAL_H__

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <cstdint>

// ---------------------------------------------------------------------------
// LinuxTerminal — the tty half of a shell, and nothing else.
//
// Raw mode, history, tab completion, the attach relay. All of it used to sit in
// ShellREPL.h behind #ifdef ETCS_REPL_SHELL, one #if defined(_WIN32) fork per
// function, wedged beside core because there was no provider to own a terminal.
// This file is the Linux backend of a provider, so the forks are simply gone:
// a WinTerminal.h would be the other branch, selected once by
// Contract_ShellProvider.h rather than twelve times by the preprocessor.
//
// WHAT THIS FILE IS ALLOWED TO KNOW. It reads keystrokes and returns lines. It
// does not know what a module is, what an entity is, or what a line means --
// the navigator asks for a line and this answers, which is the seam
// ReplLineSource named long before the terminal had anywhere else to live.
//
// HOW THE LOADER REACHES THESE. Through ordinary dispatch, and through nothing
// else. ShellProvider.cc wraps them as four actions on the Shell tag --
// ReadLine, Attach, OpenConsole, CloseConsole -- so the loader calls
// `shell->call("Shell.ReadLine", data, sig)` exactly as a script calls any
// other action. No export, no dlsym, no registry, no hook: a terminal is
// something a Shell DOES, and the dispatch table already carries what a Shell
// does.
//
// The buffer is in/out (WorkFunc takes `Buffer&`) and the SignalContext crosses
// by value, which is what makes a prompt-in/line-out call the natural shape
// rather than an imposed one. The one-byte status protocol on the reply is
// documented at THE SHELL SEAM, core/CommandExecutor.h.
//
// A header-inline global could not have carried it in either direction: under
// -Bsymbolic each DSO resolves its own copy, so a module assigning "the"
// terminal pointer writes into itself and the loader goes on reading null. The
// node the loader receives is the one address both sides share.
// ---------------------------------------------------------------------------

namespace lsh {

namespace fs = std::filesystem;

// ── Who holds stdin ─────────────────────────────────────────────────────────
//
// A process has ONE console, and a Shell is an ordinary entity of which there
// can be many -- every detached script is one. So the console is claimed, by
// RID, and a second Shell asking to read gets refused rather than racing the
// first for keystrokes.
//
// Not a security boundary and not pretending to be one: it is the same kind of
// statement Pixels_ makes about owning its buffer. The loader already knows
// which Shell is driving the console (repl_console_shell); this is the module's
// own half of that fact, so a shell that was never handed the console cannot
// take it by asking.
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

// ── Character source ────────────────────────────────────────────────────────
//
// Non-TTY branch polls with a timeout instead of blocking on read().
// Deliberately local rather than clearing SA_RESTART process-wide -- other
// blocking calls (network I/O inside a `run` script) depend on auto-retry to
// forward an interrupt to the executing context rather than aborting on the
// first EINTR. A TTY never needed this: its line discipline already wakes a
// blocked read on SIGINT.
//
// Returns 0 on timeout AND on read failure/closed pipe (the sleep avoids
// spinning on a dead pipe, which poll() reports ready forever via POLLHUP).
// Callers treat 0 as "no character, re-check signals".
inline char get_char()
{
    char buf = 0;
    if (!isatty(STDIN_FILENO))
    {
        struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
        int pr = poll(&pfd, 1, 100); // 100ms
        if (pr <= 0) return 0;
        ssize_t n = read(STDIN_FILENO, &buf, 1);
        if (n <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 0;
        }
        return buf;
    }
    struct termios old;
    std::memset(&old, 0, sizeof(struct termios));
    if (tcgetattr(STDIN_FILENO, &old) < 0) return 0;
    struct termios raw = old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return 0;
    if (read(STDIN_FILENO, &buf, 1) < 0) buf = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return buf;
}

// ── History ─────────────────────────────────────────────────────────────────
//
// Loaded on Open rather than at process start, which is where it belongs and
// was not: shell_startup() used to read .etcs_history in every interactive
// binary whether or not a prompt was ever shown.
inline std::vector<std::string> g_history;
inline const std::string HISTORY_FILE = ".etcs_history";
static constexpr size_t MAX_HISTORY_BYTES = 16 * 1024;

inline void prune_history()
{
    size_t total = 0;
    for (const auto& line : g_history) total += line.size() + 1;
    size_t drop_from_front = 0;
    while (total > MAX_HISTORY_BYTES && drop_from_front < g_history.size())
    {
        total -= g_history[drop_from_front].size() + 1;
        ++drop_from_front;
    }
    if (drop_from_front > 0)
        g_history.erase(g_history.begin(),
                        g_history.begin() + static_cast<long>(drop_from_front));
}

inline void load_history()
{
    g_history.clear();
    std::ifstream hfile(HISTORY_FILE);
    std::string line;
    while (std::getline(hfile, line))
        if (!line.empty()) g_history.push_back(line);
    prune_history();
}

inline void save_history()
{
    prune_history();
    std::ofstream hfile(HISTORY_FILE);
    for (const auto& line : g_history) hfile << line << "\n";
}

// ── Completion ──────────────────────────────────────────────────────────────
//
// Local copies rather than reaching for CommandExecutor.h's: completion is
// about what is on the disk in front of THIS terminal, and a shell's tab key
// has no business depending on the executor's notion of anything.
inline bool is_module_file(const fs::directory_entry& entry)
{
    auto ext = entry.path().extension().string();
    return (ext == ".so" || ext == ".dll" || ext == ".dylib");
}

inline bool iequals_prefix(const std::string& full, const std::string& partial)
{
    if (partial.size() > full.size()) return false;
    return std::equal(partial.begin(), partial.end(), full.begin(),
                      [](char a, char b) {
                          return std::tolower((unsigned char)a)
                              == std::tolower((unsigned char)b);
                      });
}

// ── One line ────────────────────────────────────────────────────────────────
//
// Takes the SignalContext BY VALUE, which is what lets this be reached through
// a plain function pointer from the other side of a dlopen: a SignalContext is
// a set of pointers the runtime already copies across that edge in every
// WorkFunc.
inline std::string read_line(const std::string& prompt, ETCS::SignalContext ctx)
{
    std::string input;
    std::cout << prompt << std::flush;

    std::vector<std::string> current_matches;
    int cycle_index   = -1;
    int history_index = static_cast<int>(g_history.size());
    std::string original_partial, prefix_before_partial;

    auto refresh_line = [&](const std::string& new_text) {
        std::cout << "\r" << std::string(prompt.size() + input.size() + 5, ' ') << "\r";
        std::cout << prompt << new_text << std::flush;
        input = new_text;
    };

    while (true)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        int c = get_char();
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        if (c == 27)
        {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0)
            {
                if (seq[0] == '[' && seq[1] == 'A' && history_index > 0)
                    { history_index--; refresh_line(g_history[history_index]); }
                else if (seq[0] == '[' && seq[1] == 'B')
                {
                    if (history_index < (int)g_history.size() - 1)
                        { history_index++; refresh_line(g_history[history_index]); }
                    else
                        { history_index = static_cast<int>(g_history.size()); refresh_line(""); }
                }
            }
            continue;
        }
        // 0 is a poll timeout (routinely, ~every 100ms while idle) or a read
        // failure -- never a keystroke. Looping back re-checks the signals at
        // the top; falling through would append a NUL on every idle cycle.
        if (c == 0) continue;

        if (c == '\r' || c == '\n')
        {
            std::cout << std::endl;
            if (!input.empty() && (g_history.empty() || input != g_history.back()))
            {
                g_history.push_back(input);
                save_history();
            }
            break;
        }
        else if (c == 8 || c == 127)
        {
            if (!input.empty()) { input.pop_back(); std::cout << "\b \b" << std::flush; }
            cycle_index = -1;
        }
        else if (c == '\t')
        {
            if (cycle_index == -1)
            {
                size_t last_space = input.find_last_of(" \t");
                prefix_before_partial = (last_space == std::string::npos)
                    ? "" : input.substr(0, last_space + 1);
                original_partial = (last_space == std::string::npos)
                    ? input : input.substr(last_space + 1);
                current_matches.clear();
                std::vector<std::string> exact, case_insens;
                try {
                    for (const auto& entry : fs::directory_iterator("."))
                    {
                        std::string name = entry.path().filename().string();
                        std::string cand = entry.is_directory()
                            ? name + "/"
                            : (is_module_file(entry) ? entry.path().stem().string() : name);
                        if (cand.size() >= original_partial.size()
                            && cand.substr(0, original_partial.size()) == original_partial)
                            exact.push_back(cand);
                        else if (iequals_prefix(cand, original_partial))
                            case_insens.push_back(cand);
                    }
                } catch (...) {}
                current_matches = exact.empty() ? case_insens : exact;
                std::sort(current_matches.begin(), current_matches.end());
                if (!current_matches.empty()) cycle_index = 0;
            }
            else
                cycle_index = (cycle_index + 1) % static_cast<int>(current_matches.size());

            if (cycle_index != -1)
            {
                size_t word_len = input.size() - prefix_before_partial.size();
                for (size_t i = 0; i < word_len; ++i) std::cout << "\b \b";
                input = prefix_before_partial + current_matches[cycle_index];
                std::cout << current_matches[cycle_index] << std::flush;
            }
        }
        else
        {
            input += static_cast<char>(c);
            std::cout << static_cast<char>(c) << std::flush;
            cycle_index = -1;
        }
    }
    return input;
}

// ── Attach ──────────────────────────────────────────────────────────────────
// attach <socket> -- drive ANOTHER runtime's control socket from this one.
//
// The far end is an `etcs --listen <socket>` process. A session there IS the
// navigator, from the moment it connects -- there is no line-interpreter mode
// to opt out of any more, because the browse surface no longer executes trace
// lines at all. So what arrives over this link is that runtime's own menus,
// rendered where the entity graph is and sent here as text.
//
// IT LIVES HERE, NOT IN CORE, and the reason is exact: the relay borrows this
// process's own line editor. Everything it needs is a terminal, and the only
// thing it hands back is "the operator typed detach". A runtime with no
// terminal has no attach verb, which is now a runtime fact rather than a build
// one (the navigator's `attach` branch checks ETCS::g_shell_attach).
//
// FOREGROUND, one at a time -- the prompt itself is the thing being lent out.
// The REMOTE's prompt is displayed; this side prints none of its own, because
// the far runtime's position in its own menus is the authority on where input
// is landing. Local escape word is `detach`; everything else forwards verbatim,
// including `exit`, which ends the REMOTE session rather than this one.
inline bool attach(const std::string& path, ETCS::SignalContext sig)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::cerr << "attach: socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        std::cerr << "attach: path too long (" << path.size()
                  << " >= " << sizeof(addr.sun_path) << ")\n";
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << "attach: connect('" << path << "') failed: "
                  << std::strerror(errno)
                  << " -- is that runtime running with --listen?\n";
        ::close(fd);
        return false;
    }

    // Same 300ms wake-up the far side uses, for the same reason: a quiet peer
    // must not park this loop past a Ctrl+C.
    struct timeval tv { 0, 300000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto send_all = [fd](const std::string& s) -> bool
    {
        size_t off = 0;
        while (off < s.size())
        {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    };

    // Waits for the remote's PROMPT, not merely for the socket to go quiet. A
    // slow command emits output in bursts with gaps between them, and stopping
    // at the first gap would print half a response and then hand back an input
    // line the remote isn't ready for -- with the rest of its output arriving
    // on top of whatever you typed next. Returns false when the peer closes.
    auto pump_until_prompt = [&]() -> bool
    {
        std::string acc;
        char buf[4096];
        while (true)
        {
            if (sig.isInterrupted() || sig.isTerminated()) return false;
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n == 0) return false;                       // peer closed
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;                               // still working -- keep waiting
                return false;
            }
            std::cout << std::string(buf, static_cast<size_t>(n)) << std::flush;
            acc.append(buf, static_cast<size_t>(n));
            if (acc.size() >= 2 && acc.compare(acc.size() - 2, 2, "> ") == 0)
                return true;
        }
    };

    ETCS_LOG("Shell", "attached to " << path
             << " -- 'detach' returns here, 'exit' ends the remote session.");

    if (!pump_until_prompt())
    {
        std::cerr << "attach: remote closed immediately.\n";
        ::close(fd);
        return false;
    }

    while (!(sig.isInterrupted() || sig.isTerminated()))
    {
        // Empty local prompt -- the remote printed its own just above.
        std::string line = read_line("", sig);
        if (sig.isInterrupted() || sig.isTerminated()) break;
        if (line == "detach") break;

        if (!send_all(line + "\n"))
        {
            std::cerr << "attach: send failed -- remote gone.\n";
            break;
        }
        if (!pump_until_prompt())
        {
            ETCS_LOG("Shell", "attach: remote session ended.");
            break;
        }
    }

    ::close(fd);
    ETCS_LOG("Shell", "detached from " << path);
    return true;
}

} // namespace lsh

#endif // LINUXTERMINAL_H__
