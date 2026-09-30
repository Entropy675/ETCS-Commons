#ifndef CHESSBOARD_H__
#define CHESSBOARD_H__
#include "ChessGame.h"
#include <cstring>
#include <functional>

/*
 * ── ChessBoard: the game, as pixels ───────────────────────────────────────
 *
 * A Drawable2D that owns its raster and draws a ChessGame into it: the eight
 * by eight, the pieces, the square that is picked and the move that was
 * last played. It is what the page and the window both show, so the board
 * is drawn ONCE, here, and neither substrate has a board of its own -- the
 * browser page used to keep a DOM grid, and that was a second board to keep
 * in step with the first.
 *
 * REDRAWN WHEN THE GAME CHANGED, not per frame. It rides the frame edge as
 * an Animated (the throbber's arrangement), and on each visit compares the
 * game's hash and its own marks against the last picture: the same, nothing;
 * different, one repaint. A game that nobody is moving in costs the frame
 * edge one hash a frame, which is a FEN and a string hash of it.
 *
 * THE PIECES ARE BITMAPS, twelve by fourteen, scaled by whole numbers into
 * the square. Drawn from a string in this file rather than a font or a file,
 * for the throbber's reason: nothing to ship, nothing to stage, the same
 * picture on every substrate. The outline is derived (a transparent cell
 * beside a filled one), so a piece reads on either square colour.
 *
 * WHICH WAY UP is the viewer's: a board flipped shows black at the bottom,
 * and every square asked of it (SquareAt) is answered in that orientation,
 * so the table clicking on it never learns which way it is drawn.
 */
class ChessBoard : public Drawable2DBase<ChessBoard>,
                   public PixelsBase<ChessBoard>,
                   public AnimatedBase<ChessBoard>,
                   public DeletableBase<ChessBoard>
{
public:
    WIRE_TYPE_IDENTITY(ChessBoard);

    int32_t m_order = 0;
    bool operator<(const ChessBoard& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    ChessBoard()  = default;
    ~ChessBoard() = default;

    static constexpr uint32_t DEFAULT_SIZE = 704;   // 88 a square

    bool Create(uint32_t size_px)
    {
        m_size = size_px ? size_px : DEFAULT_SIZE;
        m_size -= m_size % 8;
        this->Allocate(m_size, m_size);
        m_drawn.clear();
        this->addTag("active");
        return true;
    }

    void Bind(ETCS::RID game) { m_game = game; m_drawn.clear(); }
    void SetFlip(bool flip)   { m_flip = flip; }
    bool Flipped() const      { return m_flip; }
    // The square the table has picked ("" for none), drawn as such.
    void Select(const std::string& sq) { m_selected = sq; }
    const std::string& Selected() const { return m_selected; }

    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; etcs_mark_observed(this); }
    void SetOrder(int32_t z) { m_order = z; this->Reorder(); etcs_mark_observed(this); }
    uint32_t Size() const { return m_size; }
    uint32_t SquarePx() const { return m_size / 8; }

    // The square under a point in this raster's own space, "" outside.
    std::string SquareAt(int32_t x, int32_t y) const
    {
        const int32_t sq = static_cast<int32_t>(SquarePx());
        if (sq <= 0 || x < 0 || y < 0 || x >= static_cast<int32_t>(m_size) || y >= static_cast<int32_t>(m_size)) return "";
        int32_t file = x / sq, row = y / sq;
        if (m_flip) { file = 7 - file; row = 7 - row; }
        std::string s;
        s += static_cast<char>('a' + file);
        s += static_cast<char>('8' - row);
        return s;
    }

    // Something else's per-frame step, taken along on this node's visit --
    // the table is not in the drawable tree and has no edge of its own.
    void BindStep(std::function<void()> step) { m_step = std::move(step); }

    // ── Animated ──────────────────────────────────────────────────────────
    bool AnimatingConcrete() override
    {
        ChessGame* g = game();
        if (m_step) m_step();
        const std::string now = (g ? g->Hash() + "|" + g->LastMove() : std::string("-"))
                              + "|" + m_selected + (m_flip ? "|f" : "|u");
        if (now != m_drawn) { m_drawn = now; repaint(g); }
        return false;
    }
    void AdvanceConcrete(double) override {}

    // ── Drawable2D ────────────────────────────────────────────────────────
    Rect2D BoundsConcrete() override { return Rect2D{ m_x, m_y, m_size, m_size }; }
    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    { return x >= 0 && y >= 0 && x < static_cast<int32_t>(m_size) && y < static_cast<int32_t>(m_size); }
    WindowSize GetSizeConcrete() override { return WindowSize{ m_size, m_size }; }
    void ClearConcrete(float r, float g, float b, float a) override { this->ClearTo(r, g, b, a); }
    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h, float r, float g, float b, float a) override
    { this->FillRect(x, y, w, h, r, g, b, a); }
    void BlitConcrete(Surface_* source, int32_t x, int32_t y, uint32_t, uint32_t, float opacity) override
    {
        if (!source) return;
        void* raw = static_cast<ETCS::Entity*>(source)->getInterfacePointer(ETCS::Buffer("Pixels"));
        if (raw) this->Composite(*static_cast<Pixels_*>(raw), x, y, opacity);
    }
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;
        const Rect2D b = BoundsConcrete();
        const Point2D base = this->parentAbsoluteOrigin();
        if (Pixels_* raw = etcs_direct_pixels(static_cast<ETCS::Entity*>(dst)))
            render_composite_raw(*raw, this->PixelData(), m_size, m_size, base.x + b.x, base.y + b.y, 1.0f);
        else
            dst->Blit(this, base.x + b.x, base.y + b.y, 0, 0, 1.0f);
    }
    bool DeleteConcrete() override { return true; }
    bool ResizeTo(WindowSize s) override
    {
        const uint32_t side = (s.width < s.height) ? s.width : s.height;
        if (!side) return false;
        Create(side);
        return true;
    }
    bool MoveTo(Point2D p) override { SetPosition(p.x, p.y); return true; }

    // Where this raster sits in the window: the walk every leaf makes at
    // draw time, answered for a pointer that arrives in the window's frame.
    Point2D Origin() { const Point2D base = this->parentAbsoluteOrigin(); return Point2D{ base.x + m_x, base.y + m_y }; }

    ChessGame* game() const
    {
        ETCS::Entity* e = m_game ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_game) : nullptr;
        return e ? static_cast<ChessGame*>(e->getTrueType()) : nullptr;
    }

private:
    static constexpr int GW = 12, GH = 14;
    // '#' is the piece; the outline is every '.' touching one.
    static const char* glyph(char piece)
    {
        switch (piece)
        {
        case 'p': return
            "............"
            "............"
            ".....##....."
            "....####...."
            "....####...."
            ".....##....."
            "....####...."
            "...######..."
            "....####...."
            "....####...."
            "...######..."
            "..########.."
            ".##########."
            "............";
        case 'r': return
            "............"
            ".##.####.##."
            ".##.####.##."
            ".##########."
            "..########.."
            "...######..."
            "...######..."
            "...######..."
            "...######..."
            "...######..."
            "..########.."
            ".##########."
            ".##########."
            "............";
        case 'n': return
            "............"
            ".....##....."
            "....####...."
            "...######..."
            "..########.."
            ".##########."
            ".####.#####."
            ".###..#####."
            ".....#####.."
            "....#####..."
            "....#####..."
            "...#######.."
            "..#########."
            "............";
        case 'b': return
            "............"
            ".....##....."
            "....####...."
            "...######..."
            "...###.##..."
            "..####.###.."
            "..###.####.."
            "..########.."
            "...######..."
            "....####...."
            "...######..."
            "..########.."
            ".##########."
            "............";
        case 'q': return
            "............"
            ".#...##...#."
            ".##..##..##."
            ".###.##.###."
            ".##########."
            "..########.."
            "..########.."
            "...######..."
            "...######..."
            "....####...."
            "...######..."
            "..########.."
            ".##########."
            "............";
        case 'k': return
            ".....##....."
            "....####...."
            ".....##....."
            "...######..."
            "..########.."
            ".##########."
            ".##########."
            "..########.."
            "...######..."
            "....####...."
            "...######..."
            "..########.."
            ".##########."
            "............";
        }
        return nullptr;
    }

    static bool filled(const char* g, int x, int y)
    { return x >= 0 && y >= 0 && x < GW && y < GH && g[y * GW + x] == '#'; }

    void piece(int32_t px, int32_t py, char p)
    {
        const bool white = (p >= 'A' && p <= 'Z');
        const char* g = glyph(static_cast<char>(white ? p - 'A' + 'a' : p));
        if (!g) return;
        const int32_t sq = static_cast<int32_t>(SquarePx());
        int32_t s = (sq * 5 / 6) / GH; if (s < 1) s = 1;
        const int32_t ox = px + (sq - GW * s) / 2, oy = py + (sq - GH * s) / 2;
        const float fr = white ? 0.97f : 0.17f, fg = white ? 0.95f : 0.15f, fb = white ? 0.89f : 0.14f;
        const float lr = white ? 0.16f : 0.86f, lg = white ? 0.13f : 0.83f, lb = white ? 0.11f : 0.76f;
        for (int y = -1; y <= GH; ++y)
            for (int x = -1; x <= GW; ++x)
            {
                if (filled(g, x, y)) { this->FillRect(ox + x * s, oy + y * s, s, s, fr, fg, fb, 1.0f); continue; }
                if (filled(g, x - 1, y) || filled(g, x + 1, y) || filled(g, x, y - 1) || filled(g, x, y + 1))
                    this->FillRect(ox + x * s, oy + y * s, s, s, lr, lg, lb, 1.0f);
            }
    }

    void tint(const std::string& sq_name, float r, float g, float b, float a)
    {
        if (sq_name.size() < 2) return;
        int32_t file = sq_name[0] - 'a', row = '8' - sq_name[1];
        if (file < 0 || file > 7 || row < 0 || row > 7) return;
        if (m_flip) { file = 7 - file; row = 7 - row; }
        const int32_t sq = static_cast<int32_t>(SquarePx());
        this->FillRect(file * sq, row * sq, sq, sq, r, g, b, a);
    }

    void repaint(ChessGame* g)
    {
        if (m_size == 0) return;
        etcs_observed_batch _batch(this);
        const int32_t sq = static_cast<int32_t>(SquarePx());
        for (int32_t row = 0; row < 8; ++row)
            for (int32_t file = 0; file < 8; ++file)
            {
                const bool light = ((row + file) & 1) == 0;
                this->FillRect(file * sq, row * sq, sq, sq,
                               light ? 0.93f : 0.55f, light ? 0.87f : 0.40f, light ? 0.73f : 0.28f, 1.0f);
            }
        if (!g) return;
        const std::string last = g->LastMove();
        if (last.size() >= 4) { tint(last.substr(0, 2), 0.85f, 0.75f, 0.20f, 0.45f); tint(last.substr(2, 2), 0.85f, 0.75f, 0.20f, 0.45f); }
        if (!m_selected.empty()) tint(m_selected, 0.30f, 0.65f, 0.35f, 0.55f);

        // The FEN's first field, top rank first, is the board in reading order.
        const std::string fen = g->Fen();
        int32_t row = 0, file = 0;
        for (char c : fen)
        {
            if (c == ' ') break;
            if (c == '/') { ++row; file = 0; continue; }
            if (c >= '1' && c <= '8') { file += c - '0'; continue; }
            if (row > 7 || file > 7) break;
            int32_t dr = row, df = file;
            if (m_flip) { dr = 7 - dr; df = 7 - df; }
            piece(df * sq, dr * sq, c);
            ++file;
        }
    }

    ETCS::RID   m_game = 0;
    std::function<void()> m_step;
    int32_t     m_x = 0, m_y = 0;
    uint32_t    m_size = 0;
    bool        m_flip = false;
    std::string m_selected, m_drawn;
};

#endif // CHESSBOARD_H__
