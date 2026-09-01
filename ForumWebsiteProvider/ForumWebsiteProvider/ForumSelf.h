#ifndef FORUMSELF_H__
#define FORUMSELF_H__
#include "ForumThread.h"

class ForumNode;

// ── ForumSelf: one participant's SELF ─────────────────────────────────────────
// Not a server-wide user table. A self is one identity's own view: the threads
// that identity holds an edge to, and that identity's own record.
//
// Same P2P shape ChessLobby carries, brought forward for the same reason. In
// the eventual model every participant runs their own node and their own copy
// of the threads they follow, and a thread is an EDGE joining selves, each
// replaying the same posts locally and arriving at the same head. Right now
// every edge resolves to one shared ForumThread. The transition changes what
// the edge RESOLVES to (a shared entity becomes my own entity plus a replayed
// post stream), not the arrangement of types -- and the hash chain is already
// the thing that makes those replays checkable against each other.
//
// ── Where the forum DIVERGES from chess, and it matters ──────────────────────
// ChessLobby's comment says a history belongs to a self, so losing the session
// loses the history, and that is honest for chess. It is NOT honest here, and
// the asymmetry is the reason this module needs a database at all:
//
//   In chess the artifact dies with the players. A finished game is a record on
//   a self, and when the self expires the record is meaningfully gone.
//
//   In a forum the artifact OUTLIVES every self that touched it. A thread whose
//   participants have all gone home is still a thread, and still the thing
//   anyone else came for. So a self expiring drops the SELF -- the session, the
//   edges, the local counters -- and touches no thread.
//
// That is why the node persists threads and does not persist selves. A self is
// a session; a thread is the record.
class ForumSelf :
    public DeletableBase<ForumSelf>,
    public FilterBase<ForumSelf>
{
    friend struct ForumStream;
    friend class  ForumNode;

public:
    WIRE_TYPE_IDENTITY(ForumSelf);

    using Clock = ForumThread::Clock;

    ForumSelf()          = default;
    virtual ~ForumSelf() = default;

    // A self answers to its own identity. Present so it can be routed to
    // directly once it is the only self on a node (the P2P case), even though
    // today the node in front of it does the dispatch.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        if (self_.empty()) { io.reset(); return false; }
        const std::string desc = io.restAsString();
        bool found = false;
        for (size_t i = 0; !found && i < desc.size(); )
        {
            size_t j = desc.find('/', i);
            if (j == std::string::npos) j = desc.size();
            if (desc.compare(i, j - i, self_) == 0) found = true;
            i = j + 1;
        }
        if (!found) { io.reset(); return false; }
        io.writeString(self_.c_str());
        return true;
    }

    const std::string& Self() const { return self_; }

    bool DeleteConcrete()
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                  + this->getSourceTag().toString();
        ETCS_LOG("ForumSelf", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

private:
    // ── Ordering thread only ──────────────────────────────────────────────

    void touchLocked() { seen_ = Clock::now(); }

    int idleSecondsLocked() const
    {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                   Clock::now() - seen_).count());
    }

    // A single edge join. Idempotent: revisiting a thread I am already in is the
    // ordinary case (a reload), not a second edge.
    void addEdgeLocked(const std::string& tkey, ForumThread* t)
    {
        for (const auto& [k, p] : edges_) if (k == tkey) return;
        edges_.emplace_back(tkey, t);
    }

    void dropEdgeLocked(const std::string& tkey)
    {
        for (auto it = edges_.begin(); it != edges_.end(); ++it)
            if (it->first == tkey) { edges_.erase(it); return; }
    }

    bool hasEdgeLocked(const std::string& tkey) const
    {
        for (const auto& [k, p] : edges_) if (k == tkey) return true;
        return false;
    }

    // Posting LOCKS the name, exactly as a chess result does. Not enforcement --
    // there is no authority here to enforce against, since the identity is a
    // client-supplied token -- but a declaration the client honours by
    // disabling its own rename, and the seed of real persistence once an ACE
    // key signs the identity instead.
    //
    // It means something slightly stronger here than in chess, because the post
    // it locks on is in a durable thread with the token written into its hash
    // chain. Renaming after posting would leave the old name in the record
    // permanently, so the lock is describing a fact rather than imposing a
    // policy.
    void recordPostLocked(const std::string& tkey, uint64_t ihash)
    {
        ++posts_;
        locked_   = true;
        last_head_ = ihash;
        last_tkey_ = tkey;
        touchLocked();
    }

    // "self posts threads locked|open idle"
    std::string profileLocked() const
    {
        std::string s = self_;
        s += " " + std::to_string(posts_);
        s += " " + std::to_string(edges_.size());
        s += locked_ ? " locked" : " open";
        s += " " + std::to_string(idleSecondsLocked());
        return s;
    }

    // My threads, one per line. Defined in ForumNode.h -- it reads state off
    // the threads themselves, which need to be complete.
    std::string renderListLocked() const;

    ForumNode*  node_ = nullptr;   // the host; one per peer in the P2P case
    std::string self_;
    unsigned    posts_  = 0;
    bool        locked_ = false;                 // a post has been made

    // The last thing this self committed to, kept so a client can verify its
    // own contribution survived a reload without walking the whole thread.
    uint64_t    last_head_ = 0;
    std::string last_tkey_;

    Clock::time_point seen_ = Clock::now();      // session heartbeat

    // The edges: (thread key, the thread that key currently resolves to). Today
    // that pointer is shared with every other participant; later it is mine.
    std::vector<std::pair<std::string, ForumThread*>> edges_;
};

#endif // FORUMSELF_H__
