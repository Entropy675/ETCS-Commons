#ifndef CHESSGAME_H__
#define CHESSGAME_H__
#include "../../../ontology.h"

// The CChess engine, a folder inside this module's own type folder so this
// header has somewhere to live beside it. Plain, dependency-free C++: no ETCS
// types, and no platform split (the ncurses/Windows fork lived entirely in the
// View, which the runtime replaces).
#include "CChess/src/Board.h"
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

/*
 * ── ChessGame ─────────────────────────────────────────────────────────────
 *
 * A board is a position; this owns seats, turn ownership, the outcome and the
 * conversation, and DELEGATES the position to CChess.
 *
 * THE RECORD IS THE GAME. In a session every line that changes a board -- a
 * move, a seat, a resignation, a draw, a word -- is a line of ONE record
 * (a NetworkProvider::Ledger on the host's runtime, ontology/Record.h), and
 * every board is what that record says, replayed in its order. There is no
 * agreement protocol, because there is nothing to agree: two boards fed the
 * same lines by the same engine are the same board.
 *
 * ONE BOARD JUDGES. The host's board is the one in front of the record. A
 * guest's line goes to the host's PROPOSALS ledger (`game.Emit() ->
 * proposals.Take()`, authored there by the link it came over), the judge
 * follows that ledger (`proposals.Follow(0) -> game.Judge()`), applies each
 * line as its author, and appends to the record only what the board took.
 * So the record holds what happened and nothing else -- a refused move never
 * becomes a line anybody replays -- and the proposals ledger is checkpointed
 * behind the judge, so it holds nothing for long. Every other board FOLLOWS:
 * its verbs propose (Emit), and it changes only when the record comes back
 * through `Absorb`. Your own move therefore lands after one round trip
 * through the host, and two boards cannot disagree even for a moment.
 *
 * WHO THE JUDGE IS decides the arrangement and nothing else: in a game
 * between two people, one of them hosts and their board judges; in a ranked
 * game the ACE server hosts -- an ETCS runtime like theirs, with a board that
 * holds no seat, judges every line and keeps the outcome. Same type, same
 * streams, same scripts. A spectator is a follower with no seat; a player is
 * a follower with one; the ranking server is the judge with none.
 *
 * WHO A LINE IS BY is the link it came over: the proposals ledger authors it
 * with the link's name, never with anything the line says, and the judge
 * writes that name into the record. Seats are held by name. Alone, with no session, the two
 * seats are the two colours and a click moves as whichever is to move: the
 * hotseat, where the board is its own judge.
 */
class ChessGame :
    public EphemeralBase<ChessGame>,
    public DeletableBase<ChessGame>
{
public:
    WIRE_TYPE_IDENTITY(ChessGame);

    ChessGame()  { board.setStartingBoard(true); refreshTerminal(); recordHistoryLocked(""); }
    virtual ~ChessGame() = default;

    // ── the session ───────────────────────────────────────────────────────
    // This board's name in a session. Empty: the hotseat, its own judge.
    void Session(const std::string& me) { std::lock_guard<std::mutex> hold(mu_); me_ = me; }
    /*
     * ENTER A SESSION FROM A KNOWN POSITION. A follower is its record replayed
     * from the base, and the base is the standard start (the first line is a
     * move from it) -- so a board that carried a previous game's position into
     * a new session would apply the record's moves to the wrong board and
     * diverge from it. Called by ChessShare::Host and ::Join: the seq is put
     * back to 0 so the first Absorb aligns, the outbox is emptied so nothing
     * from before is proposed, and the chat is kept (the people may be the
     * same). The host's board is the record's authority and starts here too.
     */
    void Restart()
    {
        std::lock_guard<std::mutex> hold(mu_);
        resetLocked();
        seq_ = 0;
        outbox_.clear();
        recorded_ = false;
    }
    // This board judges: what it takes goes into `record`, and `proposals`
    // (both Records) is what it judges from -- checkpointed behind it.
    void Judge(ETCS::RID record, ETCS::RID proposals)
    {
        std::lock_guard<std::mutex> hold(mu_);
        record_    = record;
        proposals_ = proposals;
        judge_     = record != 0;
    }
    bool judge() const { return judge_; }
    const std::string& me() const { return me_; }

    /*
     * A VERB FROM HERE. The hotseat and the judge apply it and answer as the
     * board does (a FEN, a status line, OK, or a refusal in capitals); a
     * follower proposes it -- queued for Emit -- and answers SENT, since the
     * answer is the record's to give. `as` is the hotseat's colour; in a
     * session it is this board's own name.
     */
    std::string Act(const std::string& as, const std::string& verb, const std::string& arg)
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (me_.empty()) return verbLocked(as, verb, arg);
        if (judge_)      return judgeLocked(me_, verb, arg);
        outbox_.push_back(verb + (arg.empty() ? "" : " " + arg));
        return "SENT";
    }

    /*
     * A PROPOSAL (the Judge stream): "<seq> <author> <verb> [<arg>]" from the
     * proposals ledger, applied as its author and appended to the record if
     * the board took it. A refusal is the proposer's to see through their own
     * board not changing. The proposals ledger is checkpointed past it.
     */
    void JudgeLine(const std::string& msg)
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (!judge_ || msg.compare(0, 2, "~ ") == 0) return;
        uint64_t seq = 0; std::string author, verb, arg;
        if (!parseLine(msg, seq, author, verb, arg) || author.empty()) return;
        judgeLocked(author, verb, arg);
        if (Record_* p = recordOf(proposals_)) p->Checkpoint(seq + 1);
    }

    /*
     * A LINE OF THE RECORD, followed (the Absorb stream): "<seq> <author>
     * <verb> [<arg>]", applied as its author. A restart ("~ <seq> <chain>",
     * the record starting later than asked) moves the seq. A line the board
     * refuses here was taken by the judge from a board that had it -- so a
     * refusal is logged as a disagreement, which the presence hashes will
     * also show; it is not repaired, because there is nothing to repair it
     * from but the record itself.
     */
    void Absorb(const std::string& msg)
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (msg.compare(0, 2, "~ ") == 0)
        {
            seq_ = std::strtoull(msg.c_str() + 2, nullptr, 10);
            return;
        }
        uint64_t seq = 0; std::string author, verb, arg;
        if (!parseLine(msg, seq, author, verb, arg)) return;
        const std::string r = verbLocked(author, verb, arg);
        seq_ = seq + 1;
        if (refusal(r) && verb != "leave")
            ETCS_LOG("ChessGame", "line " << seq << " (" << author << " " << verb << " " << arg
                     << ") refused here: " << r << " -- this board is not the record's.");
    }

    // Emit's queue: the next proposed line, or false.
    bool nextEmit(std::string& out)
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (outbox_.empty()) return false;
        out = std::move(outbox_.front());
        outbox_.pop_front();
        return true;
    }

    // ── reads ─────────────────────────────────────────────────────────────
    std::string Fen() const                          { std::lock_guard<std::mutex> hold(mu_); return board.toFENString(); }
    std::string StatusLine(const std::string& as) const { std::lock_guard<std::mutex> hold(mu_); return statusLineLocked(as); }
    std::string ChatPage(const std::string& from) const
    {
        std::lock_guard<std::mutex> hold(mu_);
        return from.empty() ? chatLogLocked() : pageLocked(chat_, chat_base_, parseIndex(from));
    }
    std::string HistoryPage(const std::string& from) const
    {
        std::lock_guard<std::mutex> hold(mu_);
        return pageLocked(history_, history_base_, parseIndex(from));
    }
    std::string Seats() const
    {
        std::lock_guard<std::mutex> hold(mu_);
        return std::string("white:") + (white_.empty() ? "open" : "taken")
             + " black:" + (black_.empty() ? "open" : "taken");
    }
    /*
     * WHAT THE BOARD IS, as one number: the position, the seats, the offer
     * and the outcome -- what two boards fed the same record must agree on
     * -- and the record seq it is the board of. Sent with presence, so the
     * members compare boards without a verb for it (ChessShare::roster).
     */
    std::string Hash() const
    {
        std::lock_guard<std::mutex> hold(mu_);
        const std::string s = board.toFENString() + "|" + white_ + "|" + black_ + "|" + draw_offer_ + "|" + over_;
        return hex64(XXH3_64bits(s.data(), s.size())) + " " + std::to_string(seq_);
    }
    std::string LoadFenStr(const std::string& fen)
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (!board.loadFEN(fen)) return "INVALID";
        refreshTerminal();
        // A loaded position is a new root, not a continuation: the plies before
        // it never happened on this board and stepping back into them would
        // show a line that does not lead here.
        history_.clear();
        history_base_ = 0;
        recordHistoryLocked("");
        return board.toFENString();
    }
    // Narration from outside the record: who arrived, who left (ChessShare).
    void Note(const std::string& text) { std::lock_guard<std::mutex> hold(mu_); logLocked(text); }

    // For the board that draws this and the table that clicks on it.
    std::string LastMove() const
    {
        std::lock_guard<std::mutex> hold(mu_);
        if (history_.empty()) return "";
        const std::string& h = history_.back();
        const size_t sp = h.find(' ');
        const std::string uci = sp == std::string::npos ? h : h.substr(0, sp);
        return uci == "-" ? "" : uci;
    }
    std::string Me() const     { std::lock_guard<std::mutex> hold(mu_); return me_; }
    // The last `n` lines of the conversation, one per line, for a pane that
    // shows a fixed number of rows.
    std::string ChatTail(size_t n) const
    {
        std::lock_guard<std::mutex> hold(mu_);
        std::string out;
        for (size_t i = chat_.size() > n ? chat_.size() - n : 0; i < chat_.size(); ++i) { out += chat_[i]; out += "\n"; }
        return out;
    }
    bool WhiteToMove() const   { std::lock_guard<std::mutex> hold(mu_); return board.isWhiteTurn(); }
    bool Over() const          { std::lock_guard<std::mutex> hold(mu_); return !over_.empty() || checkmate_ || stalemate_; }
    // The piece on a square as its FEN letter, ' ' for none, 0 off the board.
    char PieceAt(const std::string& sq) const
    {
        if (sq.size() < 2 || sq[0] < 'a' || sq[0] > 'h' || sq[1] < '1' || sq[1] > '8') return 0;
        const std::string fen = Fen();
        int row = 0, file = 0;
        const int want_row = '8' - sq[1], want_file = sq[0] - 'a';
        for (char c : fen)
        {
            if (c == ' ') break;
            if (c == '/') { ++row; file = 0; continue; }
            if (c >= '1' && c <= '8') { if (row == want_row && want_file >= file && want_file < file + (c - '0')) return ' '; file += c - '0'; continue; }
            if (row == want_row && file == want_file) return c;
            ++file;
        }
        return ' ';
    }

    const std::string& White() const { return white_; }
    const std::string& Black() const { return black_; }

    // ── EphemeralBase / DeletableBase ─────────────────────────────────────
    bool ResetConcrete() override { return Act(me_.empty() ? "white" : me_, "reset", "") != "GAME OVER"; }
    bool IsActiveConcrete() const override { return active_; }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                   + this->getSourceTag().toString();
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

    // Decode one path segment. Browsers encode content-bearing args with
    // encodeURIComponent; without this, spaces arrive as literal %20.
    static std::string percentDecode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size())
            {
                const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(in[i]);
        }
        return out;
    }

private:
    // Raised from 40 because the log carries narration as well as speech:
    // arrivals, seat claims, every move, and the outcome.
    static constexpr size_t kChatLines = 200;

    // A verb's answer is one ETCS::Buffer (writeString clears it first and
    // leaves it EMPTY if the text does not fit), so the growing verbs answer in
    // PAGES: a "<base> <next>" header and then as many whole lines as fit.
    // 200 rather than a number derived from bufsize because this module
    // should not encode a constant it does not own.
    static constexpr size_t kFrameBudget = 200;

    // 600 plies is past the longest recorded tournament game.
    static constexpr size_t kHistoryPlies = 600;

    // A record's line: "<seq> <author> <verb> [<arg...>]"
    static bool parseLine(const std::string& msg, uint64_t& seq, std::string& author,
                          std::string& verb, std::string& arg)
    {
        const size_t a = msg.find(' ');
        const size_t b = a == std::string::npos ? a : msg.find(' ', a + 1);
        if (b == std::string::npos) return false;
        seq    = std::strtoull(msg.c_str(), nullptr, 10);
        author = msg.substr(a + 1, b - a - 1);
        const std::string line = msg.substr(b + 1);
        const size_t sp = line.find(' ');
        verb = line.substr(0, sp);
        arg  = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        while (!arg.empty() && arg.back() == '\r') arg.pop_back();
        return !verb.empty();
    }
    static Record_* recordOf(ETCS::RID rid)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        void* rec = e ? e->getInterfacePointer(ETCS::Buffer("Record")) : nullptr;
        return static_cast<Record_*>(rec);
    }

    // Every refusal here is upper case ("ILLEGAL", "NOT YOUR TURN", "GAME
    // OVER", "TAKEN" ...); what a verb takes answers with a FEN, a status
    // line or "OK".
    static bool refusal(const std::string& a)
    {
        if (a.empty() || a == "OK") return false;
        for (char c : a) if (!(c == ' ' || (c >= 'A' && c <= 'Z'))) return false;
        return true;
    }

    // The judge: applied here, and into the record if taken. A read verb
    // answers and is not a line.
    std::string judgeLocked(const std::string& author, const std::string& verb, const std::string& arg)
    {
        const std::string r = verbLocked(author, verb, arg);
        if (refusal(r) || !changes(verb)) return r;
        Record_* rec = recordOf(record_);
        if (!rec) { ETCS_LOG("ChessGame", "the record is gone -- " << verb << " taken here only."); return r; }
        const uint64_t seq = rec->Append(author, verb + (arg.empty() ? "" : " " + arg));
        if (seq != UINT64_MAX) seq_ = seq + 1;
        return r;
    }
    // The verbs that are lines of the record; the rest read.
    static bool changes(const std::string& v)
    {
        return v == "move" || v == "sit" || v == "say" || v == "resign" || v == "draw"
            || v == "decline" || v == "leave" || v == "reset";
    }

    // "<base> <next>\n" then src[from - base ...], stopping before the budget.
    // Whole lines only: half a line is not a thing any reader here can use.
    std::string pageLocked(const std::vector<std::string>& src, size_t base, size_t from) const
    {
        if (from < base) from = base;               // caller fell behind the ring
        size_t i = (from - base < src.size()) ? (from - base) : src.size();
        std::string body;
        while (i < src.size() && body.size() + src[i].size() + 1 <= kFrameBudget)
        {
            body += src[i];
            body += "\n";
            ++i;
        }
        // A single line longer than the whole budget would otherwise never be
        // sent and the reader would stall on it forever. Ship it clipped.
        if (body.empty() && i < src.size())
        {
            body = src[i].substr(0, kFrameBudget);
            body += "\n";
            ++i;
        }
        return std::to_string(base) + " " + std::to_string(base + i) + "\n" + body;
    }

    // One line per ply: "<uci> <fen>", oldest first, index 0 being the position
    // the game STARTED from and carrying "-" for its move.
    void recordHistoryLocked(const std::string& uci)
    {
        history_.emplace_back((uci.empty() ? "-" : uci) + " " + board.toFENString());
        if (history_.size() > kHistoryPlies) { history_.erase(history_.begin()); ++history_base_; }
    }

    bool resetLocked()
    {
        board.setStartingBoard(true);
        last = ChessStatus::SUCCESS;
        white_.clear(); black_.clear();
        over_.clear(); draw_offer_.clear();
        started_ = false;
        refreshTerminal();
        // The chat SURVIVES a reset while the history does not: the position
        // is a new game, but the people are the same people.
        history_.clear();
        history_base_ = 0;
        recordHistoryLocked("");
        logLocked("new game -- seats are open");
        return true;
    }

    // Claim by moving: the seat for the side to move is free, and this name
    // does not already hold the OTHER seat -- so one person cannot quietly
    // become both players. The hotseat's names are the colours themselves.
    std::string applyMoveLocked(const std::string& mv, const std::string& who)
    {
        if (!over_.empty()) return "GAME OVER";
        if (mv.size() < 4)  return "ILLEGAL";

        const bool white_moved = board.isWhiteTurn();
        std::string& seat        = white_moved ? white_ : black_;
        const std::string& other = white_moved ? black_ : white_;
        if (seat.empty())
        {
            if (who == other) return "NOT YOUR TURN";
        }
        else if (seat != who) return (who == other) ? "NOT YOUR TURN" : "NOT YOUR SEAT";

        Pos from(mv[0] - 'a', 8 - (mv[1] - '0'));
        Pos to  (mv[2] - 'a', 8 - (mv[3] - '0'));

        ChessStatus st = board.movePiece(from, to);
        last = st;
        if (st == ChessStatus::PROMOTE && mv.size() >= 5)
        {
            std::string promo(1, mv[4]);
            board.registerPromotion(promo);
        }
        if (st == ChessStatus::FAIL) return "ILLEGAL";

        // The seat is claimed by a LEGAL move: a refused one claims nothing,
        // so a line the judge refuses leaves the record and the seats as
        // they were.
        if (seat.empty()) { seat = who; logLocked(who + " sits as " + (white_moved ? "white" : "black")); }

        const bool first_move = !started_;
        started_ = true;
        // Moving answers a pending offer: playing on IS declining.
        draw_offer_.clear();
        refreshTerminal();
        recordHistoryLocked(mv);

        if (first_move) logLocked("game started");
        logLocked(std::string(white_moved ? "white" : "black") + " plays " + mv
                  + (checkmate_ ? "#" : (board.sideToMoveInCheck() ? "+" : "")));
        if (!active_)
        {
            if      (checkmate_) logLocked(std::string(white_moved ? "white" : "black") + " wins by checkmate");
            else if (stalemate_) logLocked("stalemate -- drawn");
            outcomeLocked();
        }
        return board.toFENString();
    }

    // Resigning and offering a draw are seated privileges: a spectator has
    // nothing to give up. Both refuse a finished game rather than overwriting
    // its outcome.
    std::string resignLocked(const std::string& who)
    {
        const std::string role = roleOfLocked(who);
        if (role == "viewer") return "NOT YOUR SEAT";
        if (!over_.empty())   return "GAME OVER";
        over_ = "resign-" + role;
        draw_offer_.clear();
        logLocked(role + " resigns -- " + (role == "white" ? "black" : "white") + " wins");
        refreshTerminal();
        outcomeLocked();
        return statusLineLocked(who);
    }

    // One verb for offer AND accept: an offer standing from the OTHER player
    // makes this an acceptance, otherwise it records yours.
    std::string drawLocked(const std::string& who)
    {
        const std::string role = roleOfLocked(who);
        if (role == "viewer") return "NOT YOUR SEAT";
        if (!over_.empty())   return "GAME OVER";
        if (!draw_offer_.empty() && draw_offer_ != who)
        {
            over_ = "draw";
            draw_offer_.clear();
            logLocked(role + " accepts -- drawn by agreement");
            refreshTerminal();
            outcomeLocked();
            return statusLineLocked(who);
        }
        if (draw_offer_ != who) logLocked(role + " offers a draw");
        draw_offer_ = who;
        return statusLineLocked(who);
    }

    std::string declineLocked(const std::string& who)
    {
        if (roleOfLocked(who) == "viewer") return "NOT YOUR SEAT";
        if (!draw_offer_.empty() && draw_offer_ != who)
        {
            draw_offer_.clear();
            logLocked(roleOfLocked(who) + " declines the draw");
        }
        return statusLineLocked(who);
    }

    // A seat given up: one's own, or -- from the owner, who saw the link
    // close -- somebody else's ("leave <name>").
    std::string leaveLocked(const std::string& who, const std::string& arg)
    {
        std::string gone = who;
        if (!arg.empty() && arg != who)
        {
            if (who != owner_) return "NOT YOUR SEAT";
            gone = arg;
        }
        if (gone.empty()) return "OK";
        if (draw_offer_ == gone) draw_offer_.clear();
        if (white_ == gone)      { white_.clear(); logLocked(gone + " left -- white seat is open"); }
        else if (black_ == gone) { black_.clear(); logLocked(gone + " left -- black seat is open"); }
        return "OK";
    }

    std::string roleOfLocked(const std::string& who) const
    {
        if (!who.empty() && who == white_) return "white";
        if (!who.empty() && who == black_) return "black";
        return "viewer";
    }

    // side, state, this name's role, each seat, the draw offer relative to
    // the asker, then WHO holds each seat ('-' for nobody).
    std::string statusLineLocked(const std::string& who) const
    {
        std::string s = board.isWhiteTurn() ? "w" : "b";
        if      (!over_.empty())            s += " " + over_;
        else if (checkmate_)                s += " checkmate";
        else if (stalemate_)                s += " stalemate";
        else if (board.sideToMoveInCheck()) s += " check";
        else                                s += " ok";
        s += " " + roleOfLocked(who);
        s += white_.empty() ? " open" : " taken";
        s += black_.empty() ? " open" : " taken";
        if      (draw_offer_.empty())  s += " none";
        else if (draw_offer_ == who)   s += " mine";
        else                           s += " theirs";
        s += " " + (white_.empty() ? std::string("-") : white_);
        s += " " + (black_.empty() ? std::string("-") : black_);
        return s;
    }

    // Taking a seat without moving. Same rule as the move's claim: a seat
    // somebody holds is theirs, and one name never holds both.
    std::string sitLocked(const std::string& who, const std::string& side)
    {
        if (who.empty())                         return "NOT YOUR SEAT";
        if (side != "white" && side != "black")  return "NOT FOUND";
        if (!over_.empty())                      return "GAME OVER";
        std::string& seat        = (side == "white") ? white_ : black_;
        const std::string& other = (side == "white") ? black_ : white_;
        if (seat == who)   return "OK";
        if (!seat.empty()) return "TAKEN";
        if (other == who)  return "NOT YOUR SEAT";
        seat = who;
        logLocked(who + " sits as " + side);
        return "OK";
    }

    static std::string hex64(uint64_t v)
    {
        char b[17];
        std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
        return b;
    }

    static size_t parseIndex(const std::string& s)
    {
        size_t n = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + static_cast<size_t>(c - '0');
            if (n > 100000000u) return 0;
        }
        return n;
    }

    void sayLocked(const std::string& who, const std::string& text)
    {
        const std::string msg = percentDecode(text);
        if (msg.empty()) return;
        chat_.push_back((who.empty() ? std::string("viewer") : who) + ": " + msg);
        if (chat_.size() > kChatLines) { chat_.erase(chat_.begin()); ++chat_base_; }
    }

    // Narration goes into the SAME ring as speech, marked "* " -- a prefix
    // no "<name>:" line can produce.
    void logLocked(const std::string& text)
    {
        chat_.push_back("* " + text);
        if (chat_.size() > kChatLines) { chat_.erase(chat_.begin()); ++chat_base_; }
    }

    // What a caller with no cursor gets: the TAIL, not the whole log.
    std::string chatLogLocked() const
    {
        size_t from = chat_base_;
        std::string out = pageLocked(chat_, chat_base_, from);
        while (true)
        {
            size_t next = from + 1;
            if (next >= chat_base_ + chat_.size()) break;
            std::string cand = pageLocked(chat_, chat_base_, next);
            if (cand.size() < out.size() && next + 1 >= chat_base_ + chat_.size()) { out = cand; break; }
            out = cand;
            from = next;
        }
        return out;
    }

    // The outcome, once: where a ranking server counts it (ETCS_LOG for now;
    // a Persistence child later).
    void outcomeLocked()
    {
        if (recorded_) return;
        recorded_ = true;
        ETCS_LOG("ChessGame", "outcome: " << (over_.empty() ? (checkmate_ ? "checkmate" : "stalemate") : over_)
                 << " white=" << white_ << " black=" << black_);
    }

    // The verb surface, one entry point: the hotseat, the judge and Absorb
    // all come through here, so a line means the same on every board.
    std::string verbLocked(const std::string& who, const std::string& verb, const std::string& arg)
    {
        if (verb == "move")    return applyMoveLocked(arg, who);
        if (verb == "fen" || verb.empty()) return board.toFENString();
        if (verb == "status")  return statusLineLocked(who);
        if (verb == "say")     { sayLocked(who, arg); return "OK"; }
        if (verb == "chat")    return arg.empty() ? chatLogLocked() : pageLocked(chat_, chat_base_, parseIndex(arg));
        if (verb == "history") return pageLocked(history_, history_base_, parseIndex(arg));
        if (verb == "sit")     return sitLocked(who, arg);
        if (verb == "resign")  return resignLocked(who);
        if (verb == "draw")    return drawLocked(who);
        if (verb == "decline") return declineLocked(who);
        if (verb == "leave")   return leaveLocked(who, arg);
        if (verb == "reset")   { resetLocked(); recorded_ = false; return board.toFENString(); }
        return "NOT FOUND";
    }

    // Recompute the terminal flags where the answer can change, which is
    // what keeps IsActiveConcrete const without making the Board mutable.
    void refreshTerminal()
    {
        checkmate_ = board.isCheckmate();
        stalemate_ = board.isStalemate();
        active_    = over_.empty() && !(checkmate_ || stalemate_);
    }

public:
    // The session's owner, whose `leave <name>` releases another's seat.
    void Owner(const std::string& owner) { std::lock_guard<std::mutex> hold(mu_); owner_ = owner; }

private:
    mutable std::mutex mu_;
    ::Board     board;
    ChessStatus last = ChessStatus::SUCCESS;

    std::string me_;                         // this board's name in a session
    std::string owner_;                      // the session's host
    bool        judge_  = false;             // in front of the record
    ETCS::RID   record_ = 0, proposals_ = 0;
    uint64_t    seq_    = 0;                 // the record seq this board is at
    std::deque<std::string> outbox_;         // a follower's proposals, for Emit

    std::string white_, black_;              // seat holders, by name ("" = open)
    std::string over_;                       // "" | resign-white | resign-black | draw
    std::string draw_offer_;                 // who offered ("" = none)

    bool        started_   = false;
    bool        recorded_  = false;
    bool        checkmate_ = false;
    bool        stalemate_ = false;
    bool        active_    = true;

    std::vector<std::string> chat_;
    std::vector<std::string> history_;       // "<uci> <fen>" per ply, root first
    size_t chat_base_ = 0, history_base_ = 0;
};

#endif // CHESSGAME_H__
