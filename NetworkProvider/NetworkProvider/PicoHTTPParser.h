#ifndef PICOHTTPPARSER_H__
#define PICOHTTPPARSER_H__

#include "../../../ontology.h"
#include <iostream>
#include <cstring>
#include "../picohttpparser/picohttpparser.h"

class PicoHTTPParser : 
    public ParserBase<PicoHTTPParser>, public DeletableBase<PicoHTTPParser>
{
public:

    WIRE_TYPE_IDENTITY(PicoHTTPParser);

    enum class Mode : uint8_t
    {
        Request,
        Response
    };

    enum class State : uint8_t
    {
        Idle,
        Parsing,
        Complete,
        Error
    };

    // ETCS_NETWORK_MAX_HEADER_SIZE = 8192*8, see ETCS_API definitions.
    // Public so an owner embedding this as a plain member (rather than a
    // standalone spawned entity -- see bindAccumBuffer's own comment)
    // can size its own arena allocation to match exactly.
    static constexpr size_t kAccumCapacity = ETCS_NETWORK_MAX_HEADER_SIZE;

private:
    const char* method_      = nullptr;
    size_t      method_len_  = 0;
    const char* path_        = nullptr;
    size_t      path_len_    = 0;
    int         minor_ver_   = 0;

    int         status_      = 0;
    const char* msg_         = nullptr;
    size_t      msg_len_     = 0;

    struct phr_header headers_[32];
    size_t            num_headers_  = 0;
    State             state_        = State::Idle;
    Mode              mode_         = Mode::Request;

    // Raw accumulation buffer — picohttpparser needs the full header block.
    //
    // A pointer, not an inline array -- see bindAccumBuffer's own comment
    // for why. The DEFAULT constructor below self-allocates this from
    // getArena() (this instance's OWN local_arena_, correctly rooted the
    // ordinary way for a genuinely standalone PicoHTTPParser -- e.g. one
    // spawned directly via `spawn NetworkProvider::HTTPParser`). An owner
    // embedding this as a plain composed MEMBER (never addTag<T>'d in its
    // own right -- SocketConnectionState's own parser_ is exactly this
    // case) should call bindAccumBuffer() immediately after construction
    // to REPLACE this default allocation with one drawn from the OWNER's
    // own, correctly-nested arena instead: a plain member's own
    // local_arena_ is never individually reclaimed on its own (nothing
    // ever addTag<T>'s it, so nothing ever explicitly tears it down per-
    // instance -- it only gets swept whenever whatever arena IT happens
    // to have been constructed under eventually tears down in full), so
    // anything self-allocated into it stays leaked for as long as that
    // outer arena lives, regardless of this object's own much shorter
    // lifetime.
    char*  accum_        = nullptr;
    size_t accum_len_    = 0;
    size_t prev_len_     = 0;

public:
    // Was private and unreferenced. HttpServer::Serve needs it: without a
    // reader, every response said Connection: close and the client burned a
    // TCP connection per request.
    bool isPersistentConnection() const 
    {
        // Logic: HTTP/1.1 is persistent by default unless "Connection: close" is present
        // You can iterate through headers_ to check for "Connection" : "close"
        for (size_t i = 0; i < num_headers_; ++i) {
            if (std::string_view(headers_[i].name, headers_[i].name_len) == "Connection") {
                if (std::string_view(headers_[i].value, headers_[i].value_len) == "close")
                    return false;
            }
        }
        return (minor_ver_ == 1);
    }

public:

    PicoHTTPParser()
    {
        accum_ = static_cast<char*>(getArena().allocateRaw(static_cast<long long>(kAccumCapacity)));
        std::memset(accum_, 0, kAccumCapacity);
    }
    virtual ~PicoHTTPParser() = default;

    // bindAccumBuffer — see accum_'s own comment above for the full
    // reasoning. buf must be at least kAccumCapacity bytes, already
    // owned by (and reclaimed alongside) whatever arena the CALLER
    // actually wants this buffer's lifetime tied to. Replaces whatever
    // the default constructor self-allocated -- that original
    // allocation is simply abandoned bump space in this instance's own
    // (orphaned, for the embedded-member case) local_arena_, harmless
    // beyond the one-time waste of kAccumCapacity bytes there.
    void BindAccumBuffer(char* buf)
    {
        accum_ = buf;
        std::memset(accum_, 0, kAccumCapacity);
    }

    void SetMode(Mode mode) { mode_ = mode; }

    bool FeedRaw(const char* data, size_t len)
    {
        if (accum_len_ + len >= kAccumCapacity)
        {
            std::cerr << "[PicoHTTPParser] Accumulation overflow\n";
            state_ = State::Error;
            return false;
        }

        std::memcpy(accum_ + accum_len_, data, len);
        prev_len_   = accum_len_;
        accum_len_ += len;
        num_headers_ = 32;

        int result = phr_parse_request(
            accum_, accum_len_,
            &method_,  &method_len_,
            &path_,    &path_len_,
            &minor_ver_,
            headers_,  &num_headers_,
            prev_len_
        );

        if (result > 0)  { state_ = State::Complete; return true; }
        if (result == -1){ state_ = State::Error;    return false; }
        state_ = State::Parsing;
        return false;
    }

    void ParseConcrete(ETCS::MirrorBuffer& io, ETCS::SignalContext ctx) override
    {
        state_ = State::Parsing;

        ETCS::Buffer frame;
        while (io.readRaw(frame))
        {
            if (ctx.isInterrupted() || ctx.isTerminated()) { state_ = State::Error; return; }

            size_t incoming = frame.written;
            if (accum_len_ + incoming >= kAccumCapacity)
            {
                std::cerr << "[PicoHTTPParser] Accumulation buffer overflow\n";
                state_ = State::Error;
                return;
            }

            std::memcpy(accum_ + accum_len_, frame.buf, incoming);
            prev_len_   = accum_len_;
            accum_len_ += incoming;

            int result = -2;

            if (mode_ == Mode::Request)
            {
                num_headers_ = 32;
                result = phr_parse_request(
                    accum_, accum_len_,
                    &method_,  &method_len_,
                    &path_,    &path_len_,
                    &minor_ver_,
                    headers_,  &num_headers_,
                    prev_len_
                );
            }
            else
            {
                result = phr_parse_response(
                    accum_, accum_len_,
                    &minor_ver_,
                    &status_,
                    &msg_,     &msg_len_,
                    headers_,  &num_headers_,
                    prev_len_
                );
            }

            if (result > 0)
            {
                state_ = State::Complete;
                flushParsed(io, result);
                return;
            }
            else if (result == -1)
            {
                std::cerr << "[PicoHTTPParser] Parse error\n";
                state_ = State::Error;
                return;
            }
        }
    }

    bool ResetConcrete() override
    {
        method_     = nullptr; method_len_  = 0;
        path_       = nullptr; path_len_    = 0;
        msg_        = nullptr; msg_len_     = 0;
        minor_ver_  = 0;
        status_     = 0;
        num_headers_ = 0;
        accum_len_  = 0;
        prev_len_   = 0;
        state_      = State::Idle;
        std::memset(accum_,   0, kAccumCapacity);
        std::memset(headers_, 0, sizeof(headers_));
        return true;
    }

    bool DeleteConcrete() 
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }
    

    State       GetState()     const { return state_; }
    const char* GetMethod()    const { return method_; }
    size_t      GetMethodLen() const { return method_len_; }
    const char* GetPath()      const { return path_; }
    size_t      GetPathLen()   const { return path_len_; }
    int         GetStatus()    const { return status_; }
    int         GetMinorVer()  const { return minor_ver_; }

    size_t GetNumHeaders() const { return num_headers_; }
    const struct phr_header* GetHeaders() const { return headers_; }

private:
    // Write the parsed header block back into io as one or more Buffer frames.
    //
    // FIX: the previous guard `out.written > Buffer::bufsize - 256` evaluated
    // to `out.written > 0` whenever Buffer::bufsize == 256 (the common case —
    // see TBuffer.h, MAX_TAG_BUFFER_SIZE), since 256 - 256 == 0. That meant
    // flushParsed issued a separate writeRaw call after nearly every single
    // header line instead of batching into full 256-byte frames, which is
    // correct but wasteful. The real intent was "flush when close to full,
    // leaving headroom for the next header line" — use a fixed headroom
    // constant well under bufsize instead of subtracting bufsize from itself.
    static constexpr size_t FLUSH_HEADROOM = 64; // bytes reserved per header line

    void flushParsed(ETCS::MirrorBuffer& io, int header_len)
    {
        ETCS::Buffer out;

        if (mode_ == Mode::Request)
        {
            out.writeString(method_, method_len_);
            out.writeString(" ", 1);
            out.writeString(path_, path_len_);
        }
        else
        {
            out << status_;
            out.writeString(msg_, msg_len_);
        }

        for (size_t i = 0; i < num_headers_; ++i)
        {
            out.writeString(headers_[i].name,  headers_[i].name_len);
            out.writeString(": ", 2);
            out.writeString(headers_[i].value, headers_[i].value_len);
            out.writeString("\n", 1);

            // Flush when within FLUSH_HEADROOM bytes of the buffer's capacity —
            // leaves room for the next header line before forcing a frame split.
            if (out.written + FLUSH_HEADROOM > ETCS::Buffer::bufsize)
            {
                io.writeRaw(out);
                out.reset();
            }
        }

        size_t body_offset = static_cast<size_t>(header_len);
        if (body_offset < accum_len_)
        {
            size_t body_len = accum_len_ - body_offset;
            out.writeString(accum_ + body_offset, body_len);
        }

        if (out.written > 0)
            io.writeRaw(out);
    }
};

#endif
