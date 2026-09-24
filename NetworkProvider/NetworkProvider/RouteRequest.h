#ifndef ROUTEREQUEST_H__
#define ROUTEREQUEST_H__
#include "../../../ontology.h"
#include <cstring>
#include <string>
#include <vector>

// ── What a route is handed, and what it may hand back ────────────────────────
//
// Two shapes, both of them answers to the same discovery: a work function's
// payload is an ETCS::Buffer, the tag buffer is 256 bytes, and that ceiling is
// correct for what it is for -- POD and RIDs across an ABI boundary -- and
// hopeless as the place a request or a response actually lives.
//
// Before this, a route saw a path string and answered with a string in the same
// buffer, so:
//
//   * a route could not see the METHOD or the BODY. HTTPParser has had both all
//     along (method_, and the body slice at header_len); nothing forwarded them,
//     so every node in the tree was a GET-only surface whose whole vocabulary
//     had to fit in a URL.
//   * a route could not answer with more than 255 bytes, and not by truncation
//     either: writeString(const char*) resets FIRST and then refuses a string
//     that will not fit, so the 256th byte turns the whole answer into an empty
//     body. ChessNode::roomsLocked emits about fifteen bytes a match, so a node
//     silently stops answering `rooms` at all somewhere past the seventeenth --
//     latent today only because nobody has run seventeen matches.
//
// RouteRequest fixes the first by being the thing passed instead of a path, and
// RouteRef fixes the second by putting a REFERENCE in the buffer rather than the
// bytes. The buffer goes back to carrying POD, which is what it is good at.
//
// NEITHER OWNS ANYTHING. Both are views, alive for one dispatch, and the
// lifetime contract is the one HtmlPage_::ResolvedAsset already has: the bytes
// belong to whoever answered and must outlive the send. HttpServer::Serve keeps
// its own route_body for exactly that reason, and a route answering by
// reference is making the same promise a FileHtmlPage makes about its mounted
// file -- with the same consequence for breaking it.

// ── RouteRequest ─────────────────────────────────────────────────────────────
//
// The segments are split ONCE, here, instead of by each node in its own way.
// ChessNode::requestLocked, TarpitNode and anything after them were each
// re-deriving "what are the parts of this path" from the string, which is three
// answers to one question and three places for the reserved-word checks to
// disagree about what a trailing slash means.
//
// Mount is seg 0 by convention and not by enforcement: a node still checks its
// own, because a server can carry several mounts and only the node knows which
// one is its.
struct RouteRequest
{
    std::string method;                 // "GET", "POST", ... verbatim
    std::string target;                 // query stripped, LEADING SLASH INTACT
    std::string path;                   // target with leading slashes removed
    std::string query;                  // after '?', undecoded, usually empty
    std::vector<std::string> seg;       // path split on '/', empties dropped

    // TWO SPELLINGS OF ONE PATH, and the difference is not cosmetic. A page
    // tree resolves the address the browser asked for, slash and all
    // (FileHtmlPage::Resolve matches mounted keys that begin with one); a node
    // parses segments, where a leading empty segment is noise every one of them
    // was independently skipping. Serving `target` to the tree and handing
    // `path` to routes is what keeps both true at once -- passing the stripped
    // form to ResolvePath 404s every static file on the site.

    // The request body, pointing into the connection's own accumulator. Empty
    // for a GET; for a POST it is the whole entity the headers declared, since
    // the parser is not Complete before that much has arrived (PicoHTTPParser::
    // FeedRaw). Bounded by the accumulator, ETCS_NETWORK_MAX_HEADER_SIZE.
    const char* body     = nullptr;
    size_t      body_len = 0;

    // WHERE A ROUTE'S ANSWER LIVES: with the request, which is a stack object
    // in HttpServer::Serve and outlives the copy into the send buffer. A route
    // that answers by reference (RouteRef) points at this. It used to point at
    // a member of its own, "held until the next request" -- and the next
    // request came on another connection, on another thread, while this one
    // was still being copied out, so a read of a session answered with the
    // first half of one reply and the freed bytes of the next.
    mutable std::string reply;

    size_t count() const { return seg.size(); }

    // Out of range is empty rather than undefined: every caller is parsing an
    // attacker-supplied path, and a bounds check per access at every call site
    // is a check somebody eventually forgets.
    std::string at(size_t i) const { return i < seg.size() ? seg[i] : std::string(); }

    bool is(size_t i, const char* lit) const
    {
        return lit && i < seg.size() && seg[i] == lit;
    }

    // The tail from i, re-joined. For a verb whose argument is itself a path.
    std::string from(size_t i) const
    {
        std::string out;
        for (size_t k = i; k < seg.size(); ++k)
        {
            if (!out.empty()) out += '/';
            out += seg[k];
        }
        return out;
    }

    bool posted() const { return method == "POST"; }

    std::string bodyString() const
    {
        return (body && body_len) ? std::string(body, body_len) : std::string();
    }

    static void Split(const std::string& p, std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (char c : p)
        {
            if (c == '/') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
    }

    // `target` is the request target as picohttpparser hands it over: path with
    // the query still attached. Split here rather than at the call site so the
    // two halves cannot drift apart -- Serve used to strip the query for its own
    // consumers and throw it away, which was fine while nothing read parameters
    // and is not a decision worth re-making per node.
    static RouteRequest Parse(const std::string& target, const std::string& method,
                              const char* body, size_t body_len)
    {
        RouteRequest r;
        r.method   = method;
        r.body     = body;
        r.body_len = body_len;

        const size_t q = target.find('?');
        if (q == std::string::npos) r.target = target;
        else { r.target = target.substr(0, q); r.query = target.substr(q + 1); }

        r.path = r.target;
        while (!r.path.empty() && r.path.front() == '/') r.path.erase(0, 1);
        Split(r.path, r.seg);
        return r;
    }
};

// ── RouteRef ─────────────────────────────────────────────────────────────────
//
// A reference answer, as sixty-four bytes of POD in the payload buffer.
//
// The discriminator is a magic word CONTAINING A NUL, plus an exact size check,
// and both halves are load-bearing. A route answering with text uses
// writeString, which stops at the first NUL -- so no string answer can ever
// produce these bytes, whatever a caller puts in it. Without the NUL, a route
// echoing back an attacker-chosen string could hand HttpServer a pointer.
struct RouteRef
{
    static constexpr size_t MIME_MAX = 40;
    static constexpr size_t FRAME    = 8 + sizeof(uint64_t) * 2 + MIME_MAX;

    const char* data   = nullptr;
    size_t      length = 0;
    char        mime[MIME_MAX] = {};

    static const char* Magic() { return "ETCSREF\0"; }   // eight bytes, NUL last

    // Called by the ROUTE, on the payload it was handed. Replaces whatever was
    // in it -- a route answers one way or the other, never both.
    static bool Emit(ETCS::Buffer& io, const void* data, size_t length,
                     const char* mime = "text/plain")
    {
        if (!data && length) return false;
        io.reset();
        const uint64_t p = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
        const uint64_t n = static_cast<uint64_t>(length);
        char m[MIME_MAX] = {};
        if (mime) std::strncpy(m, mime, MIME_MAX - 1);
        if (!io.writeRaw(Magic(), 8))         return false;
        if (!io.writeRaw(&p, sizeof(p)))      return false;
        if (!io.writeRaw(&n, sizeof(n)))      return false;
        if (!io.writeRaw(m, MIME_MAX))        return false;
        return true;
    }

    // Called by HttpServer on the way back. False means "this is an ordinary
    // inline answer", which is the common case and not an error.
    static bool Read(const ETCS::Buffer& io, RouteRef& out)
    {
        if (io.written != FRAME) return false;
        if (std::memcmp(io.buf, Magic(), 8) != 0) return false;
        uint64_t p = 0, n = 0;
        std::memcpy(&p, io.buf + 8, sizeof(p));
        std::memcpy(&n, io.buf + 8 + sizeof(p), sizeof(n));
        std::memcpy(out.mime, io.buf + 8 + sizeof(p) + sizeof(n), MIME_MAX);
        out.mime[MIME_MAX - 1] = '\0';
        out.data   = reinterpret_cast<const char*>(static_cast<uintptr_t>(p));
        out.length = static_cast<size_t>(n);
        if (!out.data && out.length) return false;
        return true;
    }
};

#endif
