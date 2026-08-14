#ifndef CHESSNODE_H__
#define CHESSNODE_H__
#include "ChessLobby.h"
#include <mutex>

// ── ChessNode: the host ───────────────────────────────────────────────────────
// Owns the selves and the boards, and does all path parsing.
//
// This is the CENTRALIZATION ARTIFACT, and naming it as one is the point: a node
// hosting many lobbies is a server, and a node hosting exactly ONE lobby is a
// peer. Nothing else about the arrangement changes between those two cases --
// same types, same edges, same routing -- so the P2P transition is a change of
// cardinality plus a change in what an edge resolves to, not a restructure.
// That is why the lobby is a self rather than a room registry, even though a
// room registry would have been less code today.
//
// Path: /<mount>/<self>/<match>/<verb>[/<arg>]
// Self comes FIRST because it is the segment that becomes implicit when the
// request arrives at your own machine: a peer's own URL is just /<match>/<verb>.
class ChessNode :
    public DeletableBase<ChessNode>,
    public FilterBase<ChessNode>
{
    friend struct ChessStream;
    friend class  ChessGame;

public:
    WIRE_TYPE_IDENTITY(ChessNode);

    using Kind  = ChessInEvent::Kind;
    using Clock = ChessGame::Clock;

    ChessNode()          = default;
    virtual ~ChessNode() = default;

    // ── THE ordering domain ───────────────────────────────────────────────
    // One stream per node. Not per module, and not per board.
    //
    // Per module was what this replaced: it asserted a causal relation between
    // unrelated games and charged head-of-line blocking proportional to
    // module-wide traffic, which converts straight into ThreadPool starvation
    // because every ChessOpEvent waiter is a parked pool thread.
    //
    // Per BOARD is the finest domain that is correct in isolation, and it is
    // not what this does, for a reason worth stating: this node touches every
    // game it hosts. roomsLocked reads liveTokensLocked/over_/checkmate_/
    // started_ across all of them; reapLocked walks them and calls Delete();
    // joinLocked reads active_ and every lobby's edges; ChessGame::
    // reportOutcomeLocked calls straight back into reportLocked; ChessLobby::
    // listLocked reads white_/black_ off boards it does not own. Every one of
    // those is safe ONLY because it is the same thread. Cutting between node
    // and boards turns all of them into unsynchronised cross-domain reads,
    // repairable only by giving the node a published read model -- a real CQRS
    // split, and a much larger change than this one.
    //
    // Putting the boundary AT the node means none of that machinery is needed
    // and every listed call stays a plain function call.
    //
    // WHY THE NODE, in terms that survive the P2P transition: the tempting
    // justification is "a game needs two parties, so a shared host exists
    // anyway, and it is the natural sink for both sides". That is true of the
    // current arrangement and FALSE of the target one -- in the peer case each
    // participant runs their own node and their own board, and what is shared
    // is the edge, not the host. Baking in the two-party-shared-host reasoning
    // would be baking in centralization.
    //
    // The justification that holds either way: a node is ONE PARTICIPANT'S
    // LOCAL ORDERING DOMAIN. Replay is node-to-node -- a peer syncs an ordering
    // domain over a MirrorBuffer by replaying its events -- so the sync unit
    // and the ordering domain must be the same object, or a peer following
    // three of my games needs three channels. Per-board would force exactly
    // that.
    //
    // It follows that contention is proportional to how centralized the
    // deployment is: one lobby on the node (the peer case) and there is none,
    // since the node holds one board; N lobbies and it is N-way. The
    // centralization cost stops being a paragraph and becomes a latency term.
    //
    // Started lazily under call_once, not in the ctor: two pool threads can
    // reach a freshly created node at once and start() is not idempotent. Lazy
    // also keeps a configured-but-unused node from costing a thread.
    ChessStream& stream() const
    {
        std::call_once(stream_started_, [this]
        {
            // Same arena as the old getInstance() used, deliberately: this
            // change is about WHO owns the domain, not about where it
            // allocates. Moving it to getArena() is a separate question.
            stream_.start(ETCS::MemoryArena::getInstance());
        });
        return stream_;
    }

    // One filter for everything under the mount. Deliberately NOT routed
    // through the stream: it runs for every request on the server, including
    // paths that turn out not to be ours, and a round trip here would serialize
    // all path matching behind game logic.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        const std::string desc = io.restAsString();
        size_t i = 0;
        while (i < desc.size() && desc[i] == '/') ++i;
        size_t j = desc.find('/', i);
        if (j == std::string::npos) j = desc.size();
        if (desc.compare(i, j - i, mount_) != 0) { io.reset(); return false; }
        io.writeString(mount_.c_str());
        return true;
    }

    // ── Public surface: serialized on the shared chess stream ─────────────
    std::string Request(const std::string& path) { return op(Kind::LobbyRequest, path); }
    std::string Players() const                   { return op(Kind::LobbyPlayers); }
    std::string Rooms() const                     { return op(Kind::LobbyList); }

    const std::string& MountPath() const { return mount_; }
    void SetMount(const std::string& m)  { mount_ = m; }   // setup only

    bool DeleteConcrete()
    {
    // OPEN, and the one thing this change leaves unfinished: the stream is
    // never stopped. As a module singleton it lived for the life of the DSO and
    // there was nothing to tear down; owned per node, a deleted node leaves its
    // ordering thread running. Nodes are few and long-lived so this is not
    // urgent, but it IS a leak per Delete, and the fix needs EventStream's own
    // stop/join surface -- which is core, not module, so it is named here
    // rather than guessed at.
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                   + this->getSourceTag().toString();
        ETCS_LOG("ChessNode", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

private:
    // A session is far longer than a seat grace: losing your seat because you
    // walked away for a minute is fine, losing your whole history for it is not.
    static constexpr int kSession   = 600;   // self kept this long after last request
    static constexpr int kSeatGrace = 30;    // matches ChessGame's own reaping

    std::string op(Kind k, const std::string& arg = "") const
    {
        return ChessOpEvent{&stream(), k, const_cast<ChessNode*>(this), arg}();
    }

    // ── Ordering thread only ──────────────────────────────────────────────

    ChessLobby* findLobbyLocked(const std::string& self) const
    {
        for (const auto& [s, l] : lobbies_) if (s == self) return l;
        return nullptr;
    }

    ChessGame* findGameLocked(const std::string& match) const
    {
        for (const auto& [m, g] : games_) if (m == match) return g;
        return nullptr;
    }

    // addTag from here blocks on the LOADER's ordering thread -- a different
    // thread with no cycle back, so it completes rather than deadlocking. It
    // stalls chess dispatch for the duration, acceptable for something that
    // happens once per self or per match rather than once per request.
    ChessLobby* createLobbyLocked(const std::string& self)
    {
        ChessLobby* l = addTag<ChessLobby>();
        if (!l) { ETCS_LOG("ChessNode", "addTag<ChessLobby> failed for '" << self << "'"); return nullptr; }
        l->self_ = self;
        l->node_ = this;
        lobbies_.emplace_back(self, l);
        ETCS_LOG("ChessNode", "self joined: '" << self << "' RID:" << l->getRID()
                 << " (" << lobbies_.size() << " live)");
        return l;
    }

    ChessGame* createGameLocked(const std::string& match)
    {
        ChessGame* g = addTag<ChessGame>();
        if (!g) { ETCS_LOG("ChessNode", "addTag<ChessGame> failed for '" << match << "'"); return nullptr; }
        g->match_key_ = match;
        g->node_      = this;
        games_.emplace_back(match, g);
        ETCS_LOG("ChessNode", "match opened: '" << match << "' RID:" << g->getRID()
                 << " (" << games_.size() << " live)");
        return g;
    }

    ChessLobby* selfLocked(const std::string& s)
    {
        ChessLobby* l = findLobbyLocked(s);
        if (!l) l = createLobbyLocked(s);
        if (l)  l->touchLocked();
        return l;
    }

    // Called by ChessGame when a game finishes, once per outcome. The game knows
    // its seats (tokens); the node is what maps a token to a self.
    void reportLocked(const std::string& tok, char outcome)
    {
        if (tok.empty()) return;
        ChessLobby* l = findLobbyLocked(tok);
        if (!l) l = createLobbyLocked(tok);   // a result is worth a self
        if (l)  l->recordLocked(outcome);
    }

public:
    // Public only because ChessLobby::listLocked reports the same states from
    // the edge's side; everything else here stays private.
    static const char* stateOf(const ChessGame* g)
    {
        if (!g->over_.empty()) return g->over_.c_str();
        if (g->checkmate_)     return "checkmate";
        if (g->stalemate_)     return "stalemate";
        return g->started_ ? "playing" : "waiting";
    }

private:

    // Every self on this node: the operator view, and a leaderboard.
    std::string playersLocked() const
    {
        std::string out;
        for (const auto& [s, l] : lobbies_) { out += l->profileLocked(); out += "\n"; }
        return out;
    }

    // Every match on this node: "match occupants seats state".
    std::string roomsLocked() const
    {
        std::string out;
        for (const auto& [m, g] : games_)
        {
            out += m;
            out += " " + std::to_string(g->liveTokensLocked(kSeatGrace));
            out += " " + std::to_string(g->Seats());
            out += " ";
            out += stateOf(g);
            out += "\n";
        }
        return out;
    }

    // Fill-in pairing, from the asking self's point of view. Rejoin one of my own
    // unfinished matches first: a reload should return me to my game, not pair me
    // with a stranger.
    std::string joinLocked(const std::string& self)
    {
        ChessLobby* me = selfLocked(self);
        if (!me) return "FAILED";

        for (const auto& [m, g] : me->edges_)
            if (g && g->active_) return m;

        // Otherwise the oldest match with room, so two arrivals MEET rather than
        // each opening an empty one and waiting alone.
        for (const auto& [m, g] : games_)
        {
            if (!g->active_) continue;
            if (g->liveTokensLocked(kSeatGrace) >= 2) continue;
            if (me->hasEdgeLocked(m)) continue;
            me->addEdgeLocked(m, g);
            return m;
        }

        const std::string match = "m" + std::to_string(++counter_);
        ChessGame* g = createGameLocked(match);
        if (!g) return "FAILED";
        me->addEdgeLocked(match, g);
        return match;
    }

    // Drop selves whose session has expired -- which wipes their history, by
    // design: a name only means something while it is being kept alive. Also
    // drops matches nobody holds a live edge to, so rooms-per-person cannot
    // accumulate without bound.
    void reapLocked()
    {
        for (size_t i = 0; i < lobbies_.size(); )
        {
            ChessLobby* l = lobbies_[i].second;
            if (l && l->idleSecondsLocked() >= kSession)
            {
                ETCS_LOG("ChessNode", "session expired, wiping self '" << l->self_
                         << "' (" << l->wins_ << "W " << l->losses_ << "L "
                         << l->draws_ << "D discarded)");
                lobbies_.erase(lobbies_.begin() + static_cast<long>(i));
                l->Delete();
                continue;
            }
            ++i;
        }

        for (size_t i = 0; i < games_.size(); )
        {
            ChessGame* g = games_[i].second;
            bool referenced = false;
            for (const auto& [s, l] : lobbies_)
                if (l && l->hasEdgeLocked(games_[i].first)) { referenced = true; break; }
            if (!referenced && g && g->liveTokensLocked(kSeatGrace) == 0)
            {
                ETCS_LOG("ChessNode", "match '" << games_[i].first << "' has no edges left -- closing.");
                const std::string m = games_[i].first;
                games_.erase(games_.begin() + static_cast<long>(i));
                g->Delete();
                continue;
            }
            ++i;
        }
    }

    // /<mount>/<self>/<match>/<verb>[/<arg>]
    // /<mount>/<self>/list | join | me
    // /<mount>/players | rooms
    //
    // "players" and "rooms" are reserved selves; "list", "join" and "me" are
    // reserved matches.
    std::string requestLocked(const std::string& path)
    {
        std::vector<std::string> seg;
        ChessGame::splitPath(path, seg);
        if (seg.empty() || seg[0] != mount_) return "NOT FOUND";

        reapLocked();

        const std::string self = (seg.size() > 1) ? seg[1] : "";
        if (self.empty())      return "NOT FOUND";
        if (self == "players") return playersLocked();
        if (self == "rooms")   return roomsLocked();

        ChessLobby* me = selfLocked(self);
        if (!me) return "FAILED";

        const std::string match = (seg.size() > 2) ? seg[2] : "";
        if (match.empty() || match == "list") return me->listLocked();
        if (match == "me")                    return me->profileLocked();
        if (match == "join")                  return joinLocked(self);

        ChessGame* g = findGameLocked(match);
        if (!g) g = createGameLocked(match);
        if (!g) return "FAILED";

        // Visiting a match IS the edge join. There is no separate step, which is
        // what makes a shared link work: following it puts the match on your own
        // self without anyone granting you anything.
        me->addEdgeLocked(match, g);

        const std::string verb = (seg.size() > 3) ? seg[3] : "";
        const std::string arg  = (seg.size() > 4) ? seg[4] : "";
        return g->verbLocked(self, verb, arg);
    }

    // mutable: stream() is const because the read verbs are, and call_once is
    // the mutation. The domain is part of this node's identity, not part of
    // its observable state.
    mutable ChessStream    stream_;
    mutable std::once_flag stream_started_;

    std::string mount_ = "game";
    std::vector<std::pair<std::string, ChessLobby*>> lobbies_;   // the selves
    std::vector<std::pair<std::string, ChessGame*>>  games_;     // the boards
    unsigned counter_ = 0;
};

// ── Deferred definitions ──────────────────────────────────────────────────────
// These need both the lobby and the node complete.

// "match opponent occupants state" per line -- my games, from my side. Opponent
// is read off the shared board's seats today; once the boards are separate that
// same field comes from the edge itself, which is why it is reported per-self
// rather than as a property of the room.
inline std::string ChessLobby::listLocked() const
{
    std::string out;
    for (const auto& [m, g] : edges_)
    {
        if (!g) continue;
        std::string opp = "-";
        if (!g->white_.empty() && g->white_ != self_) opp = g->white_;
        if (!g->black_.empty() && g->black_ != self_) opp = g->black_;
        out += m;
        out += " " + opp;
        out += " " + std::to_string(g->liveTokensLocked(30));
        out += " ";
        out += ChessNode::stateOf(g);
        out += "\n";
    }
    return out;
}

// A board's ordering domain is its node's. Deferred to here because it needs
// ChessNode complete. Null for a standalone board, which therefore refuses
// every verb rather than running one unsynchronised.
inline ChessStream* ChessGame::streamOf() const
{
    return node_ ? &node_->stream() : nullptr;
}

// The game reports to the NODE, which owns the token -> self mapping. A game with
// no node (the standalone board chess_server.etcs spawns) simply keeps no records.
inline void ChessGame::reportOutcomeLocked()
{
    if (recorded_ || !node_) return;
    recorded_ = true;

    if (over_ == "draw" || stalemate_)
    {
        node_->reportLocked(white_, 'd');
        node_->reportLocked(black_, 'd');
        return;
    }

    std::string loser, winner;
    if      (over_ == "resign-white") { loser = white_; winner = black_; }
    else if (over_ == "resign-black") { loser = black_; winner = white_; }
    else if (checkmate_)
    {
        // The side TO MOVE is the side that got mated.
        if (board.isWhiteTurn()) { loser = white_; winner = black_; }
        else                     { loser = black_; winner = white_; }
    }
    else return;   // not actually finished

    node_->reportLocked(winner, 'w');
    node_->reportLocked(loser,  'l');
}

inline ETCS::DispatchResult ChessStream::on_event(ChessState&,
                                                  const ChessInEventPtr& evt,
                                                  uint64_t)
{
    ChessInEvent& e = *evt.ptr;
    const std::string arg = e.arg ? e.arg : "";
    const std::string tok = e.tok ? e.tok : "";
    std::string result;

    switch (e.kind)
    {
        case ChessInEvent::Kind::LobbyRequest:
        {
            ChessNode* n = static_cast<ChessNode*>(e.target);
            if (n) result = n->requestLocked(arg);
            break;
        }
        case ChessInEvent::Kind::LobbyPlayers:
        {
            ChessNode* n = static_cast<ChessNode*>(e.target);
            if (n) result = n->playersLocked();
            break;
        }
        case ChessInEvent::Kind::LobbyList:
        {
            ChessNode* n = static_cast<ChessNode*>(e.target);
            if (n) result = n->roomsLocked();
            break;
        }
        case ChessInEvent::Kind::LobbyJoin:
        {
            ChessNode* n = static_cast<ChessNode*>(e.target);
            if (n) result = n->joinLocked(arg);
            break;
        }

        default:
        {
            ChessGame* g = static_cast<ChessGame*>(e.target);
            if (!g) break;
            switch (e.kind)
            {
                case ChessInEvent::Kind::Request:  result = g->requestLocked(arg);        break;
                case ChessInEvent::Kind::Move:     result = g->applyMoveLocked(arg, tok); break;
                case ChessInEvent::Kind::LoadFen:  result = g->loadFenLocked(arg);        break;
                case ChessInEvent::Kind::Fen:      result = g->fenLocked();               break;
                case ChessInEvent::Kind::Status:   result = g->statusLineLocked(tok);     break;
                case ChessInEvent::Kind::Chat:     result = g->chatLogLocked();           break;
                case ChessInEvent::Kind::Say:      g->sayLocked(tok, arg);
                                                   result = g->chatLogLocked();           break;
                case ChessInEvent::Kind::Key:      result = g->keyLocked(arg);            break;
                case ChessInEvent::Kind::Reset:    g->resetLocked();
                                                   result = g->fenLocked();               break;
                case ChessInEvent::Kind::IsActive: result = g->isActiveLocked() ? "active" : "finished"; break;
                default: break;
            }
            break;
        }
    }

    if (e.result_out) *e.result_out = result;
    // Release AFTER writing the result: the waiting thread acquire-loads this
    // flag and then reads result_out, so this store is the happens-before edge.
    if (e.done) e.done->store(true, std::memory_order_release);

    // Drop, not Inline: the work is finished and nothing downstream consumes an
    // emit, so a gap slot would reserve reorder capacity for nothing. The
    // serialization comes from the ordering thread being a single thread.
    return { ETCS::DispatchKind::Drop, 0, nullptr };
}

#endif // CHESSNODE_H__