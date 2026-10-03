#ifndef PAINTPROVIDER_PAINTSHARE_H__
#define PAINTPROVIDER_PAINTSHARE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintRouter.h"   // in order: everything above this in the module is visible here

class PaintShare : public DeletableBase<PaintShare>
{
public:
    WIRE_TYPE_IDENTITY(PaintShare);

    PaintShare() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    // This page's document, canvas pane and visitor window.
    void Attach(ETCS::RID doc, ETCS::RID canvas, ETCS::RID visitors)
    {
        m_doc = doc; m_canvas = canvas; m_visitors = visitors;
        if (PaintDocument* d = document()) d->SetShare(getRID());
    }

    // Hosting: the record, the sealed way in and its seal, the presence lobby
    // and the room -- all this runtime's own -- and this page's name.
    bool Host(ETCS::RID record, ETCS::RID intake, ETCS::RID seal, ETCS::RID presence, ETCS::RID room,
              const std::string& name)
    {
        return begin(record, intake, seal, presence, room, name, "owner");
    }
    // Joining: the same surfaces, bound through the Peer, and this page's name
    // -- a reader until the key arrives (Mail).
    bool Join(ETCS::RID record, ETCS::RID intake, ETCS::RID seal, ETCS::RID presence, ETCS::RID peer,
              const std::string& name)
    {
        return begin(record, intake, seal, presence, peer, name, "reader");
    }

    // The key the intake takes (the host's, for posting); a fresh one when
    // it is turned.
    std::string Key() const { return m_key; }
    std::string role() const { return m_role; }
    const std::string& name() const { return m_name; }
    bool inSession() const { return !m_name.empty(); }

    /*
     * THE HOST SETS A ROLE. A writer is told so here and given the key by the
     * page (Key, then a grant script -- the mailbox is another module's type,
     * which this one cannot spawn). A reader, or one put out, is what turns
     * the key: the seal takes a new one, the remaining writers are re-posted
     * it (the page, told "rekey"), and the old key opens nothing.
     */
    bool Role(const std::string& who, const std::string& role)
    {
        if (m_role != "owner" || who.empty() || who == m_name) return false;
        if (role != "writer" && role != "reader" && role != "out") return false;
        const std::string was = m_roles.count(who) ? m_roles[who] : "reader";
        if (role == "out") m_roles.erase(who); else m_roles[who] = role;
        if (was == "writer" && role != "writer") turnKey();
        ETCS_LOG("PaintShare", who << " is now " << (role == "out" ? "out of the session" : "a " + role));
        advertiseRoles();
        m_sent_view.clear();
        return true;
    }
    // Every writer's name, for re-posting a turned key.
    std::string Writers() const
    {
        std::string out;
        for (auto& [n, r] : m_roles) if (r == "writer") out += (out.empty() ? "" : " ") + n;
        return out;
    }
    void Hue(const std::string& hex) { m_hue = hex; m_sent_view.clear(); tellWindow(); }

    /*
     * ONCE A SECOND, FROM THE PAGE: this page's presence, when it changed --
     * where it is looking, its colour, the record head it has taken in and the
     * hash of what it made of it (PaintDocument::PictureHash), and whether it
     * needs the page whole -- and the housekeeping a clock does: a box typed
     * into and then left alone is let go, and a wait for the page whole ends.
     */
    void Tick()
    {
        PaintDocument* doc = document();
        if (!doc || !inSession()) return;
        if (doc->textIdleMs() > kTextIdleMs)
        {
            ETCS_LOG("PaintShare", "text box let go after a while with no typing -- sent as it stands");
            doc->SelectTextBox(0);
        }
        if (m_syncing && now_ms() - m_sync_since > kSyncMaxMs) caughtUp();
        int32_t x = 0, y = 0, w = 0, h = 0;
        if (PaintSurface* c = canvas()) c->ViewRect(x, y, w, h);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(doc->PictureHash()));
        m_picture    = doc->settled() ? hex : "-";
        m_picture_at = doc->recordChainSeq();
        const std::string line = std::to_string(x) + "/" + std::to_string(y) + "/" + std::to_string(w) + "/"
                               + std::to_string(h) + "/" + (m_hue.empty() ? "888888" : m_hue) + "/"
                               + std::to_string(m_picture_at) + "/" + m_picture + "/" + (m_want_page ? "R" : "-");
        if (line == m_sent_view) return;
        m_sent_view = line;
        advertise(m_name, "view", line);
        if (m_role == "owner") advertiseRoles();
    }

    /*
     * THE ROSTER ARRIVES (presence.Watch -> Roster), the whole listing on
     * every change: who is here and what their role is, into the window; where
     * each is looking, onto the canvas; and the owner's picture against this
     * one. A member at the owner's record head whose picture differs from
     * the owner's, for three beats running, has diverged -- whatever it took
     * in, what it made of it is not what the owner made -- and asks for the
     * page whole; the owner answers (PaintDocument::Restate) at most every
     * twenty seconds, since every member takes what it sends.
     */
    void roster(const std::string& listing)
    {
        PaintDocument* doc = document();
        std::map<std::string, std::string> views, roles;
        std::istringstream in(listing);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream ls(line);
            std::string who, kind, info;
            if (!(ls >> who >> kind)) continue;
            std::getline(ls, info);
            if (!info.empty() && info[0] == ' ') info.erase(0, 1);
            if (kind == "view") views[who] = info;
            else if (kind == "roles")
            {
                std::istringstream rs(info);
                std::string pair;
                while (rs >> pair)
                {
                    const size_t eq = pair.find('=');
                    if (eq != std::string::npos) roles[pair.substr(0, eq)] = pair.substr(eq + 1);
                }
            }
        }
        // Departures: the owner releases what they held.
        if (m_role == "owner" && doc)
            for (auto& [who, v] : m_views)
                if (!views.count(who)) { doc->ReleaseHeld(who); m_roles.erase(who); }
        m_views = views;
        // The window: name, role, colour; the owner first.
        std::string who_text;
        bool want_page = false;
        for (auto& [who, info] : views)
        {
            std::vector<std::string> f = split(info, '/');
            const std::string role = who == m_owner ? "owner" : roles.count(who) ? roles[who] : "reader";
            who_text += who + " " + role + " " + (f.size() > 4 ? f[4] : "888888") + "\n";
            if (f.size() > 7 && f[7] == "R" && who != m_name) want_page = true;
        }
        // Everyone else's frame, in document space, through this pane's own
        // projection (PaintSurface::SetPeer).
        if (PaintSurface* c = canvas()) c->ClearPeers();
        for (auto& [who, info] : views)
        {
            if (who == m_name) continue;
            std::vector<std::string> f = split(info, '/');
            Rgb rgb;
            if (f.size() > 4 && canvas() && paint_hex_rgb(f[4], rgb))
                canvas()->SetPeer(who, std::atoi(f[0].c_str()), std::atoi(f[1].c_str()),
                                  std::atoi(f[2].c_str()), std::atoi(f[3].c_str()), rgb.r, rgb.g, rgb.b);
        }
        if (PaintVisitors* v = visitors()) v->SetRoster(who_text);
        // A guest learns its role from the table; the key follows by post.
        if (m_role != "owner")
        {
            const std::string mine = roles.count(m_name) ? roles[m_name] : "reader";
            if (mine != m_role) { m_role = mine; tellWindow(); ETCS_LOG("PaintShare", "you are now a " << mine << " in this session"); }
        }
        // The owner's picture is the room's.
        if (m_role != "owner" && doc && views.count(m_owner))
        {
            std::vector<std::string> f = split(views[m_owner], '/');
            if (f.size() > 6)
            {
                const uint64_t owner_at = std::strtoull(f[5].c_str(), nullptr, 10);
                const std::string& owner_picture = f[6];
                const uint64_t at = m_picture_at;
                const bool still = owner_at == at && at > 0 && at == m_last_head
                                && m_picture != "-" && owner_picture != "-" && m_picture != owner_picture;
                m_mismatch = still ? m_mismatch + 1 : 0;
                m_last_head = at;
                if (owner_at == at && at > 0 && m_picture != "-" && m_picture == owner_picture) inStep();
                if (m_mismatch >= 3)
                {
                    m_mismatch = 0;
                    outOfStep("this picture (" + m_picture + ") is not the owner's (" + owner_picture + ") at "
                              + std::to_string(at) + " -- asking for the page whole");
                }
            }
        }
        if (m_role == "owner" && want_page && doc && now_ms() - m_restated_at > kRestateEveryMs)
        {
            m_restated_at = now_ms();
            doc->Restate();
            ETCS_LOG("PaintShare", "a member cannot rebuild this page from the record -- sending it whole");
        }
        if (PaintSurface* c = canvas()) c->Render();
    }

    /*
     * THE POST ARRIVES (mail.Follow -> Mail): "key <key>" turns this page's
     * seal, and the page is told to open the way in (doc.Emit -> intake.Take)
     * -- a stream is the script's to open, not this type's.
     */
    void mail(const std::string& line)
    {
        std::istringstream in(line);
        uint64_t seq = 0; std::string from, what, key;
        if (!(in >> seq >> from >> what >> key) || what != "key") return;
        if (ETCS::Entity* seal = resolve(m_seal))
        {
            ETCS::Buffer arg;
            arg.writeString(key.c_str());
            seal->call(ETCS::Buffer("Seal.Key"), arg);
        }
        m_key = key;
        ETCS_LOG("PaintShare", "the host has given this page drawing.");
        page_event("writer");
    }

    // The way in has closed on this page (its Emit ended): a turned key, or
    // the link. What was drawn and not taken comes off.
    void emitEnded()
    {
        if (!inSession()) return;
        if (PaintDocument* doc = document()) { doc->RevertPending(); if (PaintSurface* c = canvas()) c->Render(); }
        if (m_role != "owner") ETCS_LOG("PaintShare", "the room has you as a reader -- what you draw here comes back off");
    }
    // The record has ended for this page (its Absorb ended): the session is
    // over, or this page was put out. The canvas stays: it is a copy of the
    // one that was shared.
    void absorbEnded()
    {
        if (!inSession()) return;
        ETCS_LOG("PaintShare", "the shared session has ended -- this canvas is yours to keep");
        Leave();
        page_event("ended");
    }

    // Out of the session, whichever way: the window shut, the author cleared,
    // the peers off the canvas. What ends the link is the script's (the
    // room, the peer).
    void Leave()
    {
        if (!inSession()) return;
        if (PaintDocument* doc = document())
        {
            doc->SetAuthor("");
            doc->SetReadOnly(false);
            if (m_syncing) doc->Syncing(false);
        }
        if (PaintVisitors* v = visitors()) v->Hide();
        if (PaintSurface* c = canvas()) { c->ClearPeers(); c->Render(); }
        m_name.clear(); m_role.clear(); m_owner.clear(); m_key.clear();
        m_roles.clear(); m_views.clear(); m_sent_view.clear();
        m_syncing = false; m_want_page = false; m_mismatch = 0; m_last_head = 0;
        m_record = m_intake = m_seal = m_presence = m_link = 0;
    }

    static constexpr long kTextIdleMs     = 20000;
    static constexpr long kSyncMaxMs      = 15000;
    static constexpr long kRestateEveryMs = 20000;

private:
    bool begin(ETCS::RID record, ETCS::RID intake, ETCS::RID seal, ETCS::RID presence, ETCS::RID link,
               const std::string& name, const std::string& role)
    {
        PaintDocument* doc = document();
        if (!doc || name.empty()) { ETCS_LOG("PaintShare", "not attached, or no name."); return false; }
        Leave();
        m_record = record; m_intake = intake; m_seal = seal; m_presence = presence; m_link = link;
        m_name = name; m_role = role;
        if (role == "owner")
        {
            m_owner = name;
            m_key   = mint();
            if (ETCS::Entity* s = resolve(m_seal)) { ETCS::Buffer arg; arg.writeString(m_key.c_str()); s->call(ETCS::Buffer("Seal.Key"), arg); }
            // The picture as it stands is the session's first entries.
            doc->Announce();
        }
        doc->SetAuthor(name);
        doc->SetRecord(record, m_owner);
        doc->SetReadOnly(false);
        tellWindow();
        ETCS_LOG("PaintShare", (role == "owner" ? "sharing this canvas as " : "joined a shared canvas as ") << name);
        return true;
    }
    // The owner's name is what the roles entry is filed under; a guest
    // learns it from the listing (roster).
    void advertiseRoles()
    {
        std::string table;
        for (auto& [n, r] : m_roles) table += (table.empty() ? "" : " ") + n + "=" + r;
        advertise(m_name + ".roles", "roles", table.empty() ? "-" : table);
        m_owner = m_name;
    }
    void advertise(const std::string& name, const std::string& kind, const std::string& info)
    {
        ETCS::Entity* lobby = resolve(m_presence);
        if (!lobby) return;
        ETCS::Buffer arg;
        arg.writeString((name + " " + kind + " " + info).c_str());
        lobby->call(ETCS::Buffer("Lobby.Advertise"), arg);
    }
    void turnKey()
    {
        m_key = mint();
        if (ETCS::Entity* s = resolve(m_seal)) { ETCS::Buffer arg; arg.writeString(m_key.c_str()); s->call(ETCS::Buffer("Seal.Key"), arg); }
        ETCS_LOG("PaintShare", "the key is turned -- writers are posted the new one.");
        page_event("rekey");
    }
    void tellWindow()
    {
        PaintVisitors* v = visitors();
        if (!v || !inSession()) return;
        v->OpenAs(m_role);
        v->SetMe(m_name, m_hue.empty() ? "888888" : m_hue);
    }
    void outOfStep(const std::string& why)
    {
        m_want_page = true;
        m_sent_view.clear();
        ETCS_LOG("PaintShare", why);
        if (!m_syncing && document()) { m_syncing = true; m_sync_since = now_ms(); document()->Syncing(true); }
    }
    void inStep()
    {
        m_want_page = false;
        caughtUp();
    }
    void caughtUp()
    {
        if (!m_syncing) return;
        m_syncing = false;
        if (PaintDocument* doc = document()) doc->Syncing(false);
    }
    void page_event(const char* what)
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-share', { detail: UTF8ToString($0) }));
        }, what);
#endif
        ETCS_LOG("PaintShare", "share " << what);
    }
    static std::string mint()
    {
        std::random_device rd;
        char b[17];
        std::snprintf(b, sizeof b, "%08x%08x", rd(), rd());
        return b;
    }
    static long now_ms()
    {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static std::vector<std::string> split(const std::string& s, char c)
    {
        std::vector<std::string> out;
        size_t at = 0;
        while (true)
        {
            const size_t n = s.find(c, at);
            out.push_back(s.substr(at, n == std::string::npos ? std::string::npos : n - at));
            if (n == std::string::npos) return out;
            at = n + 1;
        }
    }
    struct Rgb { float r = 0, g = 0, b = 0; };
    static bool paint_hex_rgb(const std::string& hex, Rgb& out)
    {
        std::string h = hex;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() < 6) return false;
        out.r = std::strtol(h.substr(0, 2).c_str(), nullptr, 16) / 255.0f;
        out.g = std::strtol(h.substr(2, 2).c_str(), nullptr, 16) / 255.0f;
        out.b = std::strtol(h.substr(4, 2).c_str(), nullptr, 16) / 255.0f;
        return true;
    }
    static ETCS::Entity* resolve(ETCS::RID rid)
    {
        return rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
    }
    PaintDocument* document() const { ETCS::Entity* e = resolve(m_doc);      return e ? static_cast<PaintDocument*>(e->getTrueType()) : nullptr; }
    PaintSurface*  canvas()   const { ETCS::Entity* e = resolve(m_canvas);   return e ? static_cast<PaintSurface*>(e->getTrueType())  : nullptr; }
    PaintVisitors* visitors() const { ETCS::Entity* e = resolve(m_visitors); return e ? static_cast<PaintVisitors*>(e->getTrueType()) : nullptr; }

    ETCS::RID   m_doc = 0, m_canvas = 0, m_visitors = 0;
    ETCS::RID   m_record = 0, m_intake = 0, m_seal = 0, m_presence = 0, m_link = 0;
    std::string m_name, m_role, m_owner, m_key, m_hue;
    std::map<std::string, std::string> m_roles;   // the owner's table
    std::map<std::string, std::string> m_views;   // the last listing's views, for departures
    std::string m_sent_view, m_picture;
    uint64_t    m_picture_at = 0, m_last_head = 0;
    int         m_mismatch = 0;
    bool        m_syncing = false, m_want_page = false;
    long        m_sync_since = 0, m_restated_at = 0;
};

#endif // PAINTPROVIDER_PAINTSHARE_H__
