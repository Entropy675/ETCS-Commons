#ifndef CHESSPROVIDER_CHESSBOARD_H__
#define CHESSPROVIDER_CHESSBOARD_H__
#include "ChessGame.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/*
 * ── ChessBoard: the game, as pixels ───────────────────────────────────────
 *
 * A Drawable2D that owns its raster and draws a ChessGame into it: the eight
 * by eight, the pieces, the square that is picked and the move that was
 * last played. The page and the window both show it, so the board is drawn
 * ONCE, here.
 *
 * REDRAWN WHEN WHAT IT SHOWS CHANGED, not per frame: it rides the frame edge
 * as an Animated and compares a key of everything drawn (the game's hash,
 * the ply shown, the pick, the sets) against the last picture.
 *
 * THE PIECES ARE A SET OF PICTURES, CChess's (`piece_images/<set>/`), loaded
 * by name (LoadSet) and chosen per side (Skin). Rook, bishop, knight and pawn
 * come as a left and a right picture; a piece keeps the one of the half of
 * the board it started on, followed through the history (homeSides). With no
 * set loaded the pieces are the twelve-by-fourteen bitmaps below.
 *
 * WHAT IS SHOWN IS A PLY OF THE GAME'S HISTORY: the last one, or the one
 * under review (Review). Reviewing draws; it never moves.
 *
 * WHICH WAY UP is the viewer's: flipped, black is at the bottom, and every
 * square asked of it (SquareAt, PromoAt) is answered in that orientation.
 */
class ChessBoard : public Drawable2DBase<ChessBoard>,
                   public PixelsBase<ChessBoard>,
                   public AnimatedBase<ChessBoard>,
                   public DeletableBase<ChessBoard>
{
public:
    WIRE_TYPE_IDENTITY(ChessBoard);

    int32_t m_order = 0;
    // Hidden loses every comparison; then the stated order.
    bool operator<(const ChessBoard& o) const
    { return Hidden() != o.Hidden() ? Hidden() : m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    ChessBoard()  = default;
    ~ChessBoard() = default;

    static constexpr uint32_t DEFAULT_SIZE = 704;   // 88 a square

    bool Create(uint32_t size_px)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_size = size_px ? size_px : DEFAULT_SIZE;
        m_size -= m_size % 8;
        this->Allocate(m_size, m_size);
        m_drawn.clear();
        m_scaled_for = 0;
        this->addTag("active");
        return true;
    }

    void Bind(ETCS::RID game) { std::lock_guard<std::mutex> g(m_mu); m_game = game; m_drawn.clear(); }
    void SetFlip(bool flip)   { std::lock_guard<std::mutex> g(m_mu); m_flip = flip; }
    bool Flipped() const      { return m_flip; }
    // The square the table has picked ("" for none), drawn as such.
    void Select(const std::string& sq) { std::lock_guard<std::mutex> g(m_mu); m_selected = sq; }
    std::string Selected() const { std::lock_guard<std::mutex> g(m_mu); return m_selected; }

    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; etcs_mark_observed(this); }
    void SetOrder(int32_t z) { m_order = z; this->Reorder(); etcs_mark_observed(this); }
    uint32_t Size() const { return m_size; }
    uint32_t SquarePx() const { return m_size / 8; }

    // ── the pieces ────────────────────────────────────────────────────────
    /*
     * A set from a directory of CChess's pictures: King, Queen, and a left and
     * right Rook, Bishop, Knight and Pawn, `.png`, the first letter of either
     * case (the brown set spells some lower). A path that does not open as
     * given is tried under ACE_ROOT/modules, so one spelling serves the page
     * (staged at that path) and a runtime run from anywhere. Answers how many
     * of the ten pictures it took; a set with no king is not kept.
     */
    int LoadSet(const std::string& name, const std::string& dir)
    {
        PieceSet set;
        int took = 0;
        for (int k = 0; k < 6; ++k)
            for (int v = 0; v < 2; ++v)
            {
                if (k < 2 && v == 1) continue;                     // a king and a queen are one picture
                const std::string stem = std::string(kStems[k]) + (k < 2 ? "" : (v == 0 ? "L" : "R"));
                if (readSprite(dir, stem, set.img[k][v])) ++took;
            }
        for (int k = 0; k < 6; ++k)
        {
            if (set.img[k][1].w == 0) set.img[k][1] = set.img[k][0];   // one picture serves both halves
            if (set.img[k][0].w == 0) set.img[k][0] = set.img[k][1];
        }
        if (set.img[0][0].w == 0)
        {
            ETCS_LOG("ChessBoard", "LoadSet " << name << ": no King.png under '" << dir << "' -- not kept.");
            return 0;
        }
        std::lock_guard<std::mutex> g(m_mu);
        m_sets[name] = std::move(set);
        m_scaled_for = 0;
        m_drawn.clear();
        ETCS_LOG("ChessBoard", "set '" << name << "': " << took << " of 10 pictures from '" << dir << "'.");
        return took;
    }
    // Which set each side is drawn in, by name; "" for the bitmaps.
    bool Skin(const std::string& white, const std::string& black)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const bool ok = (white.empty() || m_sets.count(white)) && (black.empty() || m_sets.count(black));
        if (!ok) return false;
        m_white_set = white; m_black_set = black;
        m_scaled_for = 0;
        m_drawn.clear();
        return true;
    }
    // "<white set> <black set>|<every set loaded>".
    std::string Sets() const
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out = (m_white_set.empty() ? "-" : m_white_set) + " " + (m_black_set.empty() ? "-" : m_black_set) + "|";
        bool first = true;
        for (const auto& s : m_sets) { out += (first ? "" : " ") + s.first; first = false; }
        return out;
    }

    // ── what is shown ─────────────────────────────────────────────────────
    // The ply under review (-1: the live board). Clamped to what the history
    // holds when drawn, so a reset under a reviewer shows the start.
    void Review(int64_t ply) { std::lock_guard<std::mutex> g(m_mu); m_review = ply; }
    int64_t Reviewing() const { std::lock_guard<std::mutex> g(m_mu); return m_review; }

    // The promotion picker: the four pieces a pawn on `sq` may become, in the
    // mover's set, drawn over the board from that square inward. "" clears it.
    void Promote(const std::string& sq, bool white) { std::lock_guard<std::mutex> g(m_mu); m_promo = sq; m_promo_white = white; }
    bool Promoting() const { std::lock_guard<std::mutex> g(m_mu); return !m_promo.empty(); }
    // The piece of the picker under a point in this raster's own space
    // ('q', 'n', 'r', 'b'), 0 off it.
    char PromoAt(int32_t x, int32_t y) const
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int32_t sq = static_cast<int32_t>(SquarePx());
        if (m_promo.size() < 2 || sq <= 0 || x < 0 || y < 0) return 0;
        int32_t file = 0, row = 0;
        if (!drawnCell(m_promo, file, row)) return 0;
        const int32_t step = row < 4 ? 1 : -1;
        if (x / sq != file) return 0;
        for (int i = 0; i < 4; ++i) if (y / sq == row + step * i) return kPromo[i];
        return 0;
    }

    // What a square shows at the ply shown: "<piece letter> <L|R>", "-" empty.
    std::string PictureAt(const std::string& sq) const
    {
        const int c = cellOf(sq);
        ChessGame* g = game();
        if (c < 0 || !g) return "-";
        std::lock_guard<std::mutex> hold(m_mu);
        std::string fen, last;
        std::array<uint8_t, 64> half{};
        shownLocked(g, fen, last, half);
        char cells[64];
        placement(fen, cells);
        if (cells[c] == ' ') return "-";
        return std::string(1, cells[c]) + (half[c] ? " R" : " L");
    }

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
        if (m_step) m_step();
        ChessGame* g = game();
        std::lock_guard<std::mutex> hold(m_mu);
        const std::string now = (g ? g->Hash() : std::string("-")) + "|" + std::to_string(m_review) + "|" + m_selected
                              + "|" + m_promo + (m_flip ? "|f|" : "|u|") + m_white_set + "|" + m_black_set;
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
    struct Sprite { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; };
    // [K Q R B N P][left, right]
    struct PieceSet { Sprite img[6][2]; };

    static constexpr const char* kStems[6] = { "King", "Queen", "Rook", "Bishop", "Knight", "Pawn" };
    static constexpr const char  kKinds[7] = "kqrbnp";
    static constexpr char        kPromo[4] = { 'q', 'n', 'r', 'b' };

    // The board in ACE's colours: the site's light and dark, and its gold.
    static constexpr float kLight[3] = { 0.910f, 0.886f, 0.835f };
    static constexpr float kDark[3]  = { 0.478f, 0.541f, 0.435f };
    static constexpr float kGold[3]  = { 0.788f, 0.706f, 0.345f };

    static int kindOf(char p)
    {
        const char l = (p >= 'A' && p <= 'Z') ? static_cast<char>(p - 'A' + 'a') : p;
        for (int k = 0; k < 6; ++k) if (kKinds[k] == l) return k;
        return -1;
    }

    static bool readFile(const std::string& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
#ifdef ETCS_ACE_ROOT
        if (!in && !path.empty() && path[0] != '/') in.open(std::string(ETCS_ACE_ROOT) + "/modules/" + path, std::ios::binary);
#endif
        if (!in) return false;
        std::ostringstream s;
        s << in.rdbuf();
        out = s.str();
        return !out.empty();
    }
    static bool readSprite(const std::string& dir, const std::string& stem, Sprite& out)
    {
        std::string bytes;
        const std::string lower = std::string(1, static_cast<char>(stem[0] - 'A' + 'a')) + stem.substr(1);
        if (!readFile(dir + "/" + stem + ".png", bytes) && !readFile(dir + "/" + lower + ".png", bytes)) return false;
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                                  static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return false; }
        out.w = static_cast<uint32_t>(w); out.h = static_cast<uint32_t>(h);
        out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
        return true;
    }

    // A picture shrunk so its height is the square's: each pixel the average
    // of the block it covers, weighted by alpha, so the edges stay clean.
    static Sprite shrink(const Sprite& s, uint32_t sq)
    {
        Sprite d;
        if (s.w == 0 || s.h == 0 || sq == 0) return d;
        d.h = sq;
        d.w = static_cast<uint32_t>((static_cast<uint64_t>(s.w) * sq + s.h / 2) / s.h);
        if (d.w == 0) d.w = 1;
        d.rgba.assign(static_cast<size_t>(d.w) * d.h * 4, 0);
        for (uint32_t y = 0; y < d.h; ++y)
        {
            const uint32_t y0 = y * s.h / d.h, y1 = std::max(y0 + 1, (y + 1) * s.h / d.h);
            for (uint32_t x = 0; x < d.w; ++x)
            {
                const uint32_t x0 = x * s.w / d.w, x1 = std::max(x0 + 1, (x + 1) * s.w / d.w);
                uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
                for (uint32_t sy = y0; sy < y1 && sy < s.h; ++sy)
                    for (uint32_t sx = x0; sx < x1 && sx < s.w; ++sx)
                    {
                        const uint8_t* p = &s.rgba[(static_cast<size_t>(sy) * s.w + sx) * 4];
                        r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; ++n;
                    }
                uint8_t* q = &d.rgba[(static_cast<size_t>(y) * d.w + x) * 4];
                if (a) { q[0] = static_cast<uint8_t>(r / a); q[1] = static_cast<uint8_t>(g / a); q[2] = static_cast<uint8_t>(b / a); }
                q[3] = static_cast<uint8_t>(n ? a / n : 0);
            }
        }
        return d;
    }

    // Source-over, unpremultiplied, clipped to the raster.
    void stamp(const Sprite& s, int32_t ox, int32_t oy)
    {
        uint8_t* dp = this->PixelData();
        if (!dp || s.w == 0) return;
        const int32_t W = static_cast<int32_t>(m_size);
        for (uint32_t y = 0; y < s.h; ++y)
        {
            const int32_t dy = oy + static_cast<int32_t>(y);
            if (dy < 0 || dy >= W) continue;
            for (uint32_t x = 0; x < s.w; ++x)
            {
                const int32_t dx = ox + static_cast<int32_t>(x);
                if (dx < 0 || dx >= W) continue;
                const uint8_t* p = &s.rgba[(static_cast<size_t>(y) * s.w + x) * 4];
                const uint32_t a = p[3];
                if (a == 0) continue;
                uint8_t* q = dp + (static_cast<size_t>(dy) * W + dx) * 4;
                for (int c = 0; c < 3; ++c) q[c] = static_cast<uint8_t>((p[c] * a + q[c] * (255 - a)) / 255);
                q[3] = static_cast<uint8_t>(a + q[3] * (255 - a) / 255);
            }
        }
    }

    // The sets in hand, at this square size: [white K..P, black K..P][left, right].
    void rescale()
    {
        const uint32_t sq = SquarePx();
        if (m_scaled_for == sq) return;
        m_scaled_for = sq;
        for (int side = 0; side < 2; ++side)
        {
            auto it = m_sets.find(side == 0 ? m_white_set : m_black_set);
            for (int k = 0; k < 6; ++k)
                for (int v = 0; v < 2; ++v)
                    m_scaled[side * 6 + k][v] = (it == m_sets.end()) ? Sprite{} : shrink(it->second.img[k][v], sq);
        }
    }

    // ── the bitmaps, when no set is in hand ──
    static constexpr int GW = 12, GH = 14;
    // '#' is the piece; the outline is every '.' touching one.
    static const char* glyph(char piece)
    {
        switch (piece)
        {
        case 'p': return
            "............" "............" ".....##....." "....####...." "....####...." ".....##....." "....####...."
            "...######..." "....####...." "....####...." "...######..." "..########.." ".##########." "............";
        case 'r': return
            "............" ".##.####.##." ".##.####.##." ".##########." "..########.." "...######..." "...######..."
            "...######..." "...######..." "...######..." "..########.." ".##########." ".##########." "............";
        case 'n': return
            "............" ".....##....." "....####...." "...######..." "..########.." ".##########." ".####.#####."
            ".###..#####." ".....#####.." "....#####..." "....#####..." "...#######.." "..#########." "............";
        case 'b': return
            "............" ".....##....." "....####...." "...######..." "...###.##..." "..####.###.." "..###.####.."
            "..########.." "...######..." "....####...." "...######..." "..########.." ".##########." "............";
        case 'q': return
            "............" ".#...##...#." ".##..##..##." ".###.##.###." ".##########." "..########.." "..########.."
            "...######..." "...######..." "....####...." "...######..." "..########.." ".##########." "............";
        case 'k': return
            ".....##....." "....####...." ".....##....." "...######..." "..########.." ".##########." ".##########."
            "..########.." "...######..." "....####...." "...######..." "..########.." ".##########." "............";
        }
        return nullptr;
    }
    static bool filled(const char* g, int x, int y)
    { return x >= 0 && y >= 0 && x < GW && y < GH && g[y * GW + x] == '#'; }
    void bitmap(int32_t px, int32_t py, char p)
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

    // A piece at a drawn cell: its set's picture for its half, else the bitmap.
    void piece(int32_t px, int32_t py, char p, int half)
    {
        const int k = kindOf(p);
        const int side = (p >= 'A' && p <= 'Z') ? 0 : 1;
        if (k >= 0)
        {
            const Sprite& s = m_scaled[side * 6 + k][half ? 1 : 0];
            if (s.w)
            {
                const int32_t sq = static_cast<int32_t>(SquarePx());
                stamp(s, px + (sq - static_cast<int32_t>(s.w)) / 2, py + (sq - static_cast<int32_t>(s.h)) / 2);
                return;
            }
        }
        bitmap(px, py, p);
    }

    // ── positions ──
    // A FEN's placement as 64 letters, rank 8 first (' ' empty).
    static void placement(const std::string& fen, char out[64])
    {
        std::memset(out, ' ', 64);
        int row = 0, file = 0;
        for (char c : fen)
        {
            if (c == ' ') break;
            if (c == '/') { ++row; file = 0; continue; }
            if (c >= '1' && c <= '8') { file += c - '0'; continue; }
            if (row < 8 && file < 8) out[row * 8 + file] = c;
            ++file;
        }
    }
    static int cellOf(const std::string& sq)
    {
        if (sq.size() < 2 || sq[0] < 'a' || sq[0] > 'h' || sq[1] < '1' || sq[1] > '8') return -1;
        return ('8' - sq[1]) * 8 + (sq[0] - 'a');
    }
    /*
     * WHICH HALF EACH PIECE STARTED ON, at ply `upto` of `plies` ("<uci>
     * <fen>" a line, the first the root): every piece of the root takes the
     * half its file is in, and each move carries its piece's half to where it
     * lands -- a castle carries the rook's too. A promoted pawn keeps its own.
     */
    static std::array<uint8_t, 64> homeSides(const std::vector<std::string>& plies, size_t upto)
    {
        std::array<uint8_t, 64> side{};
        for (int c = 0; c < 64; ++c) side[c] = (c % 8) < 4 ? 0 : 1;
        if (plies.empty()) return side;
        char before[64];
        placement(plies[0].substr(plies[0].find(' ') + 1), before);
        for (size_t i = 1; i <= upto && i < plies.size(); ++i)
        {
            const std::string& l = plies[i];
            const int from = cellOf(l.substr(0, 2)), to = cellOf(l.size() >= 4 ? l.substr(2, 2) : "");
            if (from >= 0 && to >= 0)
            {
                const char mover = before[from];
                side[to] = side[from];
                if ((mover == 'K' || mover == 'k') && (to % 8 - from % 8 == 2 || from % 8 - to % 8 == 2))
                {
                    const int row = from / 8, kingside = to % 8 > from % 8;
                    side[row * 8 + (kingside ? 5 : 3)] = side[row * 8 + (kingside ? 7 : 0)];
                }
            }
            placement(l.substr(l.find(' ') + 1), before);
        }
        return side;
    }

    // A square's cell as drawn (flip applied).
    bool drawnCell(const std::string& sq, int32_t& file, int32_t& row) const
    {
        if (sq.size() < 2) return false;
        file = sq[0] - 'a'; row = '8' - sq[1];
        if (file < 0 || file > 7 || row < 0 || row > 7) return false;
        if (m_flip) { file = 7 - file; row = 7 - row; }
        return true;
    }
    void tint(const std::string& sq_name, float r, float g, float b, float a)
    {
        int32_t file = 0, row = 0;
        if (!drawnCell(sq_name, file, row)) return;
        const int32_t sq = static_cast<int32_t>(SquarePx());
        this->FillRect(file * sq, row * sq, sq, sq, r, g, b, a);
    }
    void frame(const std::string& sq_name, float r, float g, float b, float a)
    {
        int32_t file = 0, row = 0;
        if (!drawnCell(sq_name, file, row)) return;
        const int32_t sq = static_cast<int32_t>(SquarePx()), t = std::max<int32_t>(2, sq / 28);
        const int32_t x = file * sq, y = row * sq;
        this->FillRect(x, y, sq, t, r, g, b, a);
        this->FillRect(x, y + sq - t, sq, t, r, g, b, a);
        this->FillRect(x, y + t, t, sq - 2 * t, r, g, b, a);
        this->FillRect(x + sq - t, y + t, t, sq - 2 * t, r, g, b, a);
    }

    // The ply shown -- the last, or the one under review -- as its position,
    // its move ("" for none) and the half each piece started on.
    void shownLocked(ChessGame* g, std::string& fen, std::string& last, std::array<uint8_t, 64>& half) const
    {
        size_t base = 0;
        const std::vector<std::string> plies = g->Plies(base);
        fen = g->Fen(); last = g->LastMove();
        size_t at = plies.empty() ? 0 : plies.size() - 1;
        if (m_review >= 0 && !plies.empty())
        {
            const int64_t i = m_review - static_cast<int64_t>(base);
            at = static_cast<size_t>(i < 0 ? 0 : (i >= static_cast<int64_t>(plies.size()) ? plies.size() - 1 : i));
            const std::string& l = plies[at];
            const size_t sp = l.find(' ');
            fen = l.substr(sp + 1);
            last = l.substr(0, sp);
            if (last == "-") last.clear();
        }
        half = homeSides(plies, at);
    }

    void repaint(ChessGame* g)
    {
        if (m_size == 0) return;
        etcs_observed_batch _batch(this);
        rescale();
        const int32_t sq = static_cast<int32_t>(SquarePx());
        for (int32_t row = 0; row < 8; ++row)
            for (int32_t file = 0; file < 8; ++file)
            {
                const float* c = ((row + file) & 1) == 0 ? kLight : kDark;
                this->FillRect(file * sq, row * sq, sq, sq, c[0], c[1], c[2], 1.0f);
            }
        if (!g) return;

        std::string fen, last;
        std::array<uint8_t, 64> half{};
        shownLocked(g, fen, last, half);
        if (last.size() >= 4) { tint(last.substr(0, 2), 0.44f, 0.31f, 0.0f, 0.30f); tint(last.substr(2, 2), 0.44f, 0.31f, 0.0f, 0.30f); }
        if (!m_selected.empty() && m_review < 0) frame(m_selected, kGold[0], kGold[1], kGold[2], 1.0f);

        char cells[64];
        placement(fen, cells);
        for (int c = 0; c < 64; ++c)
        {
            if (cells[c] == ' ') continue;
            int32_t dr = c / 8, df = c % 8;
            if (m_flip) { dr = 7 - dr; df = 7 - df; }
            piece(df * sq, dr * sq, cells[c], half[c]);
        }

        // The picker last, over everything: the four pieces from the square inward.
        int32_t pf = 0, pr = 0;
        if (!m_promo.empty() && drawnCell(m_promo, pf, pr))
        {
            const int32_t step = pr < 4 ? 1 : -1;
            for (int i = 0; i < 4; ++i)
            {
                const int32_t y = (pr + step * i) * sq;
                this->FillRect(pf * sq, y, sq, sq, kLight[0], kLight[1], kLight[2], 1.0f);
                const char p = m_promo_white ? static_cast<char>(kPromo[i] - 'a' + 'A') : kPromo[i];
                piece(pf * sq, y, p, (pf < 4) ? 0 : 1);
            }
            for (int i = 0; i < 4; ++i)
            {
                const int32_t r = pr + step * i;
                std::string cell;
                cell += static_cast<char>('a' + (m_flip ? 7 - pf : pf));
                cell += static_cast<char>('8' - (m_flip ? 7 - r : r));
                frame(cell, kGold[0], kGold[1], kGold[2], 1.0f);
            }
        }
    }

    mutable std::mutex m_mu;
    ETCS::RID   m_game = 0;
    std::function<void()> m_step;
    int32_t     m_x = 0, m_y = 0;
    uint32_t    m_size = 0;
    bool        m_flip = false;
    std::string m_selected, m_drawn;
    int64_t     m_review = -1;
    std::string m_promo;
    bool        m_promo_white = true;
    std::map<std::string, PieceSet> m_sets;
    std::string m_white_set, m_black_set;
    Sprite      m_scaled[12][2];
    uint32_t    m_scaled_for = 0;
};

#endif // CHESSPROVIDER_CHESSBOARD_H__
