#ifndef CHESSTABLE_H__
#define CHESSTABLE_H__
#include "ChessBoard.h"
#include "ChessShare.h"
#include <sstream>
#include <vector>

/*
 * ── ChessTable: the seat in front of the board ────────────────────────────
 *
 * What a person does at a board -- click a piece, click where it goes, say
 * something, resign -- arrives here from the window's two streams (a pointer
 * and a keyboard) and leaves as ONE verb: ChessGame::Act, as whoever this
 * seat is. In a session that is the share's name; alone it is whichever
 * colour is to move, which makes the hotseat.
 *
 * A CLICK IS ROUTED BY THE BOARD'S OWN FRAME: the pointer arrives in window
 * pixels, the board says where it sits (ChessBoard::Origin), and the square
 * is asked of the board in the orientation it is drawn in. There is no
 * router and no pane list, because there is one thing to click on.
 *
 * THE BARS ARE THE OS VERSION'S CHAT AND COMMAND LINE. The browser page has
 * a text field and a chat column in the DOM; a window has neither, so the
 * root script that runs the boot in a window (scripts/chess_table.etcs)
 * adds two panes on the right and binds them here: rows of labels for the
 * conversation, one label for the line being typed. Tab slides them in and
 * out (Step, once a frame, ridden on the board's edge), and while they are
 * in, every key is the line's: Enter sends it, as a word to the table or,
 * with a leading slash, as a verb (`/resign`, `/draw`, `/sit black`,
 * `/flip`, `/fen ...`, `/reset`). With no bars bound, keys mean nothing here
 * and the page's field does the same job in HTML.
 *
 * THE LABELS ARE THE SCRIPT'S, positioned there and handed here by RID
 * (ChatRow, BindLine, BindStatus), the way every panel in PaintProvider is
 * assembled: this type writes their text and their place, and owns none.
 */
class ChessTable : public DeletableBase<ChessTable>
{
public:
    WIRE_TYPE_IDENTITY(ChessTable);

    ChessTable() = default;
    bool DeleteConcrete() override { if (ChessBoard* b = board()) b->BindStep(nullptr); return true; }

    static constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258, KEY_BACKSPACE = 259;
    static constexpr uint16_t KEY_LEFT_SHIFT = 340, KEY_RIGHT_SHIFT = 344;
    static constexpr int32_t  SLIDE_PX = 40;      // a frame's travel of the bars
    static constexpr int32_t  ROW_PX   = 16;      // a chat row's pitch: the 8px face, doubled
    static constexpr int32_t  ROW_X    = 8, ROW_Y = 10;

    bool Create() { this->addTag("active"); return true; }

    void BindGame(ETCS::RID game)   { m_game = game; }
    void BindShare(ETCS::RID share) { m_share = share; }
    void BindBoard(ETCS::RID rid)
    {
        if (ChessBoard* old = board()) old->BindStep(nullptr);
        m_board = rid;
        if (ChessBoard* b = board()) b->BindStep([this] { Step(); });
    }
    // Three labels: whose move and the state, the seats, and a note (the
    // last refusal, or what the share has to say).
    void BindStatus(ETCS::RID turn, ETCS::RID seats, ETCS::RID note) { m_turn = turn; m_seats = seats; m_note_label = note; m_shown_turn.clear(); m_shown_seats.clear(); m_shown_note.clear(); }
    // The bars: two panes that rest at `hidden_x` (off the edge) and slide
    // to `shown_x`. Hidden while out, so they are neither drawn nor picked.
    void BindBars(ETCS::RID chat_pane, ETCS::RID cmd_pane, int32_t shown_x, int32_t hidden_x)
    {
        m_chat_pane = chat_pane; m_cmd_pane = cmd_pane;
        m_shown_x = shown_x; m_hidden_x = hidden_x;
        m_bar_x = hidden_x; m_open = false;
        place_bars();
        node_hidden(m_chat_pane, true); node_hidden(m_cmd_pane, true);
    }
    void BindLine(ETCS::RID label) { m_line_label = label; m_shown_line = "\x01"; }
    // A row of the conversation, placed here in the order it is handed in:
    // the script that makes rows (chess_chat_row.etcs) is run once per row
    // and carries no number, so the row's place is this table's to give.
    void ChatRow(ETCS::RID label)
    {
        if (!label) return;
        node_verb(label, "SetPosition", std::to_string(ROW_X) + ", " + std::to_string(ROW_Y + ROW_PX * static_cast<int32_t>(m_rows.size())));
        m_rows.push_back(label);
        m_shown_rows.push_back("\x01");
    }

    // ── what a person does ────────────────────────────────────────────────
    // A press at a window point.
    void Click(int32_t wx, int32_t wy)
    {
        ChessBoard* b = board();
        ChessGame*  g = game();
        if (!b || !g) return;
        const Point2D o = b->Origin();
        const std::string sq = b->SquareAt(wx - o.x, wy - o.y);
        if (sq.empty()) return;
        const char p = g->PieceAt(sq);
        const bool mine = p != ' ' && p != 0 && ((p >= 'A' && p <= 'Z') == g->WhiteToMove());
        if (m_sel.empty())      { if (mine) select(sq); return; }
        if (sq == m_sel)        { select(""); return; }
        if (mine)               { select(sq); return; }
        std::string mv = m_sel + sq;
        const char from = g->PieceAt(m_sel);
        if ((from == 'P' && sq[1] == '8') || (from == 'p' && sq[1] == '1')) mv += "q";
        const std::string r = g->Act(me(), "move", mv);
        select("");
        note(refused(r) ? (mv + ": " + lower(r)) : "");
    }

    // A key from the window (GLFW codes), pressed or released.
    void Key(uint16_t key, bool down)
    {
        if (key == KEY_LEFT_SHIFT || key == KEY_RIGHT_SHIFT) { m_shift = down; return; }
        if (!down) return;
        if (key == KEY_TAB) { Toggle(); return; }
        if (!m_open) return;
        if (key == KEY_ESCAPE)    { Toggle(); return; }
        if (key == KEY_ENTER)     { Submit(); return; }
        if (key == KEY_BACKSPACE) { if (!m_line.empty()) m_line.pop_back(); return; }
        const char c = key_char(key, m_shift);
        if (c && m_line.size() < 200) m_line.push_back(c);
    }
    // Characters, for a test and for a keyboard that hands over text.
    void Type(const std::string& text) { for (char c : text) if (c >= 32 && c <= 126 && m_line.size() < 200) m_line.push_back(c); }
    // A whole line at once, then sent -- for a DOM input on the page, where the
    // line is typed in HTML and not into m_line: a leading slash is a verb, the
    // rest is said (Submit's rule). The seat is the table's, so the page needs
    // to know neither the name nor whether it is a verb.
    void Do(const std::string& text) { m_line = text; Submit(); }

    // The line, sent: a slash makes it a verb, anything else is said.
    void Submit()
    {
        std::string line = m_line;
        m_line.clear();
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) return;
        ChessGame* g = game();
        if (!g) return;
        if (line[0] != '/') { g->Act(me(), "say", encode(line)); return; }
        std::istringstream in(line.substr(1));
        std::string verb; in >> verb;
        std::string arg; std::getline(in, arg);
        if (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
        if (verb == "flip") { if (ChessBoard* b = board()) b->SetFlip(!b->Flipped()); return; }
        if (verb == "help") { note("/sit white|black  /resign  /draw  /decline  /reset  /flip  /fen <fen>  /move e2e4"); return; }
        if (verb == "fen")  { note(g->LoadFenStr(arg) == "INVALID" ? "that is not a position" : ""); return; }
        const std::string r = g->Act(me(), verb, arg);
        note(refused(r) ? ("/" + verb + ": " + lower(r)) : "");
    }

    void Toggle()
    {
        if (!m_chat_pane && !m_cmd_pane) return;
        m_open = !m_open;
        if (m_open) { node_hidden(m_chat_pane, false); node_hidden(m_cmd_pane, false); }
    }
    bool Open() const { return m_open; }
    const std::string& Line() const { return m_line; }
    const std::string& Selected() const { return m_sel; }

    // Once a frame, on the board's visit: the bars' travel, and the labels
    // brought up to the game -- every few frames, since a label's text is a
    // verb across a module and the game seldom changes between two frames.
    void Step()
    {
        const int32_t target = m_open ? m_shown_x : m_hidden_x;
        if (m_bar_x != target)
        {
            const int32_t d = target - m_bar_x;
            m_bar_x += (d > SLIDE_PX) ? SLIDE_PX : (d < -SLIDE_PX) ? -SLIDE_PX : d;
            place_bars();
            if (!m_open && m_bar_x == m_hidden_x) { node_hidden(m_chat_pane, true); node_hidden(m_cmd_pane, true); }
        }
        if (++m_tick % 6) return;
        refresh();
    }

    std::string Report() const
    {
        return std::string("seat ") + const_cast<ChessTable*>(this)->me() + ", selected '" + m_sel + "', bars "
             + (m_open ? "in" : "out") + ", line '" + m_line + "', " + std::to_string(m_rows.size()) + " chat row(s)"
             + (m_note.empty() ? "" : ", note '" + m_note + "'");
    }

private:
    std::string me()
    {
        if (ChessShare* s = share()) if (s->inSession()) return s->name();
        ChessGame* g = game();
        return (g && !g->WhiteToMove()) ? "black" : "white";
    }
    void select(const std::string& sq) { m_sel = sq; if (ChessBoard* b = board()) b->Select(sq); }
    void note(const std::string& text) { m_note = text; }
    static bool refused(const std::string& r)
    { return r == "ILLEGAL" || r == "GAME OVER" || r == "NOT YOUR TURN" || r == "NOT YOUR SEAT" || r == "TAKEN" || r == "NOT FOUND" || r == "NO OFFER"; }
    static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; }
    // What ChessGame::sayLocked decodes: the page encodes the same way.
    static std::string encode(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            if (c == '%' || c == ' ' || c < 32 || c > 126) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
        return out;
    }
    // GLFW's printable codes are the US key caps.
    static char key_char(uint16_t key, bool shift)
    {
        char c = 0;
        if (key >= 'A' && key <= 'Z') c = static_cast<char>(key - 'A' + 'a');
        else if (key >= 32 && key <= 126) c = static_cast<char>(key);
        if (!c || !shift) return c;
        if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
        static const char* from = "1234567890-=[]\\;',./`";
        static const char* to   = "!@#$%^&*()_+{}|:\"<>?~";
        for (size_t i = 0; from[i]; ++i) if (from[i] == c) return to[i];
        return c;
    }

    void refresh()
    {
        ChessGame* g = game();
        if (!g) return;
        if (m_turn || m_seats)
        {
            std::istringstream st(g->StatusLine(me()));
            std::string side, state, role, wseat, bseat, offer, wname, bname;
            st >> side >> state >> role >> wseat >> bseat >> offer >> wname >> bname;
            std::string turn = (side == "w" ? "white" : "black");
            if      (state == "ok")        turn += " to move";
            else if (state == "check")     turn += " to move, in check";
            else if (state == "checkmate") turn = std::string(side == "w" ? "black" : "white") + " wins by checkmate";
            else if (state == "stalemate") turn = "stalemate";
            else if (state == "draw")      turn = "drawn";
            else if (state.compare(0, 7, "resign-") == 0) turn = state.substr(7) + " resigned";
            if (offer == "theirs") turn += " -- a draw is offered (/draw to take it)";
            if (offer == "mine")   turn += " -- your draw offer stands";
            std::string seats = "white: " + wname + "   black: " + bname;
            if (role != "viewer") seats += "   you: " + role;
            set_text(m_turn, turn, m_shown_turn);
            set_text(m_seats, seats, m_shown_seats);
        }
        if (m_note_label)
        {
            std::string n = m_note;
            ChessShare* s = share();
            if (n.empty() && s && s->inSession())
                n = (s->name() == s->owner()) ? "hosting as " + s->name() : "at " + s->owner() + "'s table as " + s->name();
            set_text(m_note_label, n, m_shown_note);
        }
        if (m_line_label) set_text(m_line_label, "> " + m_line + "_", m_shown_line);
        if (!m_rows.empty())
        {
            std::vector<std::string> lines;
            std::istringstream in(g->ChatTail(m_rows.size()));
            std::string l;
            while (std::getline(in, l)) if (!l.empty()) lines.push_back(l);
            for (size_t i = 0; i < m_rows.size(); ++i)
                set_text(m_rows[i], i < lines.size() ? lines[i] : std::string(), m_shown_rows[i]);
        }
    }

    void place_bars()
    {
        node_x(m_chat_pane, m_bar_x);
        node_x(m_cmd_pane, m_bar_x);
    }

    // A verb across the module boundary, by the node's own tag; the chain
    // is marked afterwards so the pane that holds it recomposes.
    static bool node_verb(ETCS::RID node, const char* verb, const std::string& args)
    {
        if (!node) return false;
        std::string tag;
        {
            ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            if (!held) return false;
            tag = static_cast<ETCS::Entity*>(held.get())->getSourceTag().toString();
        }
        ETCS::Buffer action((tag + "." + verb).c_str());
        ETCS::Buffer payload(args.c_str());
        const ETCS::Buffer key(tag.c_str());
        ETCS::Entity* e = ETCS::etcs_resolve_by_key(key, node);
        if (!e) return false;
        try { e->call(action, payload); } catch (...) { return false; }
        if (ETCS::Entity* n = ETCS::etcs_resolve_by_key(key, node))
            for (; n; n = n->getParent()) etcs_mark_observed(n);
        return true;
    }
    static void set_text(ETCS::RID node, const std::string& text, std::string& shown)
    {
        if (!node || text == shown) return;
        shown = text;
        node_verb(node, "SetText", text);
    }
    static void node_hidden(ETCS::RID node, bool hidden) { node_verb(node, "SetHidden", hidden ? "1" : "0"); }
    static void node_x(ETCS::RID node, int32_t x)
    {
        if (!node) return;
        int32_t y = 0;
        {
            ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            if (!held) return;
            y = held->Bounds().y;
        }
        node_verb(node, "SetPosition", std::to_string(x) + ", " + std::to_string(y));
    }

    ChessGame* game() const
    {
        ETCS::Entity* e = m_game ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_game) : nullptr;
        return e ? static_cast<ChessGame*>(e->getTrueType()) : nullptr;
    }
    ChessShare* share() const
    {
        ETCS::Entity* e = m_share ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_share) : nullptr;
        return e ? static_cast<ChessShare*>(e->getTrueType()) : nullptr;
    }
    ChessBoard* board() const
    {
        ETCS::Entity* e = m_board ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_board) : nullptr;
        return e ? static_cast<ChessBoard*>(e->getTrueType()) : nullptr;
    }

    ETCS::RID m_game = 0, m_share = 0, m_board = 0;
    ETCS::RID m_turn = 0, m_seats = 0, m_note_label = 0, m_line_label = 0;
    ETCS::RID m_chat_pane = 0, m_cmd_pane = 0;
    std::vector<ETCS::RID> m_rows;
    std::vector<std::string> m_shown_rows;
    std::string m_shown_turn, m_shown_seats, m_shown_note, m_shown_line;
    std::string m_sel, m_line, m_note;
    int32_t  m_shown_x = 0, m_hidden_x = 0, m_bar_x = 0;
    bool     m_open = false, m_shift = false;
    uint32_t m_tick = 0;
};

#endif // CHESSTABLE_H__
