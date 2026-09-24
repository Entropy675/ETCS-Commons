#ifndef CHESSLOBBY_H__
#define CHESSLOBBY_H__
#include "ChessGame.h"

class ChessNode;

// ── ChessLobby: one player's SELF ─────────────────────────────────────────────
// Not a server-wide room registry. A lobby is one identity's own view: the
// matches that identity is in, and that identity's own record.
//
// This is the P2P shape brought forward deliberately. In the eventual model
// every participant runs their own board and their own lobby, and a match is a
// single EDGE joining two selves, each replaying the same moves locally. Right
// now both edges resolve to one shared ChessGame -- a dumb proxy to the same
// remote board -- but the structure is already the structure: my games live on
// me, your games live on you, and a match is a thing we both hold an edge to.
// The transition later changes what the edge RESOLVES to (a shared entity
// becomes my own entity plus a replay stream), not the arrangement of types.
//
// Records live here for the same reason: a history belongs to a self, not to a
// server. And the session rule falls out of it -- the lobby IS the session, so
// losing the session is losing the lobby, and wiping the history is simply the
// node dropping a stale self. That is the honest version of "weak persistence":
// the name means something exactly as long as you keep it alive.
class ChessLobby :
    public DeletableBase<ChessLobby>,
    public FilterBase<ChessLobby>
{
    friend struct ChessStream;
    friend class  ChessNode;

public:
    WIRE_TYPE_IDENTITY(ChessLobby);

    using Clock = ChessGame::Clock;

    ChessLobby()          = default;
    virtual ~ChessLobby() = default;

    // A self answers to its own identity. Present so a lobby can be routed to
    // directly once it is the only lobby on a node (the P2P case), even though
    // today the node in front of it does the dispatch.
    bool AcceptsConcrete(ETCS::Buffer& io) const override
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

    bool DeleteConcrete() override
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                   + this->getSourceTag().toString();
        ETCS_LOG("ChessLobby", "Delete: firing self-DestroyEvent for RID:"
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

    // A single edge join. Idempotent: rejoining a match I am already in is the
    // ordinary case (a reload), not a second edge.
    void addEdgeLocked(const std::string& match, ChessGame* g)
    {
        for (const auto& [m, p] : edges_) if (m == match) return;
        edges_.emplace_back(match, g);
    }

    void dropEdgeLocked(const std::string& match)
    {
        for (auto it = edges_.begin(); it != edges_.end(); ++it)
            if (it->first == match) { edges_.erase(it); return; }
    }

    bool hasEdgeLocked(const std::string& match) const
    {
        for (const auto& [m, p] : edges_) if (m == match) return true;
        return false;
    }

    // A result LOCKS the name. Not enforcement -- there is no authority here to
    // enforce against, since the identity is a client-supplied token -- but a
    // declaration the client honours by disabling its own rename, and the seed
    // of real persistence once an ACE key signs the identity instead.
    void recordLocked(char outcome)
    {
        if      (outcome == 'w') ++wins_;
        else if (outcome == 'l') ++losses_;
        else                     ++draws_;
        locked_ = true;
        touchLocked();
        ETCS_LOG("ChessLobby", "record '" << self_ << "': "
                 << wins_ << "W " << losses_ << "L " << draws_ << "D");
    }

    // "self wins losses draws locked|open games idle"
    std::string profileLocked() const
    {
        std::string s = self_;
        s += " " + std::to_string(wins_);
        s += " " + std::to_string(losses_);
        s += " " + std::to_string(draws_);
        s += locked_ ? " locked" : " open";
        s += " " + std::to_string(edges_.size());
        s += " " + std::to_string(idleSecondsLocked());
        return s;
    }

    // My games, one per line: "match opponent occupants state".
    // Opponent is resolved from the shared board's seats -- which is exactly the
    // information a real edge would carry in its own right once the boards are
    // separate, so reading it here is a stand-in, not a shortcut.
    std::string listLocked() const;   // defined in ChessNode.h (needs the games)

    ChessNode*  node_ = nullptr;   // the host; one per peer in the P2P case
    std::string self_;
    unsigned    wins_ = 0, losses_ = 0, draws_ = 0;
    bool        locked_ = false;                 // a result has been recorded
    Clock::time_point seen_ = Clock::now();      // session heartbeat

    // The edges: (match id, the board that match currently resolves to). Today
    // that pointer is shared with the opponent's lobby; later it is mine alone.
    std::vector<std::pair<std::string, ChessGame*>> edges_;

    /*
     * ── THE LOBBY AS A NAME SERVER SEES IT ──────────────────────────────────
     *
     * A self whose page is online HOSTS a lobby: its own board, two seats, on
     * its own runtime. This node does not hold that board. It lists the lobby
     * (hosting_), finds it a partner (waiting_, ChessNode::pairLocked), and
     * relays the two runtimes' lines to each other in one order (Pair::record)
     * -- each runtime replays every line against its own board, so the two
     * boards are one game. A browser can dial a node and cannot be dialled,
     * so the relay is the proxy level between two pages; the lines are the
     * verbs themselves, so a direct link can carry the same ones later.
     *
     * The pair lives on its OWNER's lobby, the one whose table the partner
     * sat at; the partner's lobby names it (guest_of_).
     */
    struct Pair
    {
        std::string id;                    // "<owner>-<n>": each pairing a new game
        std::string partner;
        std::vector<std::string> record;   // "<seq> <self> <verb>[ <arg>]"
        std::vector<uint64_t>    chains;   // the record chain after each line
        uint64_t    base_chain = 0;        // the chain before record[0]
        size_t      base = 0;              // seq of record[0] -- the ring drops the oldest
        bool        ended = true;
    };
    bool        hosting_ = false;   // its page is online at this node
    bool        waiting_ = false;   // asked for a partner and has none yet
    Pair        pair_;              // the pairing at this self's own table
    std::string guest_of_;          // the owner whose table this self sits at
    unsigned    pairs_ = 0;         // pairings made at this table, for ids
    std::string claim_;             // the token of the page that holds this name -- see ChessNode
    std::string game_ = "chess";    // what its page plays -- the listing's fourth field
};

#endif // CHESSLOBBY_H__
