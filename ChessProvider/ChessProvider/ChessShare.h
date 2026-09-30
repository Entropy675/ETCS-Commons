#ifndef CHESSSHARE_H__
#define CHESSSHARE_H__
#include "ChessGame.h"
#include <atomic>
#include <map>
#include <sstream>

/*
 * ── ChessShare: this runtime's part in a game between runtimes ───────────
 *
 * The game itself is the record and the judge (ChessGame's header note); this
 * keeps what is beside it: who is here, what board each of them has, and the
 * word to the page. Wired by chess_host.etcs / chess_join.etcs:
 *
 *   record      the host's Ledger, published to be READ (Head Since Follow):
 *               every board follows it (record.Follow -> game.Absorb)
 *   proposals   the host's Ledger, published to be WRITTEN into (Take): a
 *               guest's lines go in here, authored by their link, and the
 *               judge follows it (proposals.Follow -> game.Judge)
 *   presence    a Lobby: each member advertises its seat and its board's
 *               hash at the record seq it is at; watched by everyone
 *               (presence.Watch -> share.Roster). An entry goes with its
 *               link, so a departure is seen the moment it happens, and the
 *               host releases the seat it held (a `leave <name>` line).
 *
 * THE HALL is the same shape one level up: the site's runtime publishes a
 * Lobby (the name server) that hosts advertise their game in -- "<name>
 * <kind> <id> <white> <black>", kept current from here (Tick) -- and everyone
 * lists (chess_hall.etcs). A casual game is advertised by the tab hosting it
 * and its entry goes when that tab's link does; a ranked table is advertised
 * by the site's own runtime, which hosts and judges it with a board that
 * holds no seat, and resets it once the game is over and the players have
 * gone (roster). The hall is a service of its own: a site that runs only the
 * hall still finds casual games their partners, and judges nothing.
 *
 * A BOARD THAT DIFFERS is reported, not repaired: two boards at the same
 * record seq with different hashes ran the same lines through the same engine
 * to different ends, which is a bug to see, not a state to negotiate. The
 * ranking server's board is the judge's, so what it counts is what happened.
 */
class ChessShare : public DeletableBase<ChessShare>
{
public:
    WIRE_TYPE_IDENTITY(ChessShare);

    ChessShare() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    void Attach(ETCS::RID game) { m_game = game; }

    // `hall` (a Lobby, or a surface of the site's) is where the game is
    // advertised as `kind` under `id`; 0 for a game found by link alone.
    bool Host(ETCS::RID record, ETCS::RID proposals, ETCS::RID presence, const std::string& name,
              ETCS::RID hall = 0, const std::string& kind = "chess", const std::string& id = "")
    {
        ChessGame* g = game();
        if (!g || name.empty()) return false;
        Leave();
        m_name = name; m_owner = name; m_presence = presence; m_host = true;
        m_hall = hall; m_kind = kind; m_id = id; m_had_guest = false;
        g->Restart();
        g->Session(name);
        g->Owner(name);
        g->Judge(record, proposals);
        ETCS_LOG("ChessShare", "hosting a game as " << name);
        return true;
    }
    /*
     * A TABLE THE SITE HOSTS (chess_ranked_table.etcs): the same six
     * entities a person's table is, with this runtime naming it -- "table1",
     * "table2", ... in the order they are made -- because a script that is
     * run once per table carries no word to name it with (`run` binds names
     * to entities, not to words), and the name is what the record is
     * authored as, what the room is called on the hub, and what the hall
     * lists. Hosted as `ranked`, its board holding no seat.
     */
    bool Serve(ETCS::RID record, ETCS::RID proposals, ETCS::RID presence, ETCS::RID room,
               ETCS::RID links, ETCS::RID hall)
    {
        static std::atomic<unsigned> count{0};
        const std::string name = "table" + std::to_string(++count);
        ETCS::Buffer author; author.writeString(name.c_str());
        callOn(record, "Ledger.Author", author);
        ETCS::Buffer rname; rname.writeString(name.c_str());
        callOn(room, "Room.SetName", rname);
        if (!Host(record, proposals, presence, name, hall, "ranked", name)) return false;
        ETCS::Buffer host; host.writeString((std::to_string(links) + " " + name).c_str());
        callOn(room, "Room.Host", host);
        Tick();
        ETCS_LOG("ChessShare", name << " is up: a ranked table, judged here, listed in the hall");
        return true;
    }
    bool Join(ETCS::RID presence, const std::string& name, const std::string& owner)
    {
        ChessGame* g = game();
        if (!g || name.empty()) return false;
        Leave();
        m_name = name; m_owner = owner; m_presence = presence; m_host = false;
        g->Restart();
        g->Session(name);
        g->Owner(owner);
        g->Judge(0, 0);
        ETCS_LOG("ChessShare", "joined " << owner << "'s game as " << name);
        return true;
    }
    void Leave()
    {
        if (m_name.empty()) return;
        if (ChessGame* g = game()) { g->Session(""); g->Owner(""); g->Judge(0, 0); }
        if (m_hall) { ETCS::Buffer arg; arg.writeString(m_name.c_str()); callOn(m_hall, "Lobby.Withdraw", arg); }
        m_name.clear(); m_owner.clear(); m_here.clear(); m_sent.clear(); m_advertised.clear();
        m_presence = 0; m_hall = 0; m_host = false;
    }
    bool inSession() const { return !m_name.empty(); }
    const std::string& name()  const { return m_name; }
    const std::string& owner() const { return m_owner; }

    // A proposal judged HERE, with the hall and presence brought up to date
    // after it (proposals.Follow -> share.Judge): a table the site hosts has
    // no page to beat Tick, and a seat changes only through a judged line or
    // a departure (roster), so these two are where the entry is kept current.
    void judge(const std::string& line)
    {
        if (ChessGame* g = game()) g->JudgeLine(line);
        Tick();
    }

    // Once a second from the page, and after every judged line and every
    // change of the roster: this board, as one line of presence -- "<seat>
    // <hash> <seq>" -- and the game in the hall, each when it changed.
    void Tick()
    {
        ChessGame* g = game();
        if (!g || !inSession()) return;
        const std::string seat = g->White() == m_name ? "white" : g->Black() == m_name ? "black" : "-";
        const std::string line = seat + " " + g->Hash();
        if (line != m_sent)
        {
            m_sent = line;
            ETCS::Buffer arg;
            arg.writeString((m_name + " board " + line).c_str());
            callOn(m_presence, "Lobby.Advertise", arg);
        }
        // And the game in the hall, with its seats, as they stand.
        if (m_host && m_hall)
        {
            const std::string ad = m_kind + " " + (m_id.empty() ? "-" : m_id)
                                 + " " + (g->White().empty() ? "open" : g->White())
                                 + " " + (g->Black().empty() ? "open" : g->Black());
            if (ad != m_advertised)
            {
                m_advertised = ad;
                ETCS::Buffer arg;
                arg.writeString((m_name + " " + ad).c_str());
                callOn(m_hall, "Lobby.Advertise", arg);
            }
        }
    }

    // The listing, whole, on every change: arrivals and departures narrated
    // on the board, a departed member's seat released by the host, and
    // boards compared against the host's.
    void roster(const std::string& listing)
    {
        ChessGame* g = game();
        std::map<std::string, std::string> now;
        std::istringstream in(listing);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream ls(line);
            std::string who, kind, info;
            if (!(ls >> who >> kind) || kind != "board") continue;
            std::getline(ls, info);
            now[who] = info.empty() ? info : info.substr(1);
        }
        for (auto& [who, info] : now)
            if (!m_here.count(who) && who != m_name && g) g->Note(who + " is here");
        for (auto& [who, info] : m_here)
            if (!now.count(who) && g)
            {
                g->Note(who + " left");
                if (m_host) g->Act(m_name, "leave", who);
            }
        m_here = now;
        // Someone other than the host is present: a guest has joined since the
        // last reset. Kept because "the players have gone" is a transition FROM
        // this, and the size test alone cannot tell an emptied table from one a
        // player has not yet beaten a presence line into (a move can be judged
        // before the mover's first Tick arrives, and a table reset in that
        // window loses the game under a player who is still there).
        for (auto& [who, info] : now)
            if (who != m_name) { m_had_guest = true; break; }
        // A table whose players have gone is reset for the next two: the
        // ranking server's tables are reused this way, and a person's hosted
        // game the same. Only after a guest was actually here -- so the empty
        // table this host just made is left at the start, not reset in a loop.
        if (m_host && g && m_had_guest && now.size() <= 1
            && (!g->IsActive() || (m_kind == "ranked" && !g->LastMove().empty())))
        {
            g->Act(m_name, "reset", "");
            m_had_guest = false;
            ETCS_LOG("ChessShare", m_name << ": the table is reset for the next two");
        }
        // "<seat> <hash> <seq>": a board at the owner's seq that hashes
        // differently is not the record's board.
        if (g && !m_host && now.count(m_owner) && now.count(m_name))
        {
            std::istringstream a(now[m_owner]), b(now[m_name]);
            std::string sa, ha, qa, sb, hb, qb;
            a >> sa >> ha >> qa; b >> sb >> hb >> qb;
            if (qa == qb && !ha.empty() && ha != hb && m_differed_at != qa)
            {
                m_differed_at = qa;
                ETCS_LOG("ChessShare", "this board (" << hb << ") is not the host's (" << ha << ") at " << qa);
                g->Note("the boards differ at " + qa + " -- this one is not the record's");
            }
        }
        if (m_host) Tick();
    }
    // Who is here, for the page: "<name> <seat> <hash> <seq>" per line, the
    // host first and marked -- each member's board as they advertised it.
    std::string Who() const
    {
        std::string out;
        if (m_here.count(m_owner)) out += m_owner + " " + m_here.at(m_owner) + " host\n";
        for (auto& [who, info] : m_here)
            if (who != m_owner) out += who + " " + info + "\n";
        return out;
    }

private:
    static void callOn(ETCS::RID rid, const char* verb, ETCS::Buffer& arg)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        if (e) e->call(ETCS::Buffer(verb), arg);
    }
    ChessGame* game() const
    {
        ETCS::Entity* e = m_game ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_game) : nullptr;
        return e ? static_cast<ChessGame*>(e->getTrueType()) : nullptr;
    }

    ETCS::RID   m_game = 0, m_presence = 0, m_hall = 0;
    std::string m_name, m_owner, m_sent, m_differed_at, m_kind, m_id, m_advertised;
    bool        m_host = false, m_had_guest = false;
    std::map<std::string, std::string> m_here;   // name -> "<seat> <hash> <seq>"
};

#endif // CHESSSHARE_H__
