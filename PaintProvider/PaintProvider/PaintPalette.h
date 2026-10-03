#ifndef PAINTPROVIDER_PAINTPALETTE_H__
#define PAINTPROVIDER_PAINTPALETTE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintTextBar.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintPalette ─────────────────────────────────────────────────────────
 *
 * A TOOLBAR THAT OWNS NO PIXELS.
 *
 * The obvious shape for this is a widget: a rectangle that knows how to draw
 * swatches and how to hit-test them. It is the wrong shape here, and the
 * reason is the one PaintProvider is built on -- this module owns no raster
 * backend and should not grow one for a row of coloured squares. A swatch IS
 * a rectangle in somebody else's 2D tree, and that tree already knows how to
 * draw it, where it is, and whether a point is on it (ontology/Drawable2D.h).
 *
 * So what is left for a palette to be is a MAPPING: this node means that
 * colour, that node means this radius. Nothing about how they look, nothing
 * about where they sit. Which means the toolbar's appearance is a script --
 * PaintProvider/scripts/paint_toolbar.etcs builds it out of RenderProvider
 * shapes -- and rearranging it, restyling it, or replacing it with a colour
 * wheel from a third module changes no C++ at all.
 *
 * Keyed by RID, because that is what a pick hands back and what a script can
 * name. The entries apply to the bound tool, and PaintInput stamps from
 * m_tool->brush() every sample, so a selection takes effect on the next
 * stroke with nothing to propagate.
 */
class PaintPalette : public DeletableBase<PaintPalette>,
                     public AnimatedBase<PaintPalette>
{
public:
    WIRE_TYPE_IDENTITY(PaintPalette);

    PaintPalette() = default;
    bool DeleteConcrete() override { return true; }

    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (!raw) return;
        m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void AddColor(ETCS::RID node, float r, float g, float b, float a)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::Color);
        e.rgba[0] = r; e.rgba[1] = g; e.rgba[2] = b; e.rgba[3] = a;
        e.idle[0] = r; e.idle[1] = g; e.idle[2] = b; e.idle[3] = a;
        m_entries[node] = e;
    }

    void AddSize(ETCS::RID node, float radius)
    {
        if (node == 0 || radius <= 0.0f) return;
        Entry e = entry_of(Kind::Size, radius);
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
    }

    void AddRadiusDelta(ETCS::RID node, float delta)
    {
        if (node == 0 || delta == 0.0f) return;
        Entry e = entry_of(Kind::RadiusDelta, delta);
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
    }

    /*
 * A STEP IN THE TOOL'S OPACITY, in whole percent.
 *
 * This control used to step the motion-coalesce interval, which was a flicker
 * band-aid and is now pinned at every sample
 * (PAINT_MOTION_COALESCE_DEFAULT_MS). The geometry it occupies is the natural
 * home for the setting a painter actually reaches for next to size.
 */
    void AddAlphaDelta(ETCS::RID node, float delta_pct)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::AlphaDelta, delta_pct);
        m_entries[node] = e;
    }

    // The wheel this palette opens. By RID and called by verb name, because
    // PaintColorWheel is declared below this type -- the same hop the wheel makes
    // to reach its router.
    void BindWheel(ETCS::RID wheel) { m_wheel = wheel; }

    /*
 * AN ARROW THAT OPENS THE WHEEL FOR ONE SWATCH.
 *
 * The wheel used to be reached by a single button and replaced whichever colour
 * entry happened to be selected last, which meant the user had to press a swatch
 * and then a separate control, and a mis-remembered selection silently
 * overwrote the wrong one. An arrow that BELONGS to a swatch removes both: the
 * target is named here, at layout time, and cannot be the wrong one.
 *
 * `slot` is a colour entry's node; anything else is refused at press time rather
 * than here, because the script is free to declare the arrow before the swatch.
 */
    /*
 * ── AN ARROW IS AS WIDE AS ITS CELL ──────────────────────────────────────
 *
 * The arrows are authored as three fixed points, and the toolbar's slices are
 * layout boxes that grow. So on a wide window every arrow stayed its authored
 * width and sat against the left edge of a cell several times that size --
 * which reads as a broken layout, and is one: the cell is being driven and the
 * thing inside it is not.
 *
 * REBUILT RATHER THAN SCALED, because a polygon has no transform -- ClearPoints
 * and three AddPoints is the whole of it, and it is exactly the geometry the
 * script would have written if it had known the width.
 *
 * Full width of the cell, shallow: the apex at the middle of the top edge and
 * the base at ARROW_DROP down, which is the shape the toolbar already asks for.
 */
    static constexpr int32_t ARROW_DROP = 12;

    // The cell an arrow lives in is its PARENT -- the script spawns it there
    // (`swatch_ink.spawn(... wheel_arrow_ink)`), so nothing has to be registered
    // for this and a re-laid-out toolbar needs no second binding kept in step.
    static int32_t cell_width_of(ETCS::RID node)
    {
        ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!held) return 0;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(held.get());
        if (!e) return 0;
        ETCS::Entity* parent = e->getParent();
        if (!parent) return 0;
        void* raw = parent->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        if (!raw) return 0;
        const Rect2D r = static_cast<Drawable2D_*>(raw)->Bounds();
        return r.w;
    }

    bool arrowsNeedStretch() const
    {
        for (const auto& [rid, e] : m_entries)
        {
            if (e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow) continue;
            const int32_t w = cell_width_of(rid);
            if (w > 0 && w != e.drawn_w) return true;
        }
        return false;
    }

    void stretchArrows()
    {
        for (auto& [rid, e] : m_entries)
        {
            if (e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow) continue;
            const int32_t w = cell_width_of(rid);
            if (w <= 0 || w == e.drawn_w) continue;

            ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", rid);
            if (!held) continue;
            ETCS::Entity* node = static_cast<ETCS::Entity*>(held.get());
            if (!node) continue;

            const std::string tag = node->getSourceTag().toString();
            ETCS::Buffer none;
            try
            {
                node->call(ETCS::Buffer((tag + ".ClearPoints").c_str()), none,
                           ETCS::RootSignalContext());
                auto pt = [&](int32_t x, int32_t y)
                {
                    ETCS::Buffer p;
                    p.write((std::to_string(x) + ", " + std::to_string(y)).c_str());
                    node->call(ETCS::Buffer((tag + ".AddPoint").c_str()), p,
                               ETCS::RootSignalContext());
                };
                pt(w / 2, 0);
                pt(w, ARROW_DROP);
                pt(0, ARROW_DROP);
            }
            catch (...) { continue; }

            e.drawn_w = w;
            etcs_mark_observed(node);
        }
    }

    void AddWheelArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::WheelArrow);
        e.slot = slot;
        e.drawn_w = 0;      // built on the first Advance, at whatever width it has
        m_entries[node] = e;
    }

    /*
 * AN ARROW THAT STEPS A TOOL'S MODE -- the select tool's, which is the one
 * kind that has one (PaintSelectMode).
 *
 * The same shape as the wheel arrow, for the same reason: a control that
 * BELONGS to a slice names its slice at layout time, so what it changes cannot
 * be whatever happened to be pressed last. What differs is what pressing does.
 * A swatch's arrow opens a picker because a colour is continuous and a popup is
 * the only way to offer all of it; a mode is four words, and a popup for four
 * words is a wheel for a switch. So the arrow steps to the next one and the
 * readout beside it (SetModeReadout) says which -- one press per step, nothing
 * to dismiss.
 *
 * `slot` is a tool entry; anything else is refused at press time rather than
 * here, since the script is free to declare the arrow before the slice.
 */
    void AddModeArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::ModeArrow);
        e.slot = slot;
        e.drawn_w = 0;
        m_entries[node] = e;
    }

    /*
 * THE SAME ARROW, ASKED WHAT IT IS ON.
 *
 * Both of the above are "an arrow drawn across the top of a cell, belonging to
 * that cell". Which of the two it is was stated by the caller picking a verb,
 * and the caller was stating something the palette already knew: a cell that is
 * a COLOUR wants a picker, a cell that is a TOOL wants the next mode. So the
 * slot answers it, and the arrow becomes one thing with one declaration.
 *
 * WHY THAT MATTERS MORE THAN THE TWO LINES IT SAVES. The triangle itself was
 * written out nine times in paint_toolbar.etcs -- three points and a fill, per
 * cell -- and since the arrows learned to STRETCH (stretchArrows) those nine
 * authored copies have to agree with a rule that lives in this file. Nine
 * copies of a shape that something else now rebuilds is the eye's problem
 * again: the drawing is a thing, not a paragraph repeated. One verb that needs
 * no literal is what lets the triangle move into a script of its own
 * (paint_cell_arrow.etcs) and be `run` once per cell, because `run` carries
 * RIDs and not words (resolve_run_bindings).
 *
 * THE ORDER IS NOW LOAD-BEARING, which the two-verb form did not require: the
 * cell must be declared (AddColor or AddTool) before its arrow, because the
 * cell is what the arrow is asking. Every caller already did that -- an arrow
 * is spawned as a child of the cell -- and a cell that has not been declared
 * gets no arrow rather than a guessed one.
 */
    void AddArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        auto it = m_entries.find(slot);
        if (it == m_entries.end()) return;
        if (it->second.kind == Kind::Color) { AddWheelArrow(node, slot); return; }
        if (it->second.kind == Kind::Tool)  { AddModeArrow(node, slot);  return; }
    }

    // A third thing a node can mean, alongside a colour and a size: which TOOL
    // it selects. Same mapping, same Apply, so a tool button is a rectangle in
    // the toolbar script exactly as a swatch is.
    // The tool in hand from outside the bar, lit as if its slice had been
    // pressed -- what a window that belongs to one tool does as it closes
    // (PaintAnimation::Close).
    void PickTool(const std::string& kind)
    {
        if (!m_tool) return;
        m_tool->SetKind(kind);
        repaintPalette();
        ETCS_LOG("PaintPalette", "tool -> " << kind);
    }

    void AddTool(ETCS::RID node, const std::string& kind)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::Tool);
        e.tool = paint_tool_kind_from(kind);
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
        // LIT FROM THE MOMENT IT IS DECLARED, if it is the tool in hand. The
        // bar is built before anything has hovered or pressed it, so without
        // this the slice holding the default tool stays idle until the pointer
        // first crosses the bar -- which reads as "the highlight only appears
        // once you touch something", not as a starting state.
        float c[4]; restingColor(e, c);
        paint_node_fill(node, c[0], c[1], c[2], c[3]);
    }

    /*
 * AND A FOURTH: a node that steps the ZOOM.
 *
 * A view setting rather than a tool setting, which is the one argument against
 * putting it here -- and it loses to the argument for: this type is already
 * "what a picked node means", the +/- region is picked exactly as a swatch is,
 * and a fourth mapping type would duplicate the whole of AddX/Apply to hold one
 * float. The surface it applies to is bound separately for that reason: the
 * palette drives a tool and now also a view, and says so.
 *
 * `factor` multiplies rather than adds, because zoom is perceived
 * multiplicatively -- 1.25 is one notch in at every magnification, while +25%
 * is a big step at 100% and an imperceptible one at 800%.
 */
    void AddZoom(ETCS::RID node, float factor)
    {
        if (node == 0 || factor <= 0.0f) return;
        Entry e = entry_of(Kind::Zoom, factor);
        m_entries[node] = e;
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    /*
 * ── the general entry ────────────────────────────────────────────────────
 *
 * A NODE THAT CALLS A VERB. Every kind above is a verb this type knows how to
 * apply to a tool or a view; this one is any verb on any entity, named the way
 * Entity::call names it ("PaintCanvasMenu.StepWidth") with the argument text a
 * script would have written after it. It is what lets a whole panel be script:
 * the canvas menu's steppers, cells and buttons are rectangles bound here, and
 * the thing they drive never learns what a pointer is.
 *
 * The other kinds are not rewritten onto this one, because they are not calls
 * -- a swatch changes the tool AND records itself as the last colour, a delta
 * repeats while held, a tool change drops the selection. This is the kind for
 * the case with no such second half. The tag is read out of the action, since
 * the RID map is keyed by tag and an action already carries its own.
 */
    void AddCall(ETCS::RID node, ETCS::RID target, const std::string& action, const std::string& args)
    {
        if (node == 0 || target == 0 || action.find('.') == std::string::npos)
        {
            ETCS_LOG("PaintPalette", "AddCall on RID:" << node << " needs a target and a "
                     "'Tag.Action' -- got '" << action << "'.");
            return;
        }
        Entry e = entry_of(Kind::Call);
        e.target = target;
        e.action = action;
        e.args   = args;
        m_entries[node] = e;
    }

    /*
 * A NODE THAT OPENS A PANE, and closes it when it is pressed again -- or when
 * anything else is pressed while it is open, which is what makes it a popup
 * rather than a panel. Open is what it is for the colour wheel (PaintColorWheel::
 * Open): membership of the routing set AND drawn, one fact, so a pane that is
 * merely invisible cannot go on swallowing clicks.
 *
 * The state lives here rather than on the pane's own type, because there is no
 * such type: the pane is a compositor from another module and its contents are
 * a script. One popup at a time, since the second would need to know about the
 * first to dismiss it. The router is needed to open anything, so BindRouter.
 */
    void AddPopup(ETCS::RID node, ETCS::RID pane, ETCS::RID input)
    {
        if (node == 0 || pane == 0 || input == 0) return;
        Entry e = entry_of(Kind::Popup);
        e.slot   = pane;
        e.target = input;
        m_entries[node] = e;
    }

    // The router a popup joins while open. By RID and called by verb name, for
    // the reason PaintColorWheel gives: PaintRouter is declared below this type.
    void BindRouter(ETCS::RID router) { m_router = router; }

    // The same open and close a press on an AddPopup node performs, for a
    // popup nothing was pressed for -- a question the page has to ask, such as
    // what to do with a file that just arrived (PaintCanvasMenu::OfferImport).
    // One popup at a time still holds: opening this closes whatever was open.
    void OpenPopup(ETCS::RID pane, ETCS::RID input)
    {
        if (pane == 0 || input == 0) return;
        if (m_popup_open == pane) return;      // already up; a press would toggle, a request does not
        open_popup(pane, input);
    }
    void ClosePopup() { close_popup(); }

    /*
 * REPLACE A SWATCH'S COLOUR, which is what a colour wheel pick does to the slot
 * it was opened from.
 *
 * The mapping is updated here; the swatch's APPEARANCE is the script's, and is
 * driven by whoever picked -- PaintPalette holds no drawable and cannot restyle
 * one (see this type's header note). Returns false for a node that is not a
 * colour entry, so a caller can tell "there is no such swatch" from "done".
 */
    /*
 * REPLACE A SWATCH'S COLOUR -- the value it applies AND the way it looks.
 *
 * It used to set only the value, on the reasoning that a palette owns no
 * drawables and a script should restyle the swatch from lastSlot(). Nothing ever
 * did, so a picked colour applied to the tool while the swatch went on showing
 * the colour it had replaced -- and the reasoning was already untrue, since
 * Hover paints every entry through this same seam. A control that does not show
 * its own state is the bug, not the layering.
 *
 * `idle` moves with it, or the next hover-out would restore the old colour.
 */
    bool SetColorOf(ETCS::RID node, float r, float g, float b, float a)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end() || it->second.kind != Kind::Color) return false;
        Entry& e = it->second;
        e.rgba[0] = r; e.rgba[1] = g; e.rgba[2] = b; e.rgba[3] = a;
        e.idle[0] = r; e.idle[1] = g; e.idle[2] = b; e.idle[3] = a;
        if (m_hovering != node) paint_node_fill(node, r, g, b, a);
        return true;
    }

    // The most recent colour entry a press landed on, which is the slot a
    // colour wheel replaces when it is dismissed. 0 when the last selection was
    // not a colour.
    ETCS::RID lastColorNode() const { return m_last_color; }

    bool Knows(ETCS::RID node) const { return m_entries.find(node) != m_entries.end(); }

    // True when `node` was one of ours -- which is also the answer to "was
    // this press a tool change rather than a brush stroke", and the only
    // thing the input edge needs from this type.
    // The entry a picked node means: its own, or the nearest ancestor's -- a
    // label drawn on a swatch is picked as the label and means the swatch.
    auto resolve_entry(ETCS::RID node)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end() && node != 0)
        {
            ETCS::Held<Drawable2D_> node_e =
                ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            ETCS::Entity* e = node_e
                ? static_cast<ETCS::Entity*>(node_e.get()) : nullptr;
            for (; e; e = e->getParent())
            {
                it = m_entries.find(e->getRID());
                if (it != m_entries.end()) break;
            }
        }
        return it;
    }

    bool Apply(ETCS::RID node)
    {
        auto it = resolve_entry(node);

        /*
     * WHILE A POPUP IS OPEN, WHERE THE PRESS LANDED DECIDES FIRST. Outside the
     * pane -- the picture, the toolbar, the node that opened it -- it dismisses,
     * and is swallowed for the reason PaintInput swallows the press that closes
     * the wheel: a dismissal that also drew a dab would leave a mark for every
     * popup ever put away. Inside the pane it is the popup's: an entry there
     * applies as any entry does, and a press on the pane's own backing does
     * nothing at all rather than falling through to whatever the pane's input
     * would otherwise make of it.
     */
        if (m_popup_open != 0)
        {
            if (!node_within(node, m_popup_open)) { close_popup(); return true; }
            if (it == m_entries.end()) return true;
        }
        if (it == m_entries.end()) return false;

        const Entry& e = it->second;
        // The two kinds that apply to no tool, ahead of the tool check.
        if (e.kind == Kind::Popup) { open_popup(e.slot, e.target); return true; }
        if (e.kind == Kind::Call)  { call_entry(it->first, e); return true; }

        if (!m_tool)
        {
            ETCS_LOG("PaintPalette", "selection on RID:" << node
                     << " has no tool bound -- nothing to apply it to.");
            return true;   // still ours: it was a palette press, it just went nowhere
        }
        if (e.kind == Kind::Color)
        {
            m_tool->SetColor(e.rgba[0], e.rgba[1], e.rgba[2], e.rgba[3]);
            m_last_color = node;
            ETCS_LOG("PaintPalette", "colour -> " << e.rgba[0] << ", " << e.rgba[1]
                     << ", " << e.rgba[2] << ", " << e.rgba[3]);
        }
        else if (e.kind == Kind::Size)
        {
            m_tool->SetRadius(e.radius);
            ETCS_LOG("PaintPalette", "radius -> " << e.radius);
        }
        else if (e.kind == Kind::RadiusDelta)
        {
            const float next = std::max(1.0f, m_tool->brush().size_px + e.radius);
            m_tool->SetRadius(next);
            paint_node_text(m_radius_readout, std::to_string(static_cast<int>(next + 0.5f)));
            ETCS_LOG("PaintPalette", "radius delta " << e.radius << " -> " << next);
        }
        else if (e.kind == Kind::WheelArrow)
        {
            open_wheel_for(it->first, e.slot);
        }
        else if (e.kind == Kind::ModeArrow)
        {
            step_mode_for(it->first, e.slot);
        }
        else if (e.kind == Kind::AlphaDelta)
        {
            m_tool->AdjustAlphaPercent(static_cast<int32_t>(e.radius));
            paint_node_text(m_alpha_readout, std::to_string(m_tool->alphaPercent()));
            ETCS_LOG("PaintPalette", "alpha -> " << m_tool->alphaPercent() << "%");
        }
        else if (e.kind == Kind::Tool)
        {
            m_tool->SetKind(paint_tool_kind_name(e.tool));
            ETCS_LOG("PaintPalette", "tool -> " << paint_tool_kind_name(e.tool));
            repaintPalette();
            /*
             * THE SELECTION SURVIVES THE TOOL, which is a reversal and the
             * reason for it is the clip. Leaving select used to DROP the
             * region, because "a region left standing under the brush is a
             * trap: it looks like it should mask the stroke and it does not."
             * It does now (PaintLayer::BindClip), so the sentence that argued
             * for dropping it argues for keeping it: a selection is where you
             * may draw, which is a fact about the picture and not about which
             * tool is in hand. The way out of one is a bare click with the
             * select tool, as it always was (end_selection).
             */
        }
        else
        {
            if (!m_surface)
            {
                ETCS_LOG("PaintPalette", "zoom step on RID:" << node
                         << " has no surface bound -- BindSurface first.");
                return true;
            }
            // About the centre of the document's current on-screen box, since a
            // button press carries no meaningful point to zoom about -- unlike a
            // wheel, which zooms about the pointer.
            m_surface->ZoomBy(e.radius, m_surface->panX(), m_surface->panY());
            m_surface->Render();
            ETCS_LOG("PaintPalette", "zoom -> " << m_surface->zoomPercent() << "%");
        }

        return true;
    }


    /*
 * ── click and hold ───────────────────────────────────────────────────────
 *
 * A stepper pressed and held steps again, and again, and there is no clock to
 * step it on: while the button is held still no input arrives. So the palette
 * claims Animated (ontology/Animated.h) and says "I am not finished, come
 * back"; whatever drives that family steps it, and in this session that is the
 * frame edge (Surface::RunFrames).
 *
 * STATED IN MILLISECONDS, which is a correction. This used to be counted in
 * FRAMES -- 22 before the first repeat, then one every 3 -- on the reasoning
 * that the frame was the only clock and a rate in its own units could not drift
 * from it. That is true of the clock and false of the HAND: a stepper the user
 * holds should step twenty times a second on a 30Hz display and on a 144Hz one,
 * and in frames it steps at a third the speed on one and at twice on the other.
 * AnimatedBase hands over a measured interval for exactly this, so the rate is
 * written the way it is meant.
 *
 * THE HOLD ITSELF is a HeldCharge with a large capacity, and it is spent on the
 * same clock rather than per visit. The charge is a count of ACCESSES, so
 * spending one per visit made "how long an unconfirmed press survives" depend
 * on the display's rate and, with two surfaces driving, on how many windows
 * happened to be open. One access per nominal frame's worth of elapsed time
 * keeps the capacity meaning what its callers set it for: a real release ends
 * the hold, a stated release (the mask) ends it, and a pointer that left the
 * page and never came back ends it after the capacity -- the "much higher
 * threshold" a mode wants, against the four a stroke gets.
 */
    // How far the slice in hand is lifted towards white. Half the hover's 0.60,
    // for the reason restingColor gives.
    static constexpr float SELECTED_LIFT = 0.30f;

    static constexpr double REPEAT_DELAY_MS = 360.0;  // before the first repeat
    static constexpr double REPEAT_EVERY_MS = 50.0;   // then twenty a second
    // One HeldCharge access per nominal frame, so a capacity set in "frames"
    // still means the span of time its callers meant. See SetHoldCapacity.
    static constexpr double HOLD_ACCESS_MS  = 16.0;

    // Called after Apply() accepted a press: a delta entry becomes the held one.
    void Hold(ETCS::RID node)
    {
        auto it = resolve_entry(node);
        if (it == m_entries.end()) return;
        const Kind k = it->second.kind;
        if (k != Kind::RadiusDelta && k != Kind::AlphaDelta && k != Kind::Zoom) return;
        m_held = it->first;
        m_held_ms   = 0.0;
        m_repeat_ms = 0.0;
        m_access_ms = 0.0;
        m_hold.Press();
    }
    void Release() { m_held = 0; m_hold.Release(); }
    bool holding() const { return m_held != 0; }

    // Nothing held is the settled state, and it is what this costs then: one
    // call. See ontology/Animated.h on why the question is asked every visit.
    /*
 * TWO REASONS TO TICK. The held repeat is the old one. The new one is that an
 * arrow's CELL may have changed width: the toolbar's slices are Clay boxes that
 * grow with the window, and an arrow is a polygon with three fixed points, so a
 * wide window left every arrow drawn at the width it was authored for and
 * hugging the left edge of a cell three times that size.
 *
 * ASKED ON A TICK RATHER THAN DRIVEN BY THE SOLVE, because the solver writes
 * MoveTo/ResizeTo straight onto the nodes and nothing downstream of it is told.
 * This is the same shape PaintSurface uses for the same class of problem -- "the
 * pane is not the size I last drew for" -- and it costs one bounds read per
 * arrow per frame, only while a mismatch exists.
 */
    bool AnimatingConcrete() override { return m_held != 0 || arrowsNeedStretch(); }

    /*
 * ONE INTERVAL OF HOLDING.
 *
 * The delay and the repeat are two accumulators rather than one elapsed total
 * compared against a formula, because the second one has to survive a step: a
 * single `m_held_ms` tested modulo the period fires twice on a long frame and
 * skips one on a short one, which is the frame-counted bug in a new spelling.
 *
 * AT MOST ONE STEP PER VISIT, deliberately. A stall the cap did not absorb
 * could otherwise owe several repeats at once and deliver them as a jump; a
 * stepper is a HAND-driven control, and the honest answer to "the page was
 * away for a moment" is that the hand got fewer steps, not that it gets them
 * all back in one frame.
 */
    void AdvanceConcrete(double dt_ms) override
    {
        stretchArrows();                 // cheap, and a no-op once they agree
        // Nothing held: this visit was for the arrows. Spending the charge
        // below would "release" a hold that never began and say so in the log
        // on every tick while a cell is still stretching.
        if (m_held == 0) return;
        m_access_ms += dt_ms;
        while (m_access_ms >= HOLD_ACCESS_MS)
        {
            m_access_ms -= HOLD_ACCESS_MS;
            if (m_hold.Held()) continue;
            ETCS_LOG("PaintPalette", "hold lapsed -- released.");
            Release();
            return;
        }

        m_held_ms += dt_ms;
        if (m_held_ms < REPEAT_DELAY_MS) return;
        m_repeat_ms += dt_ms;
        if (m_repeat_ms < REPEAT_EVERY_MS) return;
        m_repeat_ms = 0.0;
        Apply(m_held);
    }

    // How long an unconfirmed hold survives, in nominal frames. See HeldCharge
    // and HOLD_ACCESS_MS -- the unit is the caller's, the clock is not.
    void SetHoldCapacity(uint16_t n) { m_hold.SetCapacity(n); }

    /*
 * RESOLVE FIRST, THEN COMPARE, THEN REPAINT -- the same order PaintLayerPanel's
 * Hover uses, and for a reason measured rather than guessed.
 *
 * This used to compare the RAW node against the last one and repaint before
 * resolving. A node that is not a palette entry at all -- the canvas pane, which
 * is what arrives on every pointer sample while somebody is painting -- misses
 * the early return, rewrites the fill of EVERY entry back to idle, and then
 * leaves without recording itself, so the next sample does it again. Every
 * SetFill marks the tree, so a stroke rewrote seven toolbar swatches per pointer
 * event and made the bar's compositor rebuild ~1,300 glyph rects each time, for
 * a picture that had not changed.
 *
 * What the guard has to compare is the resolved TARGET, because "the pointer is
 * over something that is not mine" and "the pointer is over nothing" are the
 * same fact to a palette, and neither is news twice.
 */
    void Hover(ETCS::RID node)
    {
        ETCS::RID target = 0;
        auto it = m_entries.find(node);
        if (it != m_entries.end()) { target = node; }
        else if (node != 0)
        {
            ETCS::Held<Drawable2D_> node_e =
                ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            ETCS::Entity* e = node_e
                ? static_cast<ETCS::Entity*>(node_e.get()) : nullptr;
            for (; e; e = e->getParent())
            {
                it = m_entries.find(e->getRID());
                if (it != m_entries.end()) { target = e->getRID(); break; }
            }
        }
        if (target == m_hovering) return;
        m_hovering = target;
        // The caption of whatever was hovered goes away with the hover; the new
        // target's, if it has one, appears. See AddHoverLabel.
        show_hover_label(target);
        repaintPalette();
    }


    void SetRadiusReadout(ETCS::RID label) { m_radius_readout = label; }
    void SetAlphaReadout(ETCS::RID label) { m_alpha_readout = label; }

    /*
 * A CAPTION THAT APPEARS ON HOVER. Two bare numbers at the end of the bar were
 * a guess as to which was which, and a permanent caption under each is clutter
 * on a strip that is mostly read at a glance. So the caption is a node the
 * script places and hides, and hovering any entry of the stepper's GROUP -- the
 * +, the -, the number -- reveals it; leaving hides it again. Shown and hidden
 * through SetHidden rather than drawn here, for the reason this class draws
 * nothing: the tree already knows how.
 */
    void AddHoverLabel(ETCS::RID entry, ETCS::RID label)
    {
        if (entry == 0 || label == 0) return;
        m_hover_labels[entry] = label;
        paint_node_hidden(label, true);
    }
    // The label that shows the select tool's mode -- written by the arrow, as
    // the radius readout is written by its +/- (see AddModeArrow).
    void SetModeReadout(ETCS::RID label) { m_mode_readout = label; }
    // The shape slice's, written by its arrow the same way. Bind it with
    // AddHoverLabel as well and it shows only while the slice is hovered.
    void SetShapeReadout(ETCS::RID label)
    {
        m_shape_readout = label;
        if (m_tool) paint_node_text(label, paint_shape_mode_name(m_tool->shape()));
    }
    // The brush slice's, which says which NIB is loaded rather than which
    // outline (PaintTipMode). Seeded on bind like the shape's, so the caption
    // is right before the arrow has ever been pressed.
    void SetTipReadout(ETCS::RID label)
    {
        m_tip_readout = label;
        if (m_tool) paint_node_text(label, paint_tip_mode_name(m_tool->tip()));
    }

void Report() const
    {
        ETCS_LOG("PaintPalette", m_entries.size() << " entries, tool "
                 << (m_tool ? "bound" : "UNBOUND"));
        for (const auto& [rid, e] : m_entries)
        {
            if (e.kind == Kind::Color)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  colour "
                         << e.rgba[0] << ", " << e.rgba[1] << ", " << e.rgba[2]);
            else if (e.kind == Kind::Call)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  calls RID:" << e.target
                         << " " << e.action << "(" << e.args << ")");
            else if (e.kind == Kind::Popup)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  opens pane RID:" << e.slot
                         << (e.slot == m_popup_open ? " (open)" : ""));
            // An arrow said "radius 0" here, which is what the fallthrough
            // prints for anything it has no line for -- and an arrow is now
            // declared by a script that was never told which kind it is
            // (AddArrow), so "which kind did it become" is exactly the
            // question this report has to be able to answer.
            else if (e.kind == Kind::WheelArrow)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  wheel arrow for RID:" << e.slot);
            else if (e.kind == Kind::ModeArrow)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  mode arrow for RID:" << e.slot);
            else if (e.kind == Kind::Tool)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  tool "
                         << paint_tool_kind_name(e.tool));
            else
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  radius " << e.radius);
        }
    }

    /*
 * HOW THIS TYPE REACHES A NODE IT DOES NOT OWN: by verb name over Entity::call,
 * since the node is another module's drawable (the header note says why there
 * is no other way). That seam is one free function now (paint_node_verb, near
 * the top of this file) rather than three statics here and nine more copies
 * elsewhere; the canvas menu, which used to call these by name, calls the same
 * free ones.
 */

    // Whether `node` is `pane` or sits anywhere inside it -- the walk
    // resolve_entry makes, asked a different question.
    static bool node_within(ETCS::RID node, ETCS::RID pane)
    {
        if (node == 0 || pane == 0) return false;
        ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        for (ETCS::Entity* e = h ? static_cast<ETCS::Entity*>(h.get()) : nullptr; e; e = e->getParent())
            if (e->getRID() == pane) return true;
        return false;
    }

    ETCS::RID openPopup() const { return m_popup_open; }

private:
    enum class Kind : uint8_t { Color, Size, Tool, Zoom, RadiusDelta, AlphaDelta, WheelArrow, ModeArrow,
                                Call, Popup };
    /*
 * EVERY MEMBER CARRIES ITS OWN DEFAULT, and the makers below set only what
 * their kind is about. The first four used to be positional -- Entry e{ kind,
 * rgba, radius, tool } -- which named four of nine members and left clang
 * reporting the other five as missing initializers at every one of the ten
 * call sites, on every build. They were never missing; they had defaults.
 * Saying so here is what makes that true by construction instead of by
 * convention.
 */
    struct Entry {
        Kind kind = Kind::Color;
        float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float radius = 0.0f;
        PaintToolKind tool = PaintToolKind::Brush;
        float idle[4] = { 0.16f, 0.16f, 0.20f, 1.0f };
        // The arrows only: the entry this arrow is a control FOR -- a colour
        // for a wheel arrow, a tool for a mode arrow. An arrow belongs to a
        // specific slice, not to "whatever was pressed last". A popup's PANE.
        ETCS::RID slot = 0;
        // A call's target and what to say to it; a popup's input (AddPopup).
        ETCS::RID   target = 0;
        std::string action;
        std::string args;
        // The cell width this node's geometry was last built for -- see
        // stretchArrows. Zero means never, which is also what forces the
        // first build.
        int32_t drawn_w = 0;
    };

    // The two makers every call site goes through, so that adding a member to
    // Entry is one edit here rather than ten at the sites.
    static Entry entry_of(Kind k) { Entry e; e.kind = k; return e; }
    static Entry entry_of(Kind k, float radius) { Entry e; e.kind = k; e.radius = radius; return e; }

    /*
 * WHAT A NODE LOOKS LIKE WITH NOTHING HOVERING IT -- its idle colour, except
 * for the slice whose tool is the one in hand, which stays lit.
 *
 * A bar that lights only what is under the pointer answers "what am I about to
 * press" and never "what am I holding", and between strokes the second is the
 * question you actually have. The lift is half the hover's: findable at a
 * glance, and still leaving the hover somewhere to go.
 */
    void restingColor(const Entry& e, float out[4]) const
    {
        const bool held = (e.kind == Kind::Tool && m_tool && e.tool == m_tool->kind());
        const float t = held ? SELECTED_LIFT : 0.0f;
        for (int i = 0; i < 3; ++i) out[i] = e.idle[i] + (1.0f - e.idle[i]) * t;
        out[3] = e.idle[3];
    }

    /*
 * ── the whole bar's look, in one pass ────────────────────────────────────
 *
 * A slice is one of three things: idle, lit because its tool is the one in
 * hand, or lit further because the pointer is on it. That is ONE answer per
 * node, so it is one write per node -- which is the correction here, and it is
 * not only tidiness.
 *
 * This used to be two passes: reset everything to idle, then tint the hovered
 * group over it. Every pointer move therefore wrote most of the bar twice,
 * once with a value that was already known to be wrong, and the compositor is
 * on another thread. A recompose landing between the two reads the first. The
 * visible failure was a hovered slice that stayed dark while every other slice
 * repainted correctly -- the tint had gone out, and the frame that showed it
 * had already been taken.
 *
 * It also fixes what two passes could not express: a tool change while the
 * pointer is ON the slice. The old refresh skipped the hovered group to avoid
 * dimming it, so the newly held slice was never given its lit resting colour,
 * and since the pointer had not moved there was no hover to restore it either.
 * Selecting a tool left nothing highlighted at all.
 *
 * THE GENERAL ENTRIES ARE NOT RESTYLED. Their look belongs to whoever bound
 * them -- the canvas menu paints its anchor cells to show the chosen one
 * (PaintCanvasMenu::show_anchor) -- and this type never learned an idle colour
 * for them, so writing one back would erase a state it does not know is there.
 * See owns_look, which says the same of the arrows.
 */
    void repaintPalette()
    {
        auto h = (m_hovering != 0) ? m_entries.find(m_hovering) : m_entries.end();
        const bool hovered = (h != m_entries.end());
        // A swatch lifts less than a tool: its idle colour is the colour it
        // MEANS, and washing it toward white says the wrong thing about it.
        const float t = hovered ? ((h->second.kind == Kind::Color) ? 0.40f : 0.60f) : 0.0f;

        for (const auto& [rid, e] : m_entries)
        {
            if (!owns_look(e)) continue;
            float c[4]; restingColor(e, c);
            // OVER THE RESTING COLOUR, not over idle: the slice in hand is
            // already lit, and hovering it has to read as brighter still
            // rather than as the same lift arriving twice.
            if (hovered && (rid == m_hovering || same_hover_group(h->second, e)))
            {
                for (int i = 0; i < 3; ++i) c[i] = c[i] + (1.0f - c[i]) * t;
                c[3] = 1.0f;
            }
            paint_node_fill(rid, c[0], c[1], c[2], c[3]);
        }
    }

    /*
 * Whether this type may restyle the entry at all -- see the note in
 * repaintPalette.
 *
 * AN ARROW IS NOT OURS. AddWheelArrow and AddModeArrow never learned an idle
 * colour, so they carried the default -- 0.16, the slice's own dark -- and
 * every repaint painted the arrow that colour. On the first pointer move over
 * the bar all sixteen arrows vanished into their cells, which looked like they
 * had "started highlighted" and then been turned off. Their look belongs to
 * the script that drew them (SetFill, paint_toolbar.etcs), exactly as a
 * general entry's does.
 */
    static bool owns_look(const Entry& e)
    {
        return e.kind != Kind::Call && e.kind != Kind::Popup
            && e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow;
    }

    static bool same_hover_group(const Entry& a, const Entry& b)
    {
        if (a.kind != b.kind) return false;
        if (a.kind == Kind::Tool) return a.tool == b.tool;
        if (a.kind == Kind::RadiusDelta || a.kind == Kind::AlphaDelta)
            return std::fabs(a.radius) == std::fabs(b.radius);
        if (a.kind == Kind::Size || a.kind == Kind::Zoom)
            return a.radius == b.radius;
        return false;
    }

    /*
 * Open the bound wheel directly above `arrow`, aimed at `slot`.
 *
 * The POSITION is computed here rather than laid out in the script because it is
 * a relation between two things the script places independently -- "above that
 * arrow" stays true when either moves, where a hard-coded pane position does
 * not. Root space, since that is where a floating pane is positioned; see
 * paint_root_origin for why that walk differs from a drawable's own.
 */
    void open_wheel_for(ETCS::RID arrow, ETCS::RID slot)
    {
        if (m_wheel == 0)
        {
            ETCS_LOG("PaintPalette", "wheel arrow on RID:" << arrow
                     << " pressed with no wheel bound -- BindWheel first.");
            return;
        }
        if (slot != 0)
        {
            auto sit = m_entries.find(slot);
            if (sit == m_entries.end() || sit->second.kind != Kind::Color)
            {
                ETCS_LOG("PaintPalette", "wheel arrow on RID:" << arrow
                         << " names RID:" << slot << ", which is not a colour entry.");
                return;
            }
        }
        ETCS::Entity* w = paint_resolve_tag("PaintColorWheel", m_wheel);
        if (!w) return;
        const Point2D at = paint_root_origin(arrow);
        ETCS::Buffer act; act.write("PaintColorWheel.OpenAt");
        ETCS::Buffer arg; arg.write((std::to_string(at.x) + ", " + std::to_string(at.y)
                                   + ", " + std::to_string(slot)).c_str());
        try { w->call(act, arg); } catch (...) {}
    }

    /*
 * Step the stepped thing on the slice `slot` names -- the select tool's mode,
 * the shape tool's outline, or the brush's NIB (PaintTipMode). Three slices
 * with an arrow, one arrow that knows which by asking what it points at.
 *
 * TAKES THE TOOL UP AS WELL. A wheel pick ends with the picked colour in hand,
 * so the swatch's arrow effectively selects that swatch; an arrow that changed
 * the mode of a tool you were not holding would change a readout and nothing
 * you could see on the canvas, which reads as a control that does nothing.
 */
    void step_mode_for(ETCS::RID arrow, ETCS::RID slot)
    {
        auto sit = m_entries.find(slot);
        const bool named = sit != m_entries.end() && sit->second.kind == Kind::Tool;
        const bool is_select = named && sit->second.tool == PaintToolKind::Select;
        const bool is_shape  = named && sit->second.tool == PaintToolKind::Shape;
        const bool is_brush  = named && sit->second.tool == PaintToolKind::Brush;
        if (!is_select && !is_shape && !is_brush)
        {
            ETCS_LOG("PaintPalette", "mode arrow on RID:" << arrow << " names RID:" << slot
                     << ", which is not the select, shape or brush slice.");
            return;
        }
        // Stepping the mode also takes the tool: an arrow pressed is a choice of
        // what to draw next, and asking for a second press to draw it is a
        // choice nobody meant to make.
        const PaintToolKind want = is_select ? PaintToolKind::Select
                                 : is_shape  ? PaintToolKind::Shape
                                             : PaintToolKind::Brush;
        if (m_tool->kind() != want) { m_tool->SetKind(paint_tool_kind_name(want)); repaintPalette(); }
        if (is_select)
        {
            m_tool->CycleMode();
            paint_node_text(m_mode_readout, paint_select_mode_label(m_tool->mode()));
            ETCS_LOG("PaintPalette", "select mode -> " << paint_select_mode_name(m_tool->mode()));
        }
        else if (is_shape)
        {
            m_tool->CycleShape();
            paint_node_text(m_shape_readout, paint_shape_mode_name(m_tool->shape()));
            ETCS_LOG("PaintPalette", "shape -> " << paint_shape_mode_name(m_tool->shape()));
        }
        else
        {
            m_tool->CycleTip();
            paint_node_text(m_tip_readout, paint_tip_mode_name(m_tool->tip()));
            ETCS_LOG("PaintPalette", "tip -> " << paint_tip_mode_name(m_tool->tip()));
        }
    }

    /*
 * The call a general entry makes. The target is resolved NOW, by the tag its
 * action names, rather than held: it belongs to whoever spawned it and may be
 * gone by the time the button is pressed, and a stale pointer is the one
 * failure this mapping must not have.
 */
    void call_entry(ETCS::RID node, const Entry& e)
    {
        const std::string tag = e.action.substr(0, e.action.find('.'));
        ETCS::Entity* t = paint_resolve_tag(tag.c_str(), e.target);
        if (!t)
        {
            ETCS_LOG("PaintPalette", "RID:" << node << " calls " << e.action << " on RID:"
                     << e.target << ", which is not a live " << tag << ".");
            return;
        }
        ETCS::Buffer act; act.write(e.action.c_str());
        ETCS::Buffer arg; arg.write(e.args.c_str());
        try { t->call(act, arg); } catch (...) {}
    }

    /*
 * Open and close are the wheel's (PaintColorWheel::Open / Close): the pane
 * joins the router and is drawn, or leaves it and is hidden, as one change.
 * Hiding is enough: the sheet is rebuilt from its tree by the compose walk, so
 * what the pane was covering comes back with the next frame. The wheel is
 * closed on the way in, because two popups would each have to know about the
 * other to dismiss it.
 */
    void open_popup(ETCS::RID pane, ETCS::RID input)
    {
        if (m_router == 0)
        {
            ETCS_LOG("PaintPalette", "popup pane RID:" << pane
                     << " pressed with no router bound -- BindRouter first.");
            return;
        }
        if (m_popup_open == pane) { close_popup(); return; }
        if (m_popup_open != 0) close_popup();
        if (ETCS::Entity* w = paint_resolve_tag("PaintColorWheel", m_wheel))
        {
            ETCS::Buffer act; act.write("PaintColorWheel.Close");
            ETCS::Buffer none;
            try { w->call(act, none); } catch (...) {}
        }
        if (ETCS::Entity* r = paint_resolve_tag("PaintRouter", m_router))
        {
            ETCS::Buffer act; act.write("PaintRouter.AddPane");
            ETCS::Buffer arg; arg.write((std::to_string(pane) + " " + std::to_string(input)).c_str());
            try { r->call(act, arg); } catch (...) {}
        }
        paint_node_hidden(pane, false);
        m_popup_open = pane;
        ETCS_LOG("PaintPalette", "popup pane RID:" << pane << " opened.");
    }

    void close_popup()
    {
        if (m_popup_open == 0) return;
        if (ETCS::Entity* r = paint_resolve_tag("PaintRouter", m_router))
        {
            ETCS::Buffer act; act.write("PaintRouter.RemovePane");
            ETCS::Buffer arg; arg.write(std::to_string(m_popup_open).c_str());
            try { r->call(act, arg); } catch (...) {}
        }
        paint_node_hidden(m_popup_open, true);
        ETCS_LOG("PaintPalette", "popup pane RID:" << m_popup_open << " closed.");
        m_popup_open = 0;
    }

    // The label for a hovered target: its own, or the one bound to any entry in
    // the same group (a group shares one caption). 0 hides whatever is showing.
    void show_hover_label(ETCS::RID target)
    {
        ETCS::RID want = 0;
        if (target != 0)
        {
            auto tit = m_entries.find(target);
            for (const auto& [entry, label] : m_hover_labels)
            {
                if (entry == target) { want = label; break; }
                auto eit = m_entries.find(entry);
                if (tit != m_entries.end() && eit != m_entries.end()
                    && same_hover_group(tit->second, eit->second)) { want = label; break; }
            }
        }
        if (want == m_hover_label_shown) return;
        if (m_hover_label_shown != 0) paint_node_hidden(m_hover_label_shown, true);
        if (want != 0) paint_node_hidden(want, false);
        m_hover_label_shown = want;
    }

    std::unordered_map<ETCS::RID, Entry> m_entries;
    std::unordered_map<ETCS::RID, ETCS::RID> m_hover_labels;   // entry -> caption
    ETCS::RID m_hover_label_shown = 0;
    // The held stepper, if any -- see Hold/AdvanceConcrete. 600 accesses at one
    // per nominal frame is ten seconds: a hold whose release was lost to the
    // page ends on its own then, whatever the display is actually doing.
    ETCS::RID  m_held = 0;
    double     m_held_ms   = 0.0;   // since the press -- gates the first repeat
    double     m_repeat_ms = 0.0;   // since the last repeat -- gates the rest
    double     m_access_ms = 0.0;   // remainder owed to the hold charge
    HeldCharge m_hold{ 600 };
    PaintTool* m_tool = nullptr;
    PaintSurface* m_surface = nullptr;
    ETCS::RID m_hovering = 0;
    ETCS::RID m_radius_readout = 0;
    ETCS::RID m_alpha_readout = 0;
    ETCS::RID m_mode_readout = 0;
    ETCS::RID m_shape_readout = 0;
    ETCS::RID m_tip_readout = 0;
    ETCS::RID m_wheel = 0;
    // The popup that is open, if one is, and the router it is open IN. See
    // AddPopup: one at a time, and open means routed.
    ETCS::RID m_router = 0;
    ETCS::RID m_popup_open = 0;

    mutable ETCS::RID m_last_color = 0;
};

#endif // PAINTPROVIDER_PAINTPALETTE_H__
