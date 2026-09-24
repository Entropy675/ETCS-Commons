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
#if defined(__EMSCRIPTEN__)
            // start() itself cannot tell "mid-preload" from "long after
            // boot" -- see its own comment (EventStream.h) on why every
            // attempt at teaching it to guess from shared cross-module state
            // failed, one direction or the other, under this tree's
            // per-module wasm linking. So it always starts sync
            // (main-thread poll, no ordering pthread), correct only for a
            // stream that exists before boot's own promotion pass runs.
            //
            // This call_once is never one of those. ChessNode::stream() is
            // reached for the first time from op() -- Request/Players/Rooms
            // -- and nothing calls any of those before a ChessNode exists,
            // and nothing spawns a ChessNode before a script runs, and no
            // script runs until etcs_boot_runtime_threads has already
            // finished (loaders/etcs.cc hands a script to a worker only
            // after arming every module's runtime). So by construction,
            // every call into this lambda is already past the one window
            // sync mode exists to protect -- there is no "too early" case to
            // guess wrong here, which is exactly why this promotes
            // unconditionally instead of trying to detect readiness the way
            // start() itself no longer does.
            //
            // Skipping this line is not a slower chess game, it is a silent
            // one: left in sync_emscripten_ mode, the stream drains only
            // while something happens to be polling it inline, which is
            // preload's own single-threaded assumption -- and once
            // etcs_web_call_async (etcs.cc) is doing its job correctly (off
            // the browser's real main thread, on a ThreadPool worker, which
            // is precisely where ChessOpEvent's wait is SUPPOSED to block),
            // nothing is ever inline with this stream again. The worker
            // spins against a ring nobody drains, forever, with no error --
            // proven empirically before this call existed (see this
            // module's scripts/www/README.md).
            stream_.arm_emscripten_ordering_thread();
#endif
        });
        return stream_;
    }

    // One filter for everything under the mount. Deliberately NOT routed
    // through the stream: it runs for every request on the server, including
    // paths that turn out not to be ours, and a round trip here would serialize
    // all path matching behind game logic.
    bool AcceptsConcrete(ETCS::Buffer& io) const override
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

    bool DeleteConcrete() override
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
    static constexpr int kOnline    = 30;    // a hosted lobby is listed this long after its page's last call
    static constexpr size_t kRelayLines = 4000;   // a game and its chat, many times over

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

    // ── the name server: hosted lobbies, pairing, the relay ────────────────
    //
    // See ChessLobby's Pair for the shape. Online is "its page called within
    // kOnline" -- every call touches the self (selfLocked) -- so a closed tab
    // drops out of the listing and out of its pair by itself.

    bool onlineLocked(const ChessLobby* l) const
    { return l && l->hosting_ && l->idleSecondsLocked() < kOnline; }

    // The lobby whose pair this self is in, as owner or partner, if it is live.
    ChessLobby* pairOfLocked(ChessLobby* me)
    {
        if (!me) return nullptr;
        if (!me->pair_.ended) return me;
        if (me->guest_of_.empty()) return nullptr;
        ChessLobby* o = findLobbyLocked(me->guest_of_);
        if (o && !o->pair_.ended && o->pair_.partner == me->self_) return o;
        return nullptr;
    }

    void endPairLocked(ChessLobby* owner, const std::string& why)
    {
        if (!owner || owner->pair_.ended) return;
        owner->pair_.ended = true;
        ETCS_LOG("ChessNode", "pair " << owner->pair_.id << " ended: " << why);
    }

    // A pair whose owner or partner has gone quiet is over: the board is on
    // their page, and the page is gone.
    void endStaleLocked()
    {
        for (const auto& [s, l] : lobbies_)
        {
            if (!l || l->pair_.ended) continue;
            ChessLobby* p = findLobbyLocked(l->pair_.partner);
            if (!onlineLocked(l))      endPairLocked(l, s + " went quiet");
            else if (!onlineLocked(p)) endPairLocked(l, l->pair_.partner + " went quiet");
        }
    }

    // "PAIRED <id> <owner> <partner>" | "WAITING" | "OPEN" | "ENDED <id>".
    // ENDED names the last pairing this self was in, so a page still showing
    // that game learns it is over; a page on another game ignores it.
    std::string stateLocked(ChessLobby* me)
    {
        if (ChessLobby* o = pairOfLocked(me))
            return "PAIRED " + o->pair_.id + " " + o->self_ + " " + o->pair_.partner;
        std::string last;
        if (!me->pair_.id.empty()) last = me->pair_.id;
        if (!me->guest_of_.empty())
            if (ChessLobby* o = findLobbyLocked(me->guest_of_))
                if (o->pair_.partner == me->self_) last = o->pair_.id;
        if (me->waiting_) return "WAITING";
        return last.empty() ? "OPEN" : "ENDED " + last;
    }

    // A new game at owner's table with guest in the other seat. Anything
    // either of them was in ends first: one table at a time.
    std::string startPairLocked(ChessLobby* owner, ChessLobby* guest)
    {
        if (ChessLobby* o = pairOfLocked(owner)) endPairLocked(o, owner->self_ + " took a new partner");
        if (ChessLobby* o = pairOfLocked(guest)) endPairLocked(o, guest->self_ + " took a new partner");
        owner->pair_ = ChessLobby::Pair{};
        owner->pair_.id      = owner->self_ + "-" + std::to_string(++owner->pairs_);
        owner->pair_.partner = guest->self_;
        owner->pair_.ended   = false;
        owner->waiting_ = guest->waiting_ = false;
        guest->guest_of_ = owner->self_;
        ETCS_LOG("ChessNode", "pair " << owner->pair_.id << ": " << guest->self_
                 << " sits at " << owner->self_ << "'s table");
        return stateLocked(guest);
    }

    // Quick match: somebody waiting, else wait. Oldest waiter first, so two
    // arrivals meet rather than both waiting.
    std::string pairLocked(ChessLobby* me)
    {
        endStaleLocked();
        me->hosting_ = true;
        if (pairOfLocked(me)) return stateLocked(me);
        for (const auto& [s, l] : lobbies_)
        {
            if (l == me || !l->waiting_ || !onlineLocked(l) || pairOfLocked(l)) continue;
            if (l->game_ != me->game_) continue;          // like with like
            return startPairLocked(l, me);
        }
        me->waiting_ = true;
        return stateLocked(me);
    }

    // Sitting at a named lobby -- a row in the listing, or a shared link.
    std::string visitLocked(ChessLobby* me, const std::string& owner_name)
    {
        endStaleLocked();
        me->hosting_ = true;
        ChessLobby* o = findLobbyLocked(owner_name);
        if (!o || !onlineLocked(o)) return "NO SUCH LOBBY";
        if (o == me)                return stateLocked(me);
        if (ChessLobby* live = pairOfLocked(o))
            return (live == o && o->pair_.partner == me->self_) ? stateLocked(me) : "FULL";
        return startPairLocked(o, me);
    }

    // One line into the pair's record, in the one order both boards replay.
    // A chess pair takes the verbs a chess table takes -- nothing that would
    // reach past the board (the replay is a Request on the page's node). Any
    // other game's lines are its pages' business: a verb is one lowercase
    // word, and the line is relayed unread.
    std::string pushLocked(ChessLobby* me, const std::string& id,
                           const std::string& verb, const std::string& arg)
    {
        ChessLobby* o = pairOfLocked(me);
        if (!o || o->pair_.id != id) return "NOT PAIRED";
        // "void" is the agreement's own: a board that took a line to a
        // different state than its sender (ChessNode::replayLocked).
        static const char* const kVerbs[] = { "move", "sit", "say", "resign", "draw",
                                              "decline", "leave", "reset", "void" };
        bool ok = !verb.empty() && verb.size() <= 16;
        for (char c : verb) if (c < 'a' || c > 'z') ok = false;
        if (ok && o->game_ == "chess")
        {
            ok = false;
            for (const char* v : kVerbs) if (verb == v) ok = true;
        }
        if (!ok) return "NOT FOUND";
        ChessLobby::Pair& p = o->pair_;
        const size_t seq = p.base + p.record.size();
        const std::string line = recordLine(seq, me->self_, verb, arg);
        // THE RECORD CHAIN, the share record's own (PaintNode): XXH3 of every
        // line seeded with the chain before it, kept per line so a page of
        // the record can say what the chain is at its end.
        const uint64_t before = p.chains.empty() ? p.base_chain : p.chains.back();
        p.record.push_back(line);
        p.chains.push_back(XXH3_64bits_withSeed(line.data(), line.size(), before));
        if (p.record.size() > kRelayLines)
        {
            p.base_chain = p.chains.front();
            p.record.erase(p.record.begin()); p.chains.erase(p.chains.begin()); ++p.base;
        }
        return std::to_string(seq);
    }

    // "<base> <next>\n" then whole lines from <since>, within the frame
    // budget -- ChessGame's own page shape, so a reader loops the same way.
    // Readable after the pair ends, so the last lines still arrive.
    std::string relayLocked(ChessLobby* me, const std::string& id, const std::string& since)
    {
        const size_t dash = id.rfind('-');
        ChessLobby* o = (dash == std::string::npos) ? nullptr : findLobbyLocked(id.substr(0, dash));
        if (!o || o->pair_.id != id) return "NOT PAIRED";
        if (o != me && o->pair_.partner != me->self_) return "NOT PAIRED";
        const ChessLobby::Pair& p = o->pair_;
        size_t from = ChessGame::parseIndex(since);
        if (from < p.base) from = p.base;
        size_t i = from - p.base;
        std::string body;
        while (i < p.record.size() && body.size() + p.record[i].size() + 1 <= ChessGame::kFrameBudget)
        {
            body += p.record[i]; body += "\n"; ++i;
        }
        // The third field is the chain through the page's last line: what a
        // board that replayed the record to here must have chained too.
        const uint64_t chain = (i == 0) ? p.base_chain : p.chains[i - 1];
        return std::to_string(p.base) + " " + std::to_string(p.base + i) + " "
             + ChessGame::hex64(chain) + "\n" + body;
    }

    // "<seq> <self> <verb>[ <arg>]" -- one spelling, used by the relay that
    // stores a line and by the board that replays it, so both chain the
    // same bytes.
    static std::string recordLine(size_t seq, const std::string& self,
                                  const std::string& verb, const std::string& arg)
    {
        return std::to_string(seq) + " " + self + " " + verb + (arg.empty() ? "" : " " + arg);
    }

    // The board a pair's line lands on: this runtime's, <match>, visited as
    // <self> -- the partner's name is a self here too, so its lines replay
    // as that self's verbs.
    ChessGame* pairBoardLocked(const std::string& self, const std::string& match)
    {
        ChessLobby* me = selfLocked(self);
        if (!me || match.empty()) return nullptr;
        ChessGame* g = findGameLocked(match);
        if (!g) g = createGameLocked(match);
        if (!g) return nullptr;
        me->addEdgeLocked(match, g);
        g->replayed_ = true;
        return g;
    }

    /*
     * A STEP OF YOUR OWN, drawn now: /<mount>/<self>/propose/<match>/<verb>
     * [/<arg>]. Taken on this board at once, and answered with the verb's own
     * answer and, on a line below it, "<from>.<to>" -- the state hashes
     * either side of it (ChessGame::stateHashLocked), which the page sends
     * with the line so the partner's board can check it lands the same. A
     * refusal, or a step that changes nothing, is undone and answered alone:
     * nothing to send. One step pending at a time ("BUSY"); `withdraw` takes
     * it back when the name server would not take the line.
     */
    std::string proposeLocked(const std::string& self, const std::vector<std::string>& seg)
    {
        const std::string match = (seg.size() > 3) ? seg[3] : "";
        const std::string verb  = (seg.size() > 4) ? seg[4] : "";
        const std::string arg   = (seg.size() > 5) ? seg[5] : "";
        if (verb.empty()) return "NOT FOUND";
        ChessGame* g = pairBoardLocked(self, match);
        if (!g) return "FAILED";
        if (g->pending_.on && !g->pending_.undone) return "BUSY";
        const ChessGame::Snapshot before = g->snapshotLocked();
        const uint64_t from = g->stateHashLocked();
        const std::string ans = g->verbLocked(self, verb, arg);
        const uint64_t to = g->stateHashLocked();
        if (ChessGame::refusal(ans) || to == from) { g->restoreLocked(before); return ans; }
        g->pending_ = ChessGame::Pending{ true, false, self,
                                          ChessGame::hex64(from) + "." + ChessGame::hex64(to), before };
        return ans + "\n" + g->pending_.tag;
    }

    std::string withdrawLocked(const std::string& self, const std::vector<std::string>& seg)
    {
        ChessGame* g = pairBoardLocked(self, (seg.size() > 3) ? seg[3] : "");
        if (!g) return "FAILED";
        if (g->pending_.on && !g->pending_.undone && g->pending_.self == self)
            g->restoreLocked(g->pending_.before);
        g->pending_ = ChessGame::Pending{};
        return "OK";
    }

    /*
     * A LINE OF A PAIR'S RECORD, REPLAYED ON THIS RUNTIME'S BOARD:
     * /<mount>/<self>/replay/<match>/<seq>/<verb>[/<arg>]. The line is
     * chained into the board's own record chain (ChessGame::chain_) exactly
     * as the relay chained it -- the page compares that with the relay's and
     * rebuilds the board if they differ (a line missed, not a disagreement) --
     * and then judged (ChessGame's AGREEMENT):
     *
     *   talk ("say")        applied; it changes no state.
     *   this board's own    pending step: confirmed ("OK"), already drawn.
     *   anything else       first takes back a pending step it was ordered
     *                       ahead of; then, with "<arg>~<from>.<to>": VOID if
     *                       this board is not at <from> (proposed from a state
     *                       an earlier line replaced -- void on both boards);
     *                       DISAGREE, undone, if it lands anywhere but <to>
     *                       (the page says so with a `void` line); else the
     *                       verb's answer. Untagged, applied as it stands.
     *   void/<seq>          the disagreement, in order: <seq> must be the line
     *                       just before it (other than talk) -- then its step
     *                       is taken back where it was taken ("UNDONE") -- or
     *                       the game is drawn, "DESYNC".
     */
    std::string replayLocked(const std::string& self, const std::vector<std::string>& seg)
    {
        const std::string match = (seg.size() > 3) ? seg[3] : "";
        std::string seqs        = (seg.size() > 4) ? seg[4] : "";
        const std::string verb  = (seg.size() > 5) ? seg[5] : "";
        const std::string arg   = (seg.size() > 6) ? seg[6] : "";
        if (!seqs.empty() && seqs[0] == '=') seqs.erase(0, 1);   // older pages' "already drawn"
        if (seqs.empty() || verb.empty()) return "NOT FOUND";
        ChessGame* g = pairBoardLocked(self, match);
        if (!g) return "FAILED";
        const size_t seq = ChessGame::parseIndex(seqs);
        const std::string line = recordLine(seq, self, verb, arg);
        g->chain_     = XXH3_64bits_withSeed(line.data(), line.size(), g->chain_);
        g->chain_seq_ = seq + 1;
        if (verb == "say") return g->verbLocked(self, verb, arg);

        // "<arg>~<from>.<to>", split off the end: a move's arg has no '~'.
        std::string real = arg, tag;
        if (const size_t t = arg.rfind('~'); t != std::string::npos) { real = arg.substr(0, t); tag = arg.substr(t + 1); }

        ChessGame::Pending& p = g->pending_;
        const bool mine = p.on && p.self == self && !tag.empty() && p.tag == tag;
        if (p.on && !p.undone && !mine)
        {
            g->restoreLocked(p.before); p.undone = true;
            g->logLocked(p.self + "'s last step taken back -- " + self + "'s came first");
        }

        const size_t about = ChessGame::parseIndex(real);
        const bool adjacent = g->has_line_ && g->last_line_ == about;
        // The same dispute said twice -- both boards judged a line neither
        // drew (a sender that never proposed it) -- is one dispute.
        const bool again = g->has_line_ && g->voided_.on && g->voided_.seq == about
                        && g->last_line_ == g->voided_.at;
        g->has_line_ = true; g->last_line_ = seq;

        if (verb == "void")
        {
            if (again)     { g->voided_.at = seq; return "OK"; }
            if (!adjacent) return g->desyncLocked();
            g->voided_ = ChessGame::Voided{ true, about, seq };
            if (g->last_step_.on && g->last_step_.seq == about)
            {
                g->restoreLocked(g->last_step_.before);
                g->last_step_ = ChessGame::Step{};
                g->logLocked("a step was taken back -- the boards disagreed about it");
                return "UNDONE";
            }
            return "OK";
        }
        if (mine && !p.undone)
        {
            g->last_step_ = ChessGame::Step{ true, seq, p.before };
            p = ChessGame::Pending{};
            return "OK";
        }
        if (mine) p = ChessGame::Pending{};

        const ChessGame::Snapshot before = g->snapshotLocked();
        const size_t dot = tag.find('.');
        if (!tag.empty())
        {
            if (dot == std::string::npos || tag.substr(0, dot) != ChessGame::hex64(g->stateHashLocked()))
                return "VOID";
        }
        const std::string ans = g->verbLocked(self, verb, real);
        if (!tag.empty() && (ChessGame::refusal(ans) || tag.substr(dot + 1) != ChessGame::hex64(g->stateHashLocked())))
        {
            g->restoreLocked(before);
            return "DISAGREE";
        }
        g->last_step_ = ChessGame::Step{ true, seq, before };
        return ans;
    }

    // Every lobby online here: "owner partner|- open|waiting|playing game".
    // Any game's: the node pairs and relays lines without reading them, so a
    // lobby list here is a list for every game whose pages use it.
    std::string lobbiesLocked()
    {
        endStaleLocked();
        std::string out;
        for (const auto& [s, l] : lobbies_)
        {
            if (!onlineLocked(l)) continue;
            ChessLobby* live = pairOfLocked(l);
            if (live && live != l) continue;          // sitting at somebody else's table
            out += s + " " + (live ? l->pair_.partner : std::string("-")) + " "
                 + (live ? "playing" : l->waiting_ ? "waiting" : "open") + " " + l->game_ + "\n";
        }
        return out;
    }

    // /<mount>/<self>/<match>/<verb>[/<arg>]
    // /<mount>/<self>/list | join | me
    // /<mount>/<self>/host|pair/<token>[/<game>] | unpair/<token> | visit/<token>/<owner>
    // /<mount>/<self>/push/<token>/<pair>/<verb>[/<arg>] | relay/<token>/<pair>/<since>
    // /<mount>/<self>/replay/<match>/<seq>/<verb>[/<arg>]
    // /<mount>/<self>/propose/<match>/<verb>[/<arg>] | withdraw/<match>
    // /<mount>/players | rooms | lobbies
    //
    // "players", "rooms" and "lobbies" are reserved selves; "list", "join",
    // "me" and the name-server verbs are reserved matches.
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
        if (self == "lobbies") return lobbiesLocked();

        const std::string match = (seg.size() > 2) ? seg[2] : "";

        /*
         * ONE PAGE PER NAME. The name-server verbs carry the page's token
         * (seg[3]); a name held by another token whose page is still online
         * is refused, and refused BEFORE the self is touched -- touching it
         * would keep the holder's name alive on the impostor's calls. A name
         * whose page has gone quiet (kOnline) is free to the next token.
         */
        const bool ns_verb = (match == "host" || match == "pair" || match == "visit" || match == "unpair"
                              || match == "push" || match == "relay");
        const std::string token = (ns_verb && seg.size() > 3) ? seg[3] : "";
        if (ns_verb)
        {
            if (token.empty()) return "NO TOKEN";
            ChessLobby* held = findLobbyLocked(self);
            if (held && !held->claim_.empty() && held->claim_ != token && onlineLocked(held))
                return "NAME TAKEN";
        }

        // A page's own board, replaying a line of its pair's record, or
        // drawing its own step ahead of it: see replayLocked and
        // proposeLocked. Reserved like the name-server verbs.
        if (match == "replay")   return replayLocked(self, seg);
        if (match == "propose")  return proposeLocked(self, seg);
        if (match == "withdraw") return withdrawLocked(self, seg);

        ChessLobby* me = selfLocked(self);
        if (!me) return "FAILED";
        if (ns_verb && me->claim_ != token)
        {
            if (!me->claim_.empty()) ETCS_LOG("ChessNode", "name '" << self << "' was quiet -- a new page holds it now");
            me->claim_ = token;
        }

        if (match.empty() || match == "list") return me->listLocked();
        if (match == "me")                    return me->profileLocked();
        if (match == "join")                  return joinLocked(self);

        // The name-server verbs: a lobby hosted by a page, paired, relayed.
        // See ChessLobby's Pair.
        auto at = [&](size_t i) { return (seg.size() > i) ? seg[i] : std::string(); };
        // The game a lobby is for rides on host and pair (seg[4]): the listing
        // says it, and quick match pairs like with like. Chess when unsaid.
        if ((match == "host" || match == "pair") && !at(4).empty()) me->game_ = at(4);
        if (match == "host")   { endStaleLocked(); me->hosting_ = true; return stateLocked(me); }
        if (match == "pair")   return pairLocked(me);
        if (match == "visit")  return visitLocked(me, at(4));
        if (match == "unpair") { endStaleLocked(); if (ChessLobby* o = pairOfLocked(me)) endPairLocked(o, self + " left");
                                 me->waiting_ = false; return stateLocked(me); }
        if (match == "push")   return pushLocked(me, at(4), at(5), at(6));
        if (match == "relay")  return relayLocked(me, at(4), at(5));

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

    if (over_ == "draw" || over_ == "desync" || stalemate_)
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
                case ChessInEvent::Kind::History:  result = g->historyPageLocked(ChessGame::parseIndex(arg)); break;
                case ChessInEvent::Kind::Leave:    result = g->leaveLocked(tok);
                                                   g->reapLocked(30);                     break;
                case ChessInEvent::Kind::Say:      g->sayLocked(tok, arg);
                                                   result = "OK";                         break;
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

    // Drop, not Inline: the caller was released above (e.done->store) and
    // nothing downstream consumes an emit, so the slot is handed straight back
    // -- see GapReorderBuffer::abandon.
    //
    // The mask is no longer part of this return; it is resolved before dispatch
    // by mask_for, and this stream does not define one, so it inherits
    // EventStream's all(). Fail-shut, and exactly today's behaviour: every
    // event ordered against every other, which is what one ordering thread
    // already gave.
    //
    // Narrowing it means writing ChessStream::mask_for, and the node's own
    // ordering-domain comment above is the warning label. Cross-node
    // independence is not this mask's job -- that comes from one stream per
    // node. Within a node, a tag bit names a DECLARED TYPE while the mask must
    // name what an op TOUCHES: roomsLocked reads liveTokensLocked/over_/
    // checkmate_/started_ across every game, reapLocked walks them and calls
    // Delete(), ChessLobby::listLocked reads white_/black_ off boards it does
    // not own. TAG_CLOSURE (ETCS_API.h) covers that -- ChessNode
    // addTag<ChessGame>()s and addTag<ChessLobby>()s, so its closure already
    // carries both bits -- which leaves mask_for here little more than
    // returning e.target's myTagClosure().
    return { ETCS::DispatchKind::Drop, nullptr };
}

#endif // CHESSNODE_H__
