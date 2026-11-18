#ifndef NETWORKPROVIDER_HTTPSWIRE_H__
#define NETWORKPROVIDER_HTTPSWIRE_H__
#include "../../../ontology.h"
#include "WebSocket.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

/*
 * HttpsWire -- HTTPS/1.1 as machinery, not a relation.
 *
 * The language a Foreign relation may speak (HttpsRelation); it knows nothing
 * of who is on the far side. One question, one answer, one connection
 * ("Connection: close"): dial -> TLS (ClientTlsIO, verified against a CA
 * bundle) -> request -> response, parsed for status and body (Content-Length,
 * chunked, or close-delimited).
 *
 * QUESTION "<METHOD> https://host[:port]/path[?query]" (a POST's body after the
 * first newline); ANSWER "<status> <body>". Secrets: {secret:NAME} in the path
 * or body is filled from the bound file at send, written only into the request
 * bytes; an unresolved name refuses the question. Callers log the question as
 * given, placeholder and all.
 */
class HttpsWire
{
public:
    bool SetCaFile(const std::string& path)
    {
        if (path.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        ca_file_ = path;
        return true;
    }

    bool Secret(const std::string& name, const std::string& path)
    {
        if (name.empty() || path.empty() || name.find_first_of("{}: ") != std::string::npos) return false;
        std::lock_guard<std::mutex> lock(mu_);
        secrets_[name] = path;
        return true;
    }

    bool Exchange(const std::string& question, std::string& answer, const ETCS::SignalContext& ctx) const
    {
        answer.clear();
#if defined(__EMSCRIPTEN__)
        (void)question; (void)ctx;
        ETCS_LOG("HttpsWire", "no foreign wire in a browser runtime.");
        return false;
#else
        Request req;
        if (!parseQuestion(question, req)) { ETCS_LOG("HttpsWire", "refused: '" << question << "'"); return false; }
        std::string wire;
        if (!wrapString(req, wire)) return false;
        std::string err;
        const int fd = dial(req.host, req.port, err);
        if (fd < 0) { ETCS_LOG("HttpsWire", "dial: " << err); return false; }
        etcs_ws::ClientTlsIO io(fd);   // owns fd from here
        std::string ca;
        { std::lock_guard<std::mutex> lock(mu_); ca = ca_file_; }
        if (!io.handshake(req.host, false, err, ca.c_str())) { ETCS_LOG("HttpsWire", err); return false; }
        static const std::atomic<bool> never{false};
        if (!io.writeAll(wire.data(), wire.size(), never)) { ETCS_LOG("HttpsWire", "write failed."); return false; }
        std::fill(wire.begin(), wire.end(), '\0');   // the filled secret, gone from this buffer
        std::string raw;
        if (!readResponse(io, raw, ctx)) { ETCS_LOG("HttpsWire", "no complete response."); return false; }
        if (!unwrapString(raw, answer)) { ETCS_LOG("HttpsWire", "refused: response did not parse."); return false; }
        return true;
#endif
    }

    // "https://host[:port]" -> host, port. For a relation fixing its stranger.
    static bool parseOrigin(const std::string& origin, std::string& host, int& port)
    {
        Request r;
        if (!parseQuestion("GET " + origin + "/", r) || r.target != "/") return false;
        host = r.host; port = r.port;
        return true;
    }

private:
    struct Request { std::string method, host, target, body; int port = 443; };

    static constexpr size_t kMaxResponse = 1u << 20;
    static constexpr int    kTimeoutMs   = 10000;

    // --- the ETCS side ---------------------------------------------------------

    static bool parseQuestion(const std::string& q, Request& r)
    {
        const size_t nl = q.find('\n');
        const std::string line = q.substr(0, nl);
        if (nl != std::string::npos) r.body = q.substr(nl + 1);
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) return false;
        r.method = line.substr(0, sp);
        std::string url = line.substr(sp + 1);
        while (!url.empty() && url.back() == ' ') url.pop_back();
        if (r.method.empty() || url.rfind("https://", 0) != 0) return false;
        for (char c : r.method) if (c < 'A' || c > 'Z') return false;
        const size_t slash = url.find('/', 8);
        const std::string hostport = url.substr(8, slash == std::string::npos ? std::string::npos : slash - 8);
        r.target = slash == std::string::npos ? "/" : url.substr(slash);
        const size_t colon = hostport.rfind(':');
        if (colon != std::string::npos)
        {
            r.host = hostport.substr(0, colon);
            r.port = std::atoi(hostport.c_str() + colon + 1);
        }
        else r.host = hostport;
        for (char c : r.target) if (c == ' ' || c == '\r' || c == '\n') return false;
        return !r.host.empty() && r.port > 0 && r.port < 65536;
    }

    // {secret:NAME} -> the file's first line. False on any unresolved name.
    bool fillSecrets(std::string& s) const
    {
        size_t at = 0;
        while ((at = s.find("{secret:", at)) != std::string::npos)
        {
            const size_t end = s.find('}', at);
            if (end == std::string::npos) return false;
            const std::string name = s.substr(at + 8, end - at - 8);
            std::string path;
            {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = secrets_.find(name);
                if (it == secrets_.end()) { ETCS_LOG("HttpsWire", "refused: no secret '" << name << "'."); return false; }
                path = it->second;
            }
            std::ifstream f(path);
            std::string value;
            if (!f || !std::getline(f, value)) { ETCS_LOG("HttpsWire", "refused: secret '" << name << "' unreadable."); return false; }
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
            if (value.empty()) return false;
            s.replace(at, end - at + 1, value);
            at += value.size();
            std::fill(value.begin(), value.end(), '\0');
        }
        return true;
    }

    bool wrapString(const Request& r, std::string& wire) const
    {
        std::string target = r.target, body = r.body;
        if (!fillSecrets(target) || !fillSecrets(body)) return false;
        const bool default_port = (r.port == 443);
        wire = r.method + " " + target + " HTTP/1.1\r\n"
             + "Host: " + r.host + (default_port ? std::string() : ":" + std::to_string(r.port)) + "\r\n"
             + "User-Agent: ETCS-HttpsWire\r\n"
             + "Accept: */*\r\n"
             + "Connection: close\r\n";
        if (!body.empty() || r.method == "POST")
            wire += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: "
                  + std::to_string(body.size()) + "\r\n";
        wire += "\r\n" + body;
        std::fill(target.begin(), target.end(), '\0');
        std::fill(body.begin(), body.end(), '\0');
        return true;
    }

    // --- the foreign side ------------------------------------------------------

    static std::string lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Header end, status, and whether the body is complete yet.
    struct Head { size_t end = 0; int status = 0; bool chunked = false; long long length = -1; };

    static bool parseHead(const std::string& raw, Head& h)
    {
        const size_t end = raw.find("\r\n\r\n");
        if (end == std::string::npos) return false;
        h.end = end + 4;
        if (raw.compare(0, 5, "HTTP/") != 0) return false;
        const size_t sp = raw.find(' ');
        if (sp == std::string::npos || sp + 4 > end) return false;
        h.status = std::atoi(raw.c_str() + sp + 1);
        if (h.status < 100 || h.status > 599) return false;
        const std::string head = lower(raw.substr(0, end));
        const size_t te = head.find("\r\ntransfer-encoding:");
        if (te != std::string::npos && head.find("chunked", te) < head.find("\r\n", te + 2)) h.chunked = true;
        const size_t cl = head.find("\r\ncontent-length:");
        if (cl != std::string::npos) h.length = std::atoll(head.c_str() + cl + 17);
        return true;
    }

    // Chunked body -> plain. False if incomplete or malformed.
    static bool dechunk(const std::string& in, std::string& out)
    {
        out.clear();
        size_t at = 0;
        while (true)
        {
            const size_t eol = in.find("\r\n", at);
            if (eol == std::string::npos) return false;
            char* stop = nullptr;
            const unsigned long n = std::strtoul(in.c_str() + at, &stop, 16);
            if (stop == in.c_str() + at) return false;
            at = eol + 2;
            if (n == 0) return true;
            if (at + n + 2 > in.size()) return false;
            out.append(in, at, n);
            at += n + 2;
        }
    }

    static bool unwrapString(const std::string& raw, std::string& answer)
    {
        Head h;
        if (!parseHead(raw, h)) return false;
        std::string body;
        if (h.chunked) { if (!dechunk(raw.substr(h.end), body)) return false; }
        else if (h.length >= 0)
        {
            if (raw.size() < h.end + static_cast<size_t>(h.length)) return false;
            body = raw.substr(h.end, static_cast<size_t>(h.length));
        }
        else body = raw.substr(h.end);
        answer = std::to_string(h.status) + " " + body;
        return true;
    }

#if !defined(__EMSCRIPTEN__)
    static bool complete(const std::string& raw)
    {
        Head h;
        if (!parseHead(raw, h)) return false;
        std::string scratch;
        if (h.chunked) return dechunk(raw.substr(h.end), scratch);
        if (h.length >= 0) return raw.size() >= h.end + static_cast<size_t>(h.length);
        return false;   // close-delimited: done when the peer closes
    }

    static bool readResponse(etcs_ws::ClientTlsIO& io, std::string& raw, const ETCS::SignalContext& ctx)
    {
        char buf[4096];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
        while (raw.size() < kMaxResponse)
        {
            if (ctx.isInterrupted() || ctx.isTerminated()) return false;
            const int r = io.readSome(buf, sizeof buf);
            if (r > 0) { raw.append(buf, static_cast<size_t>(r)); if (complete(raw)) return true; continue; }
            if (r < 0) return !raw.empty() && parseHeadOnly(raw);   // peer closed: close-delimited body ends here
            if (std::chrono::steady_clock::now() > deadline) return false;
            etcs_ws::wait_fd(io.fd(), POLLIN, 100);
        }
        return false;
    }
    static bool parseHeadOnly(const std::string& raw) { Head h; return parseHead(raw, h); }

    static int dial(const std::string& host, int port, std::string& err)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        { err = "cannot resolve " + host; return -1; }
        int fd = -1;
        for (addrinfo* a = res; a && fd < 0; a = a->ai_next)
        {
            fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            etcs_ws::set_nonblocking(fd);
            if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
            int soerr = 0; socklen_t sl = sizeof soerr;
            if (errno == EINPROGRESS && etcs_ws::wait_fd(fd, POLLOUT, kTimeoutMs)
                && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) break;
            ::close(fd); fd = -1;
        }
        ::freeaddrinfo(res);
        if (fd < 0) err = "cannot connect to " + host + ":" + std::to_string(port);
        return fd;
    }
#endif


private:
    mutable std::mutex                 mu_;
    std::string                        ca_file_ = "/etc/ssl/certs/ca-certificates.crt";
    std::map<std::string, std::string> secrets_;   // name -> file
};

#endif // NETWORKPROVIDER_HTTPSWIRE_H__
