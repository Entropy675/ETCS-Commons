#ifndef ETCS_RENDERPROVIDER_THROBBER_H__
#define ETCS_RENDERPROVIDER_THROBBER_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "TextLabel.h"

#include <cmath>
#include <string>

/*
 * ── Throbber ───────────────────────────────────────────────────────────────
 *
 * "ETCS" ABOVE A RING OF CYCLING DOTS. One entity, one raster, any size, no
 * asset. The thing you put on screen while something is taking a while.
 *
 * WHY THIS IS NOT PRE-BAKED FRAMES, which is the obvious alternative and very
 * nearly the same thing. It IS a frame sequence -- N dots at fixed positions,
 * each one's brightness a function of how far the sweep has got -- and the only
 * question is where those frames live. Baked, they are bytes: one set per size,
 * a resampler or a blur for every other size, an encoder and a decoder to get
 * them in, and a build step that can be forgotten. Computed, they are one
 * formula: the size is a parameter, the dot count is a parameter, the colours
 * are parameters, and there is nothing to ship alongside the binary. For a
 * shape this simple the formula is smaller than the asset pipeline would be.
 *
 * The honest cost of computing them: FillDisc is an exactly-round DISCRETE
 * circle with no antialiasing (ontology/Pixels.h), so at a small size the dots
 * are visibly chunky where a baked frame could have been smoothed. That is the
 * one thing baking would buy, and it is why the default size here is not tiny.
 *
 * SMOOTHNESS IS THE OPACITY CURVE, NOT THE FRAME RATE, and this is the part
 * worth getting right. The naive version lights dot k where k = floor(phase*N),
 * which steps -- N discrete states per revolution, and running the frame edge
 * faster gets you the same N states shown more times each. Here every dot's
 * opacity is a CONTINUOUS falloff in its angular distance from the sweep head,
 * so between two dots the leading one is already coming up while the trailing
 * one is still going down. The picture changes on every tick at any rate, and
 * more dots make it smoother rather than merely finer.
 *
 * AND THE SWEEP IS FRAME-COUPLED, WHICH TAKES NOTHING TO ARRANGE. Advance() is
 * already called once per frame-edge visit -- that is the family's hook and it
 * has always been per-frame. AnimatedBase's one addition is a StepClock that
 * measures the gap and hands it down, so a leaf can be rate-independent if it
 * wants to be. This one declines it: a fixed step per visit, dt dropped. Faster
 * edge, faster sweep; struggling edge, visibly slower spinner. See
 * AdvanceConcrete for why that is the right answer for this widget in
 * particular, and not a thing to generalise.
 *
 * COLOUR: two shades, lerped across the tail, so the head and the tail are not
 * the same hue at different alphas -- that reads as a fading dot rather than as
 * a moving one. The defaults are the canvas pane's own ruler highlight
 * (0.35, 0.55, 0.95 -- PaintProvider's in-band tick colour) and a violet shift
 * of it at the same brightness, so the throbber belongs to the same palette as
 * the window it appears over rather than introducing a third accent.
 *
 * TEXT: off-white (0.94, 0.89, 0.78), which is the value this tree already uses
 * for foreground ink on a dark pane. Off- rather than pure white on purpose: a
 * pure #ffffff label is the one thing on a dark page that can look like a
 * rendering error, and this has to read as deliberate on every background a
 * caller might put it on.
 *
 * ONE ENTITY, INCLUDING THE LABEL. The text could have been a sibling the
 * script spawns, which is the convention for a widget's look
 * (paint_wheel.etcs) -- but then a caller has to place two things relative to
 * each other and re-place both to change the size, and "ETCS above a throbber"
 * is one thing to a caller. So the label is a child of this node, driven from
 * here through the Glyphs family: a child TextLabel rasterises the run into
 * THIS raster (Glyphs_::RasterizeText takes the target by RID), so there is one
 * buffer, one composite, and the font stays in the one type that owns it.
 *
 * VISIBILITY IS THE SWITCH, and it is already the right one. SetHidden is
 * honoured in draw and in pick, skips children with the parent, and marks the
 * Observable edge so whatever holds a merged copy rebuilds without it
 * (ontology/DrawableBase.h). AnimatingConcrete reads it, so a hidden throbber
 * costs exactly one virtual call per frame and advances nothing -- there is no
 * second "is it running" fact to keep in step with "is it showing".
 */
class Throbber : public Drawable2DBase<Throbber>,
                 public PixelsBase<Throbber>,
                 public AnimatedBase<Throbber>,
                 public DeletableBase<Throbber>
{
public:
    WIRE_TYPE_IDENTITY(Throbber);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;   // Create sets ORDER_ON_TOP; see below
    bool operator<(const Throbber& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    Throbber()  = default;
    ~Throbber() = default;

    // The ring's outer diameter in pixels. 96 because of the antialiasing note
    // above: below about 48 the dots stop looking round, and a throbber is
    // almost always on an otherwise empty screen where there is room.
    static constexpr uint32_t DEFAULT_SIZE = 96;
    static constexpr uint32_t DEFAULT_DOTS = 12;
    // Degrees of sweep per FRAME, not per second -- see AdvanceConcrete. 5.4 is
    // 0.9 revolutions a second at the frame edge's default 16ms, which is where
    // this started and still the pace it reads best at.
    static constexpr float    DEFAULT_STEP = 5.4f;

    // How much of the ring is lit behind the head, in turns. 0.45 is a bit
    // under half, which keeps a clear gap: a tail that closes the circle reads
    // as a pulsing ring rather than as something going round.
    static constexpr float    TAIL_TURNS   = 0.45f;

    /*
 * ON TOP, BY DEFAULT AND BY NATURE.
 *
 * A wait indicator that something else covers is a wait indicator that is not
 * there, and unlike every other order in a scene that is not a layout decision
 * -- it is what the thing IS. So the type carries it rather than every caller
 * remembering a big number, and a script that genuinely wants one behind
 * something still says SetOrder like anything else.
 *
 * Large rather than merely larger than today's highest: the panes in
 * boot_paint_panels.etcs sit at 29, and a default of 30 would be a number that
 * silently stops being enough the first time somebody adds a pane above them.
 */
    static constexpr int32_t  ORDER_ON_TOP = 1000000;

    bool Create(uint32_t size_px)
    {
        SetSize(size_px ? size_px : DEFAULT_SIZE);
        SetOrder(ORDER_ON_TOP);
        this->addTag("active");
        return true;
    }

    /*
 * Size is the one parameter that reallocates, so it is also the one that
 * rebuilds the label. Everything else just changes what the next repaint draws.
 *
 * The label's size is derived, not separately settable: a caller asking for a
 * 96px throbber wants a 96px throbber, and a text size that has to be chosen to
 * match is a second number that can disagree with the first. A quarter of the
 * ring, floored to the font's own cell (TextLabel::SetSize rounds anyway), is
 * the proportion that keeps "ETCS" narrower than the ring at every size.
 */
    void SetSize(uint32_t size_px)
    {
        m_ring = size_px ? size_px : DEFAULT_SIZE;

        uint32_t text_px = m_ring / 4;
        if (text_px < TextLabel::CELL_H) text_px = TextLabel::CELL_H;

        ensureLabel();
        if (m_label) m_label->SetSize(text_px);

        const uint32_t text_h = m_label ? m_label->Scale() * TextLabel::CELL_H : 0;
        const uint32_t text_w = m_label
            ? static_cast<uint32_t>(m_label->MeasureTextConcrete(m_text.c_str(), 0, 0).width)
            : 0;

        m_gap    = m_ring / 8;
        m_text_h = text_h;
        m_w = (text_w > m_ring) ? text_w : m_ring;
        m_h = text_h + m_gap + m_ring;

        this->Allocate(m_w, m_h);
        repaint();
    }

    void SetText(const std::string& text)
    {
        m_text = text.empty() ? std::string("ETCS") : text;
        SetSize(m_ring);          // the run's width is part of the layout
    }

    /*
 * Degrees per frame. Clamped rather than refused: a quarter turn a frame is
 * already a throbber you cannot follow, and one that has stopped being legible
 * is worse than one slower than asked for.
 *
 * PER FRAME is the unit on purpose and the whole point of this type's timing --
 * see AdvanceConcrete. A caller who wants to think in revolutions a second
 * multiplies by the frame interval they set on RunFrames, and gets a number
 * that stops being true the moment the edge cannot keep up, which is the
 * honest situation rather than a hidden one.
 */
    void SetStep(float degrees_per_frame)
    {
        if (degrees_per_frame < 0.05f) degrees_per_frame = 0.05f;
        if (degrees_per_frame > 90.0f) degrees_per_frame = 90.0f;
        m_step = degrees_per_frame;
    }

    // More dots is smoother, not finer -- see the header. Two is the least that
    // can express a direction; above about twenty-four they overlap at any size
    // this draws at and the extra ones are cost with no picture in them.
    void SetDots(uint32_t count)
    {
        if (count < 2)  count = 2;
        if (count > 24) count = 24;
        m_dots = count;
        repaint();
    }

    void SetColors(float ar, float ag, float ab,
                   float br, float bg, float bb)
    {
        m_head[0] = ar; m_head[1] = ag; m_head[2] = ab;
        m_tail[0] = br; m_tail[1] = bg; m_tail[2] = bb;
        repaint();
    }

    void SetTextColor(float r, float g, float b, float a)
    {
        m_ink[0] = r; m_ink[1] = g; m_ink[2] = b; m_ink[3] = a;
        repaint();
    }

    void SetPosition(int32_t x, int32_t y)
    {
        m_x = x; m_y = y;
        etcs_mark_observed(this);
    }

    void SetOrder(int32_t z) { m_order = z; this->Reorder(); etcs_mark_observed(this); }

    // ── Animated_ ────────────────────────────────────────────────────────
    //
    // Hidden is the whole answer. See the header note on why there is no
    // separate running flag.
    bool AnimatingConcrete() override { return !this->Hidden(); }

    /*
 * ONE STEP PER VISIT: dt_ms IS TAKEN AND DROPPED.
 *
 * Nothing about the DRIVING changes here -- Advance() is called once per
 * frame-edge visit either way, which is what the family's hook has always been.
 * The only question a leaf answers in this method is whether to consume the
 * measured interval AnimatedBase hands it, and this one says no.
 *
 * For anything that models a real duration -- a fade, a key repeat, a camera --
 * the answer must be yes: those describe seconds, and a fade that finishes
 * sooner on a faster machine is a bug. A THROBBER MODELS NOTHING. It is not
 * showing the progress of anything, only that the runtime is still turning, so
 * there is no duration for it to get wrong.
 *
 * Given that, spending the dt makes it a READOUT instead: raise the frame edge
 * and it picks up with everything else, and an edge that is struggling shows as
 * a spinner visibly slowing down. Free diagnosis on the one widget certainly on
 * screen while you are waiting -- where a spinner keeping perfect time through a
 * stall is the one shape that actively hides the problem.
 *
 * dt is dropped rather than the signature narrowed, so the family's contract is
 * untouched and this is revertible in one line. If a caller wants a real-time
 * sweep, the frame interval is the knob (Surface::RunFrames).
 *
 * fmod, not a while-loop subtract: SetStep's ceiling is a quarter turn, so one
 * visit can never cross a whole revolution -- but a loop bounded by somebody
 * else's parameter is a loop with no bound stated here.
 */
    void AdvanceConcrete(double dt_ms) override
    {
        (void)dt_ms;
        m_phase += m_step * (1.0f / 360.0f);
        m_phase = std::fmod(m_phase, 1.0f);
        if (m_phase < 0.0f) m_phase += 1.0f;
        repaint();
    }

    // ── Drawable2D_ / Surface_ dispatch ──────────────────────────────────
    Rect2D BoundsConcrete() override { return Rect2D{ m_x, m_y, m_w, m_h }; }

    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    {
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(m_w) && y < static_cast<int32_t>(m_h);
    }

    WindowSize GetSizeConcrete() override { return WindowSize{ m_w, m_h }; }

    void ClearConcrete(float r, float g, float b, float a) override
    { this->ClearTo(r, g, b, a); }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) override
    { this->FillRect(x, y, w, h, r, g, b, a); }

    /*
 * Blit into this node is 1:1 and w/h are discarded, for the reason
 * CompositeDrawable2D states for the same override: a scaling blit INTO a
 * buffer would resample twice, once here and once when this buffer reaches the
 * destination. Nothing blits into a throbber in practice; it is written because
 * the family requires an answer and a silently wrong one is worse than a stated
 * limitation.
 */
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, float opacity) override
    {
        (void)w; (void)h;
        if (!source) return;
        void* raw = static_cast<ETCS::Entity*>(source)
                        ->getInterfacePointer(ETCS::Buffer("Pixels"));
        if (!raw) return;
        this->Composite(*static_cast<Pixels_*>(raw), x, y, opacity);
    }

    /*
 * The raster goes out, the children do not draw themselves into the
 * destination. The label IS a child, but it is rasterised into this buffer by
 * repaint() rather than drawn past it -- one buffer, and the label cannot land
 * at a screen position inside a buffer that starts at its own top-left, which
 * is what a coordinate origin means (ontology/Drawable2DBase.h).
 */
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;
        const Rect2D b = BoundsConcrete();
        const Point2D base = this->parentAbsoluteOrigin();
        void* raw = static_cast<ETCS::Entity*>(dst)
                        ->getInterfacePointer(ETCS::Buffer("Pixels"));
        if (raw)
            render_composite_raw(*static_cast<Pixels_*>(raw),
                                 this->PixelData(), m_w, m_h,
                                 base.x + b.x, base.y + b.y, 1.0f);
        else
            dst->Blit(this, base.x + b.x, base.y + b.y, 0, 0, 1.0f);
    }

    bool DeleteConcrete() override { return true; }

    // ── Resizable_ ───────────────────────────────────────────────────────
    //
    // A layout solver writing a box back onto a throbber means "be this big",
    // and for this node size is one number -- so the smaller side wins and the
    // ring fits either way round.
    bool ResizeTo(WindowSize s) override
    {
        const uint32_t side = (s.width < s.height) ? s.width : s.height;
        if (!side) return false;
        SetSize(side);
        return true;
    }

    bool MoveTo(Point2D p) override { SetPosition(p.x, p.y); return true; }

    float Step() const     { return m_step; }
    uint32_t Dots() const  { return m_dots; }
    uint32_t RingPx() const { return m_ring; }

private:
    /*
 * The label, made once and kept. addTag rather than a member: it has to be a
 * real entity for RasterizeText to reach it by RID and for the font to stay in
 * the type that owns it, and a child of this node is where its lifetime
 * belongs -- deleting the throbber takes it.
 *
 * SetHidden on it immediately, because it is a Drawable2D child of a Drawable2D
 * node and would otherwise also be drawn the ordinary way, on top of the raster
 * it was already rasterised into -- the same run twice, once in this buffer at
 * this node's origin and once in the destination at the screen position.
 */
    void ensureLabel()
    {
        if (m_label) return;
        m_label = this->template addTag<TextLabel>();
        if (!m_label) return;
        m_label->Create(TextLabel::CELL_H);
        m_label->SetHidden(true);
    }

    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    /*
 * ONE PASS, ONE MARK. ClearTo, N discs, one text run, and the Observable mark
 * that FillDisc/FillRect each raise is coalesced by the batch scope -- a
 * compositor above counted 23,746 marks for a 40-point stroke before batching
 * existed (ontology/ObservableBase.h), and this runs every frame forever.
 *
 * Transparent clear, not a background fill: a throbber goes over whatever is
 * already there, and a caller who wants a plate behind it puts this in a pane
 * that has one.
 */
    void repaint()
    {
        if (m_w == 0 || m_h == 0) return;
        etcs_observed_batch _batch(this);

        this->ClearTo(0.0f, 0.0f, 0.0f, 0.0f);

        // Geometry: the dots sit on a circle inscribed in the ring box, pulled
        // in by their own radius so none of them is clipped by the edge.
        const float dot_r_f = static_cast<float>(m_ring) * 0.10f;
        uint32_t dot_r = static_cast<uint32_t>(dot_r_f < 1.0f ? 1.0f : dot_r_f);
        const float orbit = static_cast<float>(m_ring) * 0.5f - static_cast<float>(dot_r);
        const float cx = static_cast<float>(m_w) * 0.5f;
        const float cy = static_cast<float>(m_text_h + m_gap) + static_cast<float>(m_ring) * 0.5f;

        const float TWO_PI = 6.28318530718f;
        for (uint32_t i = 0; i < m_dots; ++i)
        {
            const float at = static_cast<float>(i) / static_cast<float>(m_dots);

            /*
         * Angular distance from the sweep head, the SHORT way round, in turns
         * -- so a dot just behind the head and a dot just ahead of it are both
         * near. Taking the short way is what makes the tail continuous across
         * the wrap at phase 1.0 instead of every dot going dark for one frame.
         */
            float d = m_phase - at;
            d = d - std::floor(d);              // into [0, 1)
            if (d > 0.5f) d = 1.0f - d;         // short way: [0, 0.5]

            // t: 1 at the head, 0 at the end of the tail. Squared so the head
            // is distinctly the head rather than a gentle gradient across
            // half the ring -- the eye reads the peak as the direction.
            float t = 1.0f - (d / TAIL_TURNS);
            if (t < 0.0f) t = 0.0f;
            t = t * t;

            const float a = lerp(0.14f, 1.0f, t);
            const float r = lerp(m_tail[0], m_head[0], t);
            const float g = lerp(m_tail[1], m_head[1], t);
            const float b = lerp(m_tail[2], m_head[2], t);

            const float ang = at * TWO_PI - 1.57079632679f;   // 12 o'clock start
            const int32_t px = static_cast<int32_t>(cx + orbit * std::cos(ang));
            const int32_t py = static_cast<int32_t>(cy + orbit * std::sin(ang));
            this->FillDisc(px, py, dot_r, r, g, b, a);
        }

        // The run, centred over the ring. Through the Glyphs family into THIS
        // raster by RID -- see the header on why the label is a child rather
        // than a sibling.
        if (m_label)
        {
            const TextExtent e = m_label->MeasureTextConcrete(m_text.c_str(), 0, 0);
            const int32_t tx = static_cast<int32_t>(m_w) / 2 - e.width / 2;
            m_label->RasterizeTextConcrete(this->getRID(), m_text.c_str(), 0, 0,
                                           tx, 0,
                                           m_ink[0], m_ink[1], m_ink[2], m_ink[3]);
        }
    }

    TextLabel* m_label = nullptr;
    std::string m_text = "ETCS";

    int32_t  m_x = 0, m_y = 0;
    uint32_t m_w = 0, m_h = 0;
    uint32_t m_ring   = DEFAULT_SIZE;
    uint32_t m_gap    = DEFAULT_SIZE / 8;
    uint32_t m_text_h = 0;

    uint32_t m_dots  = DEFAULT_DOTS;
    float    m_step  = DEFAULT_STEP;   // degrees per FRAME -- AdvanceConcrete
    float    m_phase = 0.0f;

    // The canvas pane's in-band ruler highlight, and a violet shift of it at
    // the same brightness. See the header.
    float m_head[3] = { 0.35f, 0.55f, 0.95f };
    float m_tail[3] = { 0.58f, 0.40f, 0.95f };
    float m_ink[4]  = { 0.94f, 0.89f, 0.78f, 1.0f };
};

#endif
