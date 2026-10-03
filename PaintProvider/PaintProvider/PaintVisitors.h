#ifndef PAINTPROVIDER_PAINTVISITORS_H__
#define PAINTPROVIDER_PAINTVISITORS_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintCanvasMenu.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintShare: this page's part in a shared session ─────────────────────────
 *
 * A SESSION IS A RECORD AND A ROOM, and both are the host page's own. The
 * host's runtime holds the record (a NetworkProvider::Ledger) and hosts a Room
 * through the site's hub (Room::HostVia: the hub splices bytes and knows
 * nothing about paint); every guest's runtime links to it and binds surfaces
 * of what the host published. The session ends with the host, which is what a
 * shared canvas is: somebody's canvas, shared. Nothing about the session lives
 * on a server -- the HTTP relay this replaced (PaintNode) was the arrangement
 * available before a browser could hold a link, and its sessions were the
 * server's to keep and to lose.
 *
 * WHAT IS PUBLISHED, and to whom (paint_host.etcs):
 *
 *   record     the Ledger, to be READ: Head, Since, Follow. Every member
 *              follows it (record.Follow -> doc.Absorb) and every member's
 *              picture is what the record says.
 *   intake     the way in: a Ledger behind a Seal, fed into the record
 *              (Ledger::Feed). Writing is holding the key -- a writer's
 *              doc.Emit -> intake.Take passes the seal, a reader's cannot.
 *   presence   a Lobby: each member advertises where it is looking and what
 *              its picture is; watched by everyone (Lobby::Watch -> Roster).
 *              An entry goes when its link does, so the roster is the list of
 *              who is here, not of who once was.
 *
 * THE KEY GOES BY POST. A guest publishes a mailbox on its own link (a Ledger
 * on its Peer), and the host, promoting them, binds it through the Room by the
 * guest's name and appends the key there -- the one channel that is that
 * guest's alone. Demoting anyone turns the key (Seal::Key) and posts the new
 * one to those who still write; the demoted member's next line fails the seal,
 * their Emit ends, and what they had drawn since comes off (RevertPending).
 * The role table is the host's, advertised in the presence listing so every
 * window shows it, and kept by the same name the link was made under -- which
 * is the name a record line is authored as, so the two cannot disagree.
 *
 * WHO IS TYPING WHERE is in the record too (PaintOpKind::Hold): a claim is a
 * line, the first in the record holds, and every member derives the same
 * answer. The host releases what a departed member held.
 *
 * WHAT THIS TYPE DOES: keeps the role, the roster and the presence beat, and
 * says when a picture is not the owner's. The streams are the script's
 * (paint_host.etcs, paint_join.etcs), the surfaces are NetworkProvider's, and
 * the page does only what needs the page -- writes and runs those scripts, and
 * relays the visitor window's presses to verbs here.
 *
 * NOTHING HERE GROWS WITH USE. The record keeps itself from the host's last
 * Page line (Record::Checkpoint, PaintDocument::checkpoint), a link that
 * closes takes its presence with it, and a session ends with its host.
 */
/*
 * ── PaintVisitors: who is in the session, drawn on the sheet ─────────────────
 *
 * THE MENU IS IN THE CANVAS BECAUSE IT CAN BE. The page used to hold this, and
 * my reason was that a share panel needs TEXT ENTRY -- a node address, a name, a
 * session to type -- which a PaintCanvasMenu pane has no field for. The link
 * removed all three: the id is minted, the name is remembered, and the node is
 * whoever served the page. What is left is names, roles and buttons, which is
 * exactly what these panes are made of.
 *
 * ROWS ARE DECLARED BY THE SCRIPT, NOT SPAWNED HERE -- the same shape
 * PaintLayerPanel uses, and for the same reason: the LOOK belongs to the script
 * that draws it and this type only says what each row currently MEANS. A fixed
 * pool with the unused rows hidden also means no allocation on a roster change,
 * which happens on a timer.
 *
 * AND THE BUTTONS ARE PALETTE CALLS. Each row's controls are ordinary
 * PaintPalette::AddCall entries naming a verb here with the node pressed as the
 * argument, which is how every control in paint_menu.etcs already works. What
 * the input path does know about this window is the two things a palette call
 * cannot carry: where the pointer is, for dragging it by its title, and the
 * keys, while a name is being typed (PaintInput::BindVisitors).
 *
 * WHAT IT DOES NOT DO: talk to the node. It cannot -- a fetch belongs to the
 * page. So a press raises an event the page listens for, the page performs the
 * verb against the node, and the answer comes back as a fresh roster. One
 * direction each way, and this type never learns what a session is.
 */
class PaintVisitors : public DeletableBase<PaintVisitors>
{
public:
    WIRE_TYPE_IDENTITY(PaintVisitors);

    PaintVisitors() = default;
    bool DeleteConcrete() override { return true; }

    bool Create()
    {
        m_rows.clear();
        this->addTag("active");
        return true;
    }

    /*
 * ── A ROW IS ASSEMBLED, AND ITS BUTTONS NAME THEMSELVES ──────────────────
 *
 * Same shape as PaintLayerPanel's, and the second half is the part that makes
 * one row script serve eight rows: the press verbs take the NODE that was
 * pressed rather than a row index, so a palette call is written once with no
 * number in it. An index would have had to be a literal in the script, which
 * means a file per row, which is the duplication this replaced.
 *
 * The buttons (and the words on them) are the HOST's: every one of them is
 * hidden on a guest's window, and on the host's own row, where they could only
 * ever be refused.
 */
    void BeginRow() { m_title_open = false; m_rows.push_back(Row{}); }

    /*
 * THE BAR'S EYE, as the layer window has: run paint_eye.etcs between
 * BeginTitle and the first BeginRow and its nodes are the window's view
 * toggle -- open while the window shows everything, shut while it is folded
 * to its bar (PressView). A session is long and the window is big; the bar
 * left on screen is the way back, as the layer window's is.
 */
    void BeginTitle() { m_title_open = true; }

    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (m_title_open && node != 0)
        {
            if      (what == "eye")   m_view_eye  = node;
            else if (what == "iris")  m_view_iris = node;
            else if (what == "pupil") m_view_pupil = node;
            else ETCS_LOG("PaintVisitors", "RowNode: '" << what << "' on the title bar -- only an eye goes there.");
            tint_view();
            return;
        }
        if (m_rows.empty()) { ETCS_LOG("PaintVisitors", "RowNode before BeginRow -- ignored."); return; }
        if (node == 0) return;
        Row& row = m_rows.back();
        if      (what == "bg")   row.bg   = node;
        else if (what == "name") row.name = node;
        else if (what == "role") row.role = node;
        else if (what == "chip") row.chip = node;
        else if (what == "up" || what == "down" || what == "out")
        {
            m_buttons[node] = m_rows.size() - 1;
            row.controls.push_back(node);
        }
        // A button's word: the same press (the script calls the same verb from
        // it), and hidden with its button.
        else if (what == "word")
        {
            m_buttons[node] = m_rows.size() - 1;
            row.controls.push_back(node);
        }
        else
            ETCS_LOG("PaintVisitors", "RowNode: '" << what << "' is not a part of a row "
                     "(bg name role chip up down out word) -- RID:" << node << " is attached to nothing.");
    }

    size_t rowOf(ETCS::RID node) const
    {
        auto it = m_buttons.find(node);
        return (it == m_buttons.end()) ? SIZE_MAX : it->second;
    }

    void BindWindow(ETCS::RID pane) { m_window = pane; }
    ETCS::RID window() const { return m_window; }   // see PaintAnimation::window

    /*
 * ── THE WINDOW'S OWN PARTS ───────────────────────────────────────────────
 *
 * The title is the handle (the window follows a press on it, as the layer
 * window does -- PaintInput moves it, see PressTitle). A HOST-ONLY node is
 * shown to the owner of the session and nobody else (the link, the end
 * button); a GUEST-ONLY node the reverse (leave). The "you" line is everyone's:
 * the name this page goes by in the room, pressed to rename it, beside the
 * colour its frame is drawn in, with the swatches to change it.
 */
    void BindTitle(ETCS::RID node)     { if (node) m_title.push_back(node); }
    void BindHostOnly(ETCS::RID node)  { if (node) m_host_only.push_back(node); }
    void BindGuestOnly(ETCS::RID node) { if (node) m_guest_only.push_back(node); }
    void BindMe(ETCS::RID name, ETCS::RID chip) { m_me_label = name; m_me_chip = chip; }
    void BindSwatch(ETCS::RID node, float r, float g, float b)
    {
        if (!node) return;
        m_swatches[node] = Rgb{ r, g, b };
        paint_node_fill(node, r, g, b, 1.0f);
    }

    // Row colours and the two role inks, given by the script for the same
    // reason the panel's are: this is a look, and the script drew it.
    void SetRowColors(float r, float g, float b, float a)
    { m_row[0] = r; m_row[1] = g; m_row[2] = b; m_row[3] = a; }
    void SetInk(float wr, float wg, float wb, float rr, float rg, float rb)
    {
        m_ink_writer[0] = wr; m_ink_writer[1] = wg; m_ink_writer[2] = wb;
        m_ink_reader[0] = rr; m_ink_reader[1] = rg; m_ink_reader[2] = rb;
    }

    /*
 * "name role [colour]" per line, exactly as the node answers `who`. Parsed
 * here rather than by the page because the page would then be deciding what a
 * row says, and the row is this window's.
 *
 * The OWNER's line is kept and marked rather than dropped: a host looking at a
 * list of other people has no way to tell whether the list is short because
 * nobody came or because it does not include them.
 */
    void SetRoster(const std::string& text)
    {
        m_who.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(line);
            Person p;
            if (!(ls >> p.name >> p.role)) continue;
            ls >> p.hue;
            m_who.push_back(std::move(p));
        }
        // Owner first, then by name, so a row does not move under the pointer
        // every time somebody's idle clock ticks.
        std::stable_sort(m_who.begin(), m_who.end(),
            [](const Person& a, const Person& b)
            {
                if ((a.role == "owner") != (b.role == "owner")) return a.role == "owner";
                return a.name < b.name;
            });
        Refresh();
    }

    /*
 * OPENED AS WHAT THIS PAGE IS in the session -- owner, writer or reader --
 * because that decides which half of the window it gets. Reopened as the
 * role changes; the window stays where it was put.
 */
    void OpenAs(const std::string& role)
    {
        m_role = role.empty() ? std::string("reader") : role;
        show(true);
        const bool host = (m_role == "owner");
        for (ETCS::RID n : m_host_only)  paint_node_hidden(n, !host);
        for (ETCS::RID n : m_guest_only) paint_node_hidden(n, host);
        Refresh();
    }
    void Open()  { OpenAs("owner"); }

    // The window's end button: for the host it ends the session, for a guest
    // it leaves it. Either way the page is who tells the node.
    void Close() { const bool host = (m_role == "owner"); show(false); page_event(host ? "close" : "leave"); }
    // The page's own close, when the session is already over: no event back.
    void Hide()  { end_edit(false); show(false); }
    bool shown() const { return m_shown; }
    size_t count() const { return m_who.size(); }

    void CopyLink() { page_event("copy"); }

    // What this page is called in the room and the colour it is seen in, from
    // the page -- the page holds both (they persist beside each other).
    void SetMe(const std::string& name, const std::string& hex)
    {
        m_me = name;
        m_me_hex = hex;
        if (!m_editing) paint_node_text(m_me_label, name);
        Rgb c;
        if (paint_hex_rgb(hex, c)) paint_node_fill(m_me_chip, c.r, c.g, c.b, 1.0f);
        Refresh();
    }

    // A swatch pressed: the colour goes to the page, which keeps it and tells
    // the room (the next presence line carries it).
    void PickColor(ETCS::RID node)
    {
        auto it = m_swatches.find(node);
        if (it == m_swatches.end()) return;
        const std::string hex = paint_rgb_hex(it->second);
        paint_node_fill(m_me_chip, it->second.r, it->second.g, it->second.b, 1.0f);
        m_me_hex = hex;
        page_event(("hue:" + hex).c_str());
    }

    /*
 * ── RENAMING YOURSELF ────────────────────────────────────────────────────
 *
 * The layer and page lists' field again: a press on your name opens it with
 * the name in it, every key goes to it until Enter (keep) or Escape (drop),
 * and a press anywhere else keeps what was typed. The node decides whether
 * the name is free -- the page asks it, and says SetMe with whichever name
 * this page ends up with.
 */
    void EditName()
    {
        if (m_editing) return;
        m_editing = true;
        m_edit = m_me;
        paint_node_text(m_me_label, m_edit + "_");
    }

    bool editing() const { return m_editing; }
    void CloseEdit() { end_edit(true); }

    bool KeyIn(uint16_t key)
    {
        if (!m_editing) return false;
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        if (key == KEY_ENTER)  { end_edit(true);  return true; }
        if (key == KEY_ESCAPE) { end_edit(false); return true; }
        if (key == KEY_BACKSPACE) { if (!m_edit.empty()) m_edit.pop_back(); }
        else
        {
            const char ch = paint_key_to_char(key);
            // The node's own rule for a name (no space, no slash, 24 at most),
            // and no comma, which would split it in two on the way back in (SetMe).
            if (ch != 0 && ch != ' ' && ch != '/' && ch != ',' && m_edit.size() < 24) m_edit.push_back(ch);
        }
        paint_node_text(m_me_label, m_edit + "_");
        return true;
    }

    /*
 * ── MOVING THE WINDOW ────────────────────────────────────────────────────
 *
 * PaintLayerPanel's arrangement: a press on the title records the window's
 * origin and the press, and every motion until the release puts the window
 * at the origin plus how far the pointer has gone. Points are in the window's
 * PARENT's space. This window is its own routing root, so the pointer would
 * leave it at the first fast flick -- the router holds the pointer on it for
 * the length of the drag (PaintRouter::Route, `capture`).
 */
    bool PressTitle(ETCS::RID node, Point2D at)
    {
        bool mine = false;
        for (ETCS::RID t : m_title) if (t == node) { mine = true; break; }
        if (!mine) return false;
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return true;
        const Rect2D b = w->Bounds();
        m_moving = true;
        m_grab = at;
        m_origin = Point2D{ b.x, b.y };
        return true;
    }
    bool moving() const { return m_moving; }

    // A press on the bar's eye folds the window to its bar, or opens it again.
    bool PressView(ETCS::RID node)
    {
        if (node == 0 || (node != m_view_eye && node != m_view_iris && node != m_view_pupil)) return false;
        end_edit(true);
        m_folded = !m_folded;
        tint_view();
        paint_window_fold(m_window, m_folded, paint_nodes_bottom(m_title), m_full_h);
        return true;
    }
    bool folded() const { return m_folded; }

    void DragWindow(Point2D at)
    {
        if (!m_moving) return;
        if (!paint_node_moved(m_window, m_origin.x + (at.x - m_grab.x), m_origin.y + (at.y - m_grab.y)))
            m_moving = false;
    }
    void EndWindowDrag() { m_moving = false; }

    /*
 * THE THREE ROW VERBS, reached as palette calls with the node pressed. Each one
 * only NAMES what the page should ask the node for -- this window changes
 * nothing by itself, because the roster it is drawing is the node's and the
 * next refresh would overwrite any guess it made.
 */
    void Promote(ETCS::RID node) { act(rowOf(node), "writer"); }
    void Demote(ETCS::RID node)  { act(rowOf(node), "reader"); }
    void Remove(ETCS::RID node)  { act(rowOf(node), "out");    }

private:
    struct Rgb    { float r = 0, g = 0, b = 0; };
    struct Row    { ETCS::RID bg = 0, name = 0, role = 0, chip = 0; std::vector<ETCS::RID> controls; };
    struct Person { std::string name, role, hue; };

    static bool paint_hex_rgb(const std::string& hex, Rgb& out)
    {
        std::string h = hex;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() < 6) return false;
        for (size_t i = 0; i < 6; ++i) if (!std::isxdigit(static_cast<unsigned char>(h[i]))) return false;
        out.r = std::stoi(h.substr(0, 2), nullptr, 16) / 255.0f;
        out.g = std::stoi(h.substr(2, 2), nullptr, 16) / 255.0f;
        out.b = std::stoi(h.substr(4, 2), nullptr, 16) / 255.0f;
        return true;
    }
    static std::string paint_rgb_hex(const Rgb& c)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x",
                      paint_to_byte(c.r), paint_to_byte(c.g), paint_to_byte(c.b));
        return buf;
    }

    void act(size_t row, const char* verb)
    {
        if (m_role != "owner") return;               // the buttons are the host's
        if (row == SIZE_MAX || row >= m_who.size()) return;
        if (m_who[row].role == "owner") return;      // the host is not theirs to change
        page_event((std::string(verb) + ":" + m_who[row].name).c_str());
    }

    // The layer window's eye colours (paint_layers.etcs), so one eye means one
    // thing on the sheet: the white lit and the iris the page's highlight
    // while open; both sunk into the band while shut, the pupil with them.
    void tint_view()
    {
        static constexpr float white_open[3] = { 0.94f, 0.89f, 0.78f }, white_shut[3] = { 0.35f, 0.36f, 0.28f };
        static constexpr float iris_open[3]  = { 0.35f, 0.55f, 0.95f }, iris_shut[3]  = { 0.106f, 0.110f, 0.078f };
        const float* w = m_folded ? white_shut : white_open;
        const float* i = m_folded ? iris_shut  : iris_open;
        if (m_view_eye)  paint_node_fill(m_view_eye,  w[0], w[1], w[2], 1.0f);
        if (m_view_iris) paint_node_fill(m_view_iris, i[0], i[1], i[2], 1.0f);
        if (m_view_pupil) paint_node_hidden(m_view_pupil, m_folded);
    }

    void end_edit(bool keep)
    {
        if (!m_editing) return;
        m_editing = false;
        std::string want = m_edit;
        m_edit.clear();
        paint_node_text(m_me_label, m_me);          // until the page says otherwise
        if (keep && !want.empty() && want != m_me) page_event(("name:" + want).c_str());
    }

    void show(bool up)
    {
        m_shown = up;
        paint_node_hidden(m_window, !up);
    }

    static void set_ink(ETCS::RID rid, const float* c)
    {
        ETCS::Entity* raw = paint_resolve_tag("TextLabel", rid);
        if (!raw) return;
        ETCS::Buffer b;
        b.writeString((std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", "
                       + std::to_string(c[2]) + ", 1.0").c_str());
        raw->call(ETCS::Buffer("TextLabel.SetColor"), b, ETCS::RootSignalContext());
    }

    // Rows beyond the roster are HIDDEN rather than blanked: an empty plate
    // still reads as a person who has not loaded yet.
    void Refresh()
    {
        const bool host = (m_role == "owner");
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            const Row& row = m_rows[i];
            const bool live = (i < m_who.size());
            paint_node_hidden(row.bg,   !live);
            paint_node_hidden(row.name, !live);
            paint_node_hidden(row.role, !live);
            paint_node_hidden(row.chip, !live);
            const bool controls = live && host && m_who[i].role != "owner";
            for (ETCS::RID n : row.controls) paint_node_hidden(n, !controls);
            if (!live) continue;
            const Person& p = m_who[i];
            // Your own row is lit rather than labelled: a "(you)" after a long
            // name ran into the role beside it.
            const float* plate = (p.name == m_me) ? m_row_me : m_row;
            paint_node_fill(row.bg, plate[0], plate[1], plate[2], 1.0f);
            // Sixteen characters is what fits before the role column.
            paint_node_text(row.name, p.name.size() > 16 ? p.name.substr(0, 15) + "~" : p.name);
            paint_node_text(row.role, p.role);
            set_ink(row.role, (p.role == "writer" || p.role == "owner") ? m_ink_writer : m_ink_reader);
            Rgb c;
            if (paint_hex_rgb(p.hue, c)) paint_node_fill(row.chip, c.r, c.g, c.b, 1.0f);
            else                         paint_node_fill(row.chip, 0.30f, 0.31f, 0.26f, 1.0f);
        }
    }

    /*
 * UP TO THE PAGE, because only the page can reach the node. Same proxy and the
 * same reason as PaintCanvasMenu::page_event: this runs on a Worker with no
 * window, so MAIN_THREAD_EM_ASM rather than EM_ASM.
 */
    void page_event(const char* what)
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            var what = UTF8ToString($0);
            window.dispatchEvent(new CustomEvent('etcs-visitors', { detail: what }));
        }, what);
#endif
        ETCS_LOG("PaintVisitors", what << " -> the page.");
    }

    std::vector<Row>    m_rows;
    std::unordered_map<ETCS::RID, size_t> m_buttons;   // control -> its row
    std::vector<Person> m_who;
    ETCS::RID m_window = 0;
    bool  m_shown = false;
    std::string m_role = "owner";

    std::vector<ETCS::RID> m_title, m_host_only, m_guest_only;
    ETCS::RID m_me_label = 0, m_me_chip = 0;
    std::unordered_map<ETCS::RID, Rgb> m_swatches;
    std::string m_me, m_me_hex;
    bool        m_editing = false;
    std::string m_edit;

    bool    m_moving = false;
    Point2D m_grab{ 0, 0 }, m_origin{ 0, 0 };

    bool      m_title_open = false;    // between BeginTitle and the first BeginRow
    ETCS::RID m_view_eye = 0, m_view_iris = 0, m_view_pupil = 0;
    bool      m_folded = false;
    uint32_t  m_full_h = 0;            // the window's height while folded -- paint_window_fold

    float m_row[4]        = { 0.14f, 0.15f, 0.11f, 0.96f };
    float m_row_me[3]     = { 0.22f, 0.24f, 0.16f };
    float m_ink_writer[3] = { 0.79f, 0.71f, 0.35f };
    float m_ink_reader[3] = { 0.55f, 0.57f, 0.50f };
};

#endif // PAINTPROVIDER_PAINTVISITORS_H__
