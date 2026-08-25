#ifndef TARPITFILTER_H__
#define TARPITFILTER_H__
#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <vector>

// TarpitFilter — a plain pattern list, deliberately NOT an ontology type
// (no Entity, no arena, nothing WIRE_TYPE_IDENTITY needs to know about).
// It exists purely to answer one question -- "does this request path look
// like a bot/vuln scan rather than a real request?" -- and that question is
// answerable with a string and a vector, so it gets exactly that much
// machinery and no more.
//
// Matching happens against the PARSED REQUEST PATH, which askFilter's own
// probe has never carried on its own (ConnectionManager.h's Filter_ dispatch
// has always run at accept time, before a single byte is read). This type
// stays deliberately ignorant of that -- it just answers Matches(path) -- so
// TarpitNode (TarpitNode.h) is what bridges the two: it IS wired in as a
// gate-level Filter_, and ConnectionManager's own conditional pre-parse
// (preParseThenDispatch, gated on a filtered subscriber existing at all --
// see ConnectionManager.h) is what gives its AcceptsConcrete a parsed
// request to hand this class before askFilter ever runs.
//
// Case-insensitive SUBSTRING match against the whole path, deliberately not
// an exact-suffix or segment-aware parse: the point is to catch the SHAPE
// of a scan ("wp-content/plugins/x/wp-load.php", ".env.bak", a nested
// ".git/config") without having to enumerate every path a scanner actually
// tries. Every default pattern below was checked against this site's own
// real paths (/, /chess.html, /forum.html, /forum/<mount>/..., navigation
// under /whitepaper.html and /philosophy.html, /favicon/*) and none of
// them collide. Deliberately excludes .well-known/ -- that prefix is where
// legitimate clients (ACME certificate renewal, a security researcher
// reading security.txt) are SUPPOSED to look, and tarpitting it would
// punish exactly the traffic a site wants to answer normally.
class TarpitFilter
{
public:
    TarpitFilter() { LoadDefaultPatterns(); }

    void AddPattern(const std::string& pattern)
    {
        if (pattern.empty()) return;
        patterns_.push_back(lower(pattern));
    }

    // Empties the list outright -- a script that wants a from-scratch list
    // calls this first, then AddPattern for each of its own. Does NOT
    // repopulate the defaults; see LoadDefaultPatterns for that.
    void ClearPatterns() { patterns_.clear(); }

    // Restores the built-in list. Separate from the constructor's own call
    // to it only so a script can explicitly reset to defaults after having
    // cleared and customized -- ClearPatterns alone does not bring these
    // back, on purpose: a script that asked for empty presumably wants
    // empty until it says otherwise.
    void LoadDefaultPatterns()
    {
        static const char* const kDefaults[] = {
            // credentials / secrets
            ".env", ".env.bak", ".env.local", ".aws/credentials", ".aws/config",
            "id_rsa", ".ssh/", ".htpasswd",
            // PHP / WordPress -- this server is neither, so anything shaped
            // like either is a scan, never a real request.
            ".php", "wp-admin", "wp-login", "wp-content", "wp-includes",
            "xmlrpc.php", "phpmyadmin", "phpunit", "eval-stdin",
            // version control / editor droppings that should never be served
            ".git/", ".svn/", ".idea/", ".vscode/sftp.json", ".ds_store",
            // framework/server introspection endpoints this server has none of
            "server-status", "server-info", "actuator", "telescope",
            "_profiler", "cgi-bin/", "boaform/", "owa/auth",
            // generic admin/config/shell probes
            "config.php", "configuration.php", "settings.php", ".htaccess",
            "admin.php", "login.php", "shell.php", "webshell",
        };
        // Through lower(), not a raw assign -- every entry above is already
        // lowercase, so this changes nothing today, but it means a future
        // addition to kDefaults can't silently bypass the same
        // case-folding AddPattern always applies.
        patterns_.clear();
        patterns_.reserve(std::size(kDefaults));
        for (const char* p : kDefaults) patterns_.push_back(lower(p));
    }

    bool Matches(const std::string& path) const
    {
        const std::string hay = lower(path);
        for (const std::string& p : patterns_)
            if (hay.find(p) != std::string::npos) return true;
        return false;
    }

    size_t PatternCount() const { return patterns_.size(); }

private:
    static std::string lower(const std::string& s)
    {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::vector<std::string> patterns_;
};

#endif // TARPITFILTER_H__
