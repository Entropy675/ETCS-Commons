#ifndef CONNREF_H__
#define CONNREF_H__
#include "SocketConnectionState.h"

// ConnRef — one io_inflight_ reference, owned for exactly this scope's
// lifetime and released automatically on every path out, unless explicitly
// disarmed because something else now owns it.
//
// WHY THIS EXISTS: every recv/send/dispatch function in this module already
// followed one rule -- "entered holding one reference, release it via
// NoteComplete on every path out, UNLESS new async work took it over, in
// which case add a fresh NoteSubmit for that work and still release the
// original" -- but the rule lived only in comments, re-derived by hand at
// every guard clause, every error branch, every recursive call. That is
// exactly the kind of bookkeeping a review has to re-trace branch by branch
// to trust, and exactly the kind past design passes on this feature caught
// getting wrong (a missed release, a double release) before any of it ever
// reached disk. Wrapping the SAME rule in a destructor makes "released
// exactly once" a property the compiler enforces on every path -- including
// ones nobody thought to check by hand -- rather than one a reader has to
// re-verify.
//
// THE TWO WAYS TO OBTAIN ONE:
//   Wrap(conn)    -- claims a reference this scope ALREADY holds (the
//                     "entry" reference a caller handed off, or the
//                     reference an async completion is firing with). Does
//                     NOT call NoteSubmit -- there is nothing new to submit,
//                     only existing ownership to make explicit.
//   Acquire(conn) -- calls NoteSubmit() and wraps the NEW reference that
//                     creates, for when this scope is about to start a
//                     genuinely new piece of outstanding async work
//                     alongside (or instead of) the one it already holds.
//
// THE PATTERN THIS REPLACES, EVERYWHERE IN THIS MODULE:
//   conn->NoteSubmit();
//   if (!something_that_may_fail_synchronously())
//       conn->NoteComplete();   // easy to forget, easy to duplicate
// becomes:
//   ConnRef work = ConnRef::Acquire(conn);
//   if (something_that_may_fail_synchronously()) work.disarm();
//   // work's destructor covers the failure path with nothing to remember
//
// HANDING A REFERENCE TO A CONTINUATION THAT OUTLIVES THIS SCOPE (a
// callback, a recursive call, a scheduled job): disarm the ConnRef holding
// it immediately before making that call, and have the far side construct
// its OWN ConnRef::Wrap(conn) as its first statement. That is the entire
// protocol -- no separate "caller must NoteComplete after calling me"
// convention to remember at each call site, because the callee's own
// ConnRef now owns it from the moment it starts running.
class ConnRef
{
public:
    static ConnRef Wrap(SocketConnectionState* c)    { return ConnRef(c); }
    static ConnRef Acquire(SocketConnectionState* c) { c->NoteSubmit(); return ConnRef(c); }

    ~ConnRef() { if (conn_) conn_->NoteComplete(); }

    ConnRef(ConnRef&& o) noexcept : conn_(o.conn_) { o.conn_ = nullptr; }
    ConnRef& operator=(ConnRef&& o) noexcept
    {
        if (this != &o)
        {
            if (conn_) conn_->NoteComplete();
            conn_ = o.conn_;
            o.conn_ = nullptr;
        }
        return *this;
    }
    ConnRef(const ConnRef&)            = delete;
    ConnRef& operator=(const ConnRef&) = delete;

    // The reference is now accounted for some other way -- handed to a
    // continuation that will construct its own ConnRef::Wrap when it runs,
    // or already released by an explicit call this scope made itself. The
    // destructor becomes a no-op after this.
    void disarm() { conn_ = nullptr; }

    // Release NOW rather than at scope exit, for the rare place where the
    // exact release POINT is load-bearing rather than just the fact of it --
    // ConnectionManager::dispatchToSubscribers logs the pool's in-use count
    // on the line immediately after, and releasing later would print a
    // connection that has in fact already gone. Prefer plain scope exit
    // everywhere else: a release you have to place by hand is the thing this
    // class exists to get rid of. Idempotent, like disarm().
    void release() { if (conn_) { conn_->NoteComplete(); conn_ = nullptr; } }

    SocketConnectionState* get() const { return conn_; }
    explicit operator bool() const     { return conn_ != nullptr; }

private:
    explicit ConnRef(SocketConnectionState* c) : conn_(c) {}
    SocketConnectionState* conn_;
};

#endif // CONNREF_H__
