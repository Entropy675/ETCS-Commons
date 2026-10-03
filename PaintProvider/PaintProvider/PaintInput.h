#ifndef PAINTPROVIDER_PAINTINPUT_H__
#define PAINTPROVIDER_PAINTINPUT_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintVisitors.h"   // in order: everything above this in the module is visible here

class PaintInput : public DeletableBase<PaintInput>,
                   public AnimatedBase<PaintInput>
{
public:
    WIRE_TYPE_IDENTITY(PaintInput);

    PaintInput() = default;
    bool DeleteConcrete() override { return true; }

    void BindDocument(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) return;
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
    }

    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (!raw) return;
        m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (!raw) return;
        m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    void SetBrush(float radius, float r, float g, float b, float a)
    {
        if (!m_tool) return;
        m_tool->SetRadius(radius);
        m_tool->SetColor(r, g, b, a);
    }

    /*
 * ── ROUTING ──────────────────────────────────────────────────────────────
 *
 * THE INPUT CHANNEL SPEAKS ONE FRAME AND EVERY TARGET SPEAKS ITS OWN.
 *
 * A window reports a pointer in content-area pixels. That was enough while
 * the canvas WAS the window -- the two frames coincided, so ConsumeInput
 * could take ev.x/ev.y as canvas coordinates and paint. The moment anything
 * else is on screen the coincidence ends, and it ends silently: the toolbar
 * sits at the bottom of the window, a press on it is still "inside the
 * canvas" as far as raw coordinates go, and the editor paints a dot where
 * the user meant to pick a colour.
 *
 * So the event is ROUTED before it is interpreted. Pick the deepest node
 * under the point (Drawable2D_::PickAt, ontology/Drawable2D.h), which walks
 * the same ordered child list the renderer draws with -- so what receives the
 * event is what the user can actually see -- and hands back the point already
 * translated into that node's own space, because the descent computed it on
 * the way down.
 *
 * WHAT ARRIVES IS THEN ORDINARY. A palette node consumes the press as a
 * selection; the canvas gets an InputEvent identical in shape to the one a
 * bare window would have sent, differing only in that its coordinates now
 * mean what the canvas thinks coordinates mean. HandleEvent is untouched --
 * the stroke machine never learns that routing exists, which is what keeps it
 * testable without a window.
 *
 * Unrouted stays a valid configuration: with no root bound this is exactly
 * the old behaviour, which paint_surface.etcs still relies on.
 */
    /*
 * THE POINTER IN THE WINDOW'S FRAME, kept separately from m_cursor_x/y.
 *
 * Those two are the CANVAS's frame -- what the stroke machine integrates
 * against -- and are written with translated coordinates. The press channel
 * needs the untranslated position instead, because it has to be routed before
 * anyone knows which frame it belongs to. Two frames, two variables; sharing
 * one was the bug this design exists to avoid.
 */
    void NoteRoutedCursor(int32_t x, int32_t y) { m_win_x = x; m_win_y = y; }
    int32_t RoutedCursorX() const { return m_win_x; }
    int32_t RoutedCursorY() const { return m_win_y; }

    void BindRoot(ETCS::RID root)     { m_root = root; }
    void BindCanvas(ETCS::RID canvas) { m_canvas = canvas; }

    void BindPalette(ETCS::RID palette)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPalette", palette);
        if (!raw) return;
        m_palette = static_cast<PaintPalette*>(raw->getTrueType());
    }

    // Bound the same way and consulted in the same place as the palette: both
    // answer "was this press a control rather than a stroke", and neither can
    // answer it without knowing where the press landed. See RouteEvent.
    void BindPanel(ETCS::RID panel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayerPanel", panel);
        if (!raw) return;
        m_panel = static_cast<PaintLayerPanel*>(raw->getTrueType());
    }

    // The animation: the tool's drag hands it the region, its window's rows
    // are its, and the outline follows the view (PaintAnimation).
    void BindAnimation(ETCS::RID anim)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintAnimation", anim);
        if (!raw) return;
        m_anim = static_cast<PaintAnimation*>(raw->getTrueType());
    }

    // The page list, for the pane it lives on (the gear menu's): a press on
    // one of its parts is its, the wheel over it scrolls it, and the keys go
    // to it while a name is open. Same seams as the layer panel's.
    void BindPagePanel(ETCS::RID panel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPagePanel", panel);
        if (!raw) return;
        m_page_panel = static_cast<PaintPagePanel*>(raw->getTrueType());
    }

    // The bar over an open text box, placed whenever this input repaints the
    // view (PaintTextBar::Follow).
    /*
     * Undo and redo AS THIS PANE DOES THEM -- the document's step, then the
     * view repainted and a carried selection dropped, which is what ctrl+z
     * does (HandleKey) and what a button has to do too. A button wired to the
     * document's own verb stepped the notebook and left the last frame on
     * screen until something else redrew it.
     */
    bool Undo()
    {
        if (!m_document) return false;
        const bool did = m_document->Undo();
        if (did) { m_sel_carry = false; repaint_view(); }
        return did;
    }
    bool Redo()
    {
        if (!m_document) return false;
        const bool did = m_document->Redo();
        if (did) { m_sel_carry = false; repaint_view(); }
        return did;
    }
    bool RedoAlt()
    {
        if (!m_document) return false;
        const bool did = m_document->RedoAlt();
        if (did) { m_sel_carry = false; repaint_view(); }
        return did;
    }

    // The 'redo alt' control, shown only while the cursor has a second way
    // forward (PaintDocument::RedoAlt): a button for a state the history is
    // in seldom, and one nothing else on the page can say. Its parts are
    // bound one by one -- the button, the word -- and hidden together.
    void BindRedoAlt(ETCS::RID node)
    {
        if (node) m_redo_alt.push_back(node);
        m_redo_alt_shown = -1;           // the next frame places it
    }

    /*
     * ON THE FRAME EDGE, NOT WHERE THE HISTORY CHANGED. Shown or hidden from an
     * input's own thread, the control's change raced the compose walk on the
     * frame thread and was lost: it appeared only when some later input drove
     * another frame through the same panes. Made here (Animated) it is a step
     * of the frame that draws it. What it follows is the document's own
     * published answer (PaintDocument::forked), an atomic, so the frame thread
     * reads nothing the input thread is writing.
     */
    bool AnimatingConcrete() override
    {
        return !m_redo_alt.empty() && m_document && m_document->forked() != m_redo_alt_shown;
    }
    void AdvanceConcrete(double) override
    {
        if (m_redo_alt.empty() || !m_document) return;
        const int want = m_document->forked();
        if (want == m_redo_alt_shown) return;
        m_redo_alt_shown = want;
        for (ETCS::RID n : m_redo_alt) paint_node_hidden(n, !want);
    }

    void BindTextBar(ETCS::RID bar)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTextBar", bar);
        if (!raw) return;
        m_text_bar = static_cast<PaintTextBar*>(raw->getTrueType());
    }

    // The pane is not the size it was when the windows were last placed in it.
    // Zero on either side is "not laid out yet", which is not a resize.
    bool paneResized() const
    {
        if (m_root == 0) return false;
        Rect2D b{ 0, 0, 0, 0 };
        if (!paint_node_bounds(m_root, b) || b.w == 0 || b.h == 0) return false;
        return b.w != m_pane_w || b.h != m_pane_h;
    }

    // The sharing window, for the pane it is: its title drags it and the keys
    // go to it while a name is open (PaintVisitors).
    void BindVisitors(ETCS::RID visitors)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintVisitors", visitors);
        if (!raw) return;
        m_visitors = static_cast<PaintVisitors*>(raw->getTrueType());
    }

    /*
 * ── THE WINDOW YOU TOUCHED LAST IS THE ONE ON TOP ───────────────────────────
 *
 * These windows are siblings in one pane, stacked by Order(), and that order
 * was whatever the boot script typed: layers 15, animation 14, the sharing
 * window 30. So a window could be permanently behind another with no way to
 * bring it forward, and dragging a low one under a high one handed the pointer
 * to the high one mid-drag -- the window was dropped wherever it had got to.
 *
 * A BUMP, NOT A RENUMBER. The focused window is its own authored order plus a
 * constant that clears every order in the pane; everything else keeps exactly
 * the number the script gave it. The relative stacking the page was designed
 * with survives, only one node is ever written, and taking focus away is a
 * subtraction rather than a second pass over a list. Renumbering the set would
 * have made the script's numbers advisory after the first press.
 *
 * The base is read back off the node rather than remembered at bind time, for
 * the reason paint_node_order exists: a script may restack a pane whenever it
 * likes, and a cached base would be a second authority on the same fact.
 */
    static constexpr int32_t FOCUS_BUMP  = 64;  // clears every authored order in the pane
    static constexpr int32_t WINDOW_KEEP = 28;  // title bar left reachable at an edge

    void FocusWindow(ETCS::RID hit)
    {
        if (hit == 0) return;
        ETCS::RID want = 0;
        for (ETCS::RID pane : windows())
            if (pane != 0 && PaintPalette::node_within(hit, pane)) { want = pane; break; }
        if (want == 0 || want == m_focus_pane) return;      // the canvas, or already top

        // The last one goes down FIRST: two windows bumped at once is two
        // windows claiming the top, which is the state this exists to prevent.
        if (m_focus_pane != 0) paint_node_ordered(m_focus_pane, m_focus_base);
        m_focus_pane = want;
        m_focus_base = paint_node_order(want);
        paint_node_ordered(want, m_focus_base + FOCUS_BUMP);
    }

    // Every floating window this input arbitrates. One list, because this is
    // the only object holding all of them -- FocusWindow and ClampWindows are
    // the same question asked about order and about position.
    std::vector<ETCS::RID> windows() const
    {
        std::vector<ETCS::RID> out;
        if (m_panel    && m_panel->window())    out.push_back(m_panel->window());
        if (m_anim     && m_anim->window())     out.push_back(m_anim->window());
        if (m_visitors && m_visitors->window()) out.push_back(m_visitors->window());
        return out;
    }

    /*
 * THE PANE GOT SMALLER AND THE WINDOWS DID NOT MOVE.
 *
 * Read from the pane and compared to the last size seen, which is PaintSurface's
 * own "the pane is not the size I last drew for" shape. Being TOLD by whoever
 * resized would be a rule with an exception in it: the page is not the only
 * thing that can resize this pane.
 */
    void ClampWindows()
    {
        for (ETCS::RID pane : windows()) paint_clamp_into_parent(pane, WINDOW_KEEP);
    }

    /*
 * True while this input is carrying a window by its title, which is what the
 * router holds the pointer on this pane for (PaintRouter::Route).
 *
 * EVERY WINDOW, not just the sharing one. This asked only about the visitors
 * window, so a layer or animation window dragged UNDER another router pane --
 * the sharing window sits at order 30, the sheet at 0 -- lost the pointer the
 * moment it crossed: the router ranked the panes, handed the motion to the one
 * on top, and the drag ended wherever the window happened to be. Raising the
 * focused window (FocusWindow) fixes the case where both windows are children
 * of the same pane; this one fixes the case where they are not, and neither
 * covers the other's.
 */
    bool wantsCapture() const
    {
        return (m_visitors && m_visitors->moving())
            || (m_panel    && m_panel->moving())
            || (m_anim     && m_anim->moving());
    }

    /*
     * IS THE KEYBOARD WANTED HERE: a text box open, or a name field -- a
     * layer's, a page's, the sharing window's -- taking keys. What a page on
     * a phone asks (PaintRouter::Editing) to know when to bring the soft
     * keyboard up, since nothing on the canvas is a field the browser could
     * raise it for.
     */
    bool editingText() const
    {
        if (m_document && m_document->selectedTextBox() != 0) return true;
        if (m_panel && m_panel->editing()) return true;
        if (m_page_panel && m_page_panel->editing()) return true;
        if (m_visitors && m_visitors->editing()) return true;
        return false;
    }

    /*
 * A PRESS LANDED ON ANOTHER PANE. Said by the router to every input it did not
 * give the press to, because a name field open here otherwise hears nothing:
 * the press was never this pane's, the field keeps the keyboard, and typing on
 * the canvas afterwards goes into a name nobody is looking at. Keeping what was
 * typed, as a press elsewhere inside the same pane already does.
 */
    void PressedElsewhere()
    {
        if (m_visitors)   m_visitors->CloseEdit();
        if (m_panel)      m_panel->CloseEdit();
        if (m_page_panel) m_page_panel->CloseEdit();
    }

    /*
 * THE POINTER IS NO LONGER OVER THIS PANE, said by the router rather than
 * inferred here.
 *
 * A pane that does not contain the point is never offered the event (the pass
 * budget only walks panes that do), so leaving cannot be noticed from the
 * inside -- the last thing a pane sees is the motion that was still over it.
 * Anything holding state that only makes sense while the pointer is present
 * gets to drop it here.
 *
 * The stroke's own continuity flag goes with it, for the reason RouteEvent
 * already forgets the cursor on scenery: a stroke that left the pane and came
 * back would otherwise be joined by a straight line across wherever it went.
 */
    // Any leaf claiming Glyphs. Kept as a RID rather than a pointer because it
    // belongs to another module and may be deleted between two clicks.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }

    /*
 * THE WHEEL THIS INPUT ANSWERS FOR.
 *
 * Bound on TWO inputs and meaning different things on each, which is worth
 * saying plainly. On the wheel's own pane it is the thing a pick goes to. On the
 * CANVAS pane it is the thing a press dismisses -- "clicking anywhere on the
 * canvas closes it" is a statement about the canvas, so the canvas is where it
 * is implemented, and a press that closes a wheel is swallowed rather than also
 * starting a stroke.
 */
    void BindWheel(ETCS::RID wheel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintColorWheel", wheel);
        if (raw) m_wheel = static_cast<PaintColorWheel*>(raw->getTrueType());
    }

    // Whether this input's pane IS the wheel (pick) or merely dismisses it.
    void SetWheelPane(bool is_wheel) { m_is_wheel_pane = is_wheel; }

    void RouteLeave()
    {
        m_cursor_seen = false;
        if (m_panel) m_panel->Left();
        if (m_palette) m_palette->Hover(0);
    }

    void RouteEvent(const InputEvent& ev)
    {
        /*
     * THE PANE MAY HAVE CHANGED SIZE SINCE THE LAST EVENT, and if it shrank,
     * a window is now off the end of it (paint_clamp_into_parent).
     *
     * ASKED HERE RATHER THAN ON A TICK. This type claims Animated and its
     * AdvanceConcrete is never called -- verified in the browser: a probe in
     * AnimatingConcrete printed nothing across a boot and two resizes, while
     * the surface's own tick logged both. So a tick is not a mechanism this
     * type has, whatever its bases say, and hanging the windows' reachability
     * on one would have been a fix that never ran. An event is what this type
     * is certain to get.
     *
     * A compare, not a poll: the size is read once per event and the windows
     * are only touched when it differs from the size they were last placed in.
     */
        if (paneResized())
        {
            Rect2D b{ 0, 0, 0, 0 };
            if (paint_node_bounds(m_root, b)) { m_pane_w = b.w; m_pane_h = b.h; }
            ClampWindows();
        }


        /*
     * BEFORE THE PICK, because a scroll has no point to pick with -- its x/y
     * are the delta. The router already decided this pane is the one under the
     * pointer, which is the whole of the routing decision a wheel needs; what
     * is left is a view change on this pane's surface, and that is not a
     * question about which NODE was hit.
     */
        if (ev.action == INPUT_SCROLL)
        {
            // Over the layer window the notch is the window's: it scrolls the
            // rows, one per notch, up toward the top of the stack. Anywhere
            // else on the pane it is the zoom (HandleEvent). The position is
            // the last one routed, translated into the pane's space the same
            // way a picked event's is below.
            if ((m_panel || m_page_panel || m_anim) && m_root != 0)
            {
                const Point2D pane_at = paint_root_origin(m_root);
                const Point2D at{ RoutedCursorX() - pane_at.x, RoutedCursorY() - pane_at.y };
                if (m_panel && m_panel->Contains(at))
                {
                    m_panel->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
                if (m_page_panel && m_page_panel->Contains(at))
                {
                    m_page_panel->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
                if (m_anim && m_anim->Contains(at))
                {
                    m_anim->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
            }
            HandleEvent(ev);
            return;
        }

        if (m_root == 0) { HandleEvent(ev); return; }   // unrouted: the old path

        // A window carried by its title follows the pointer wherever it is --
        // over its own pane or not, which is why this is ahead of the pick --
        // until the release puts it down.
        if (m_visitors && m_visitors->moving())
        {
            if (ev.action == INPUT_MOTION) m_visitors->DragWindow(Point2D{ ev.x, ev.y });
            else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP) m_visitors->EndWindowDrag();
            return;
        }

        /*
     * INTO THE PANE'S OWN SPACE FIRST. A routed event carries a point in ROOT
     * space, and PickAt takes one in the space of the node it is called on --
     * the same point only when that node sits at the origin.
     *
     * Every pane that worked did sit at the origin (sheet_root is 0,0), so the
     * distinction cost nothing and stayed invisible until a pane that moves
     * needed it: the colour wheel's popup picked against unshifted coordinates
     * and answered for whatever happened to be under the wrong point.
     */
        const Point2D pane_at = paint_root_origin(m_root);
        const Point2D pane_pt{ ev.x - pane_at.x, ev.y - pane_at.y };

        /*
     * HELD FOR THE WALK, AND ONLY THE WALK. PickAt descends somebody else's
     * tree, so the answer has to stay true for the whole descent rather than
     * for the instant it was given (core/Entity.h) -- and then the hold is
     * let go before anything is done about the answer. What follows acts by
     * RID (the palette, the panel, the wheel all resolve their own nodes), and
     * an action may make an entity: the import prompt's answer spawns a layer,
     * and AddTag from inside a hold is refused by the core outright, since the
     * ordering thread that has to see it may be inside a Delete waiting on
     * this very hold.
     */
        ETCS::RID hit_rid = 0;
        Point2D   hit_local{ 0, 0 };
        {
            ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_root);
            if (!root)
            {
                ETCS_LOG("PaintInput", "routing root RID:" << m_root
                         << " is gone or going -- dropping the event.");
                return;
            }
            const Pick2D hit = root->PickAt(pane_pt);
            if (!hit) return;                       // outside the tree entirely
            hit_rid   = hit.node->getRID();
            hit_local = hit.local;
        }

        /*
     * A PRESS ON THE PALETTE IS NOT A STROKE, and saying so HERE rather than
     * in the palette is deliberate: what a press means is a property of where
     * it landed, and this is the only place that knows both.
     */
        const bool is_press   = (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN);
        const bool is_release  = (ev.action == INPUT_UP   || ev.action == INPUT_BUTTON_UP);

        // The window last pressed is the window on top. Decided before anything
        // acts on the press, so the raise is already in place for the drag that
        // usually follows it.
        if (is_press) FocusWindow(hit_rid);

        /*
     * A BUTTON ANYWHERE BUT THE PANEL CLOSES THE NAME FIELD.
     *
     * While the field is open it takes EVERY key ahead of the chords and the
     * text boxes both (KeyDown, below) -- which is the point, and which without
     * this leaves Enter and Escape as the only ways out: a press on the canvas,
     * the toolbar, the wheel or another window leaves the field open and
     * invisible from there, and ctrl+z is a z. The panel's own press path ends
     * it for a row body, an eye and a delete; this is everywhere else.
     *
     * HERE, because this is the only place that knows both that a button was
     * pressed and what it landed on -- the same reason the palette's
     * not-a-stroke rule is stated here rather than in the palette.
     *
     * AHEAD OF the palette and the wheel, which return early: a press they
     * consume is still a press somewhere that is not the field.
     *
     * NOT ON MOTION, and that is the one liberty taken with "any mouse event":
     * a pointer that drifts one pixel while somebody types is not a gesture,
     * and closing on it would make the field unusable rather than unsticky.
     *
     * m_on_panel is what keeps the release of an in-panel press from counting:
     * press the label, let go having drifted off the row, and the field opened
     * and shut in one gesture without it.
     */
        if ((is_press || is_release) && m_panel && !m_on_panel
            && m_panel->editing() && !m_panel->owns(hit_rid))
            m_panel->CloseEdit();
        if ((is_press || is_release) && m_page_panel
            && m_page_panel->editing() && !m_page_panel->owns(hit_rid))
            m_page_panel->CloseEdit();
        // The sharing window's title: the press that starts carrying it. In the
        // router's space, which is its parent's (the sheet sits at the origin).
        if (is_press && m_visitors && m_visitors->PressView(hit_rid))
            return;
        if (is_press && m_visitors && m_visitors->PressTitle(hit_rid, Point2D{ ev.x, ev.y }))
            return;
        if (is_press && m_visitors && m_visitors->editing()) m_visitors->CloseEdit();

        // THE PAGE LIST, ahead of the palette: its rows are on the same pane as
        // the menu's buttons and a press on one is the list's, the release
        // swallowed with it so the sheet under the menu never sees half a click.
        if ((is_press || is_release) && m_page_panel && m_page_panel->Apply(hit_rid, is_press))
            return;
        // The animation window carried by its bar, as the layer window is.
        if (m_anim && m_anim->moving())
        {
            if (ev.action == INPUT_MOTION) { m_anim->DragWindow(pane_pt); return; }
            if (is_release) { m_anim->EndWindowDrag(); return; }
        }
        if (is_press && m_anim && m_anim->PressView(hit_rid))
            return;
        if (is_press && m_anim && m_anim->PressTitle(hit_rid, pane_pt))
            return;
        if ((is_press || is_release) && m_anim && m_anim->Apply(hit_rid, is_press))
            return;

        if (m_palette && ev.action == INPUT_MOTION)
            m_palette->Hover(hit_rid);

        if (m_palette && is_press && m_palette->Apply(hit_rid))
        {
            m_on_palette = true;
            m_palette->Hold(hit_rid);           // a stepper keeps stepping until let go
            return;
        }
        if (is_release && m_on_palette)
        {
            m_on_palette = false;               // the press that ended was a selection
            m_palette->Release();
            return;
        }

        /*
     * THE LAYER PANEL, consulted exactly where the palette is and for the same
     * reason -- what a press means is a property of where it landed, and this is
     * the only place that knows both.
     *
     * Three edges rather than the palette's two, because a panel row is
     * draggable: a press selects or toggles, a release over a row is a DROP that
     * restacks, and motion is a hover. The hover goes through unconditionally so
     * that moving OFF the panel clears the isolation -- PaintLayerPanel::Hover
     * reads a node it does not own as "nobody", which is what leaving means
     * without an exit event to carry it.
     */
        /*
     * THE WHEEL, ahead of the panel and the palette because it is ON TOP of
     * them: while it is open it is the topmost pane, and a press that reaches
     * any other pane at all is a press outside it.
     */
        if (m_wheel && is_press)
        {
            if (m_is_wheel_pane)
            {
                /*
             * IN THE PANE'S SPACE, not the hit node's. A wheel measures a pick
             * from the centre and radius it was given (PaintColorWheel::Create),
             * both stated in the pane's coordinates -- and hit.local is local to
             * whatever was actually struck, which for a drawn wheel is one of
             * twelve wedges. Measured from a wedge's own origin, every point in
             * the disc reads as far outside it, so the pick always missed and the
             * press only ever dismissed. The wheel is drawn from the pane's
             * coordinates, so it has to be picked in them.
             */
                /*
             * THREE ANSWERS, NOT TWO. A press on the value strip changes the
             * disc's brightness and must leave the popup open -- closing on it
             * would make the third dimension of the colour unreachable in
             * practice, since choosing a shade takes two presses.
             */
                const PaintPick r = m_wheel->Pick(pane_pt.x, pane_pt.y);
                if (r != PaintPick::Adjusted) m_wheel->Close();
                return;
            }
            if (m_wheel->open())
            {
                // A press anywhere else dismisses and does NOT also draw --
                // otherwise every dismissal leaves a dab where the user was
                // only trying to put the popup away.
                m_wheel->Close();
                return;
            }
        }

        // The list follows the document, not the other way round: an import,
        // a page switch or a resize changes the stack with no press on a
        // row, so the next event that reaches here re-reads it. A revision
        // compare rather than a refresh per event -- Refresh rewrites six
        // fills and three labels, and a pointer sends events by the hundred.
        if (m_document && m_document->revision() != m_panel_rev)
        {
            m_panel_rev = m_document->revision();
            if (m_panel) m_panel->Refresh();
        }
        if (m_panel)
        {
            // A window being dragged by its title takes every motion until the
            // release, ahead of the hover: the pointer is over whatever the
            // window is passing, not choosing a row. The move alone is the
            // change; the compose walk redraws what the window was covering.
            if (ev.action == INPUT_MOTION && m_panel->DragWindow(pane_pt)) return;
            if (is_release && m_panel->EndWindowDrag()) { m_on_panel = false; return; }
            // A row being carried takes the motion ahead of the hover, so the
            // eyes it passes over do not isolate their layers on the way. A
            // motion that says the button is up ends a drag whose release some
            // other pane took.
            if (ev.action == INPUT_MOTION && m_panel->draggingRow()
                && input_button_state(ev, PAINT_BUTTON_LEFT) == 0)
                m_panel->CancelRowDrag();
            if (ev.action == INPUT_MOTION && m_panel->DragRow(pane_pt)) return;
            if (ev.action == INPUT_MOTION) m_panel->Hover(hit_rid);
            if (is_release && m_panel->Drop(pane_pt)) { m_on_panel = false; return; }
            if (is_press && m_panel->Apply(hit_rid, true, pane_pt)) { m_on_panel = true; return; }
            if (is_release && m_on_panel) { m_on_panel = false; return; }
        }

        /*
     * Anything that is not the canvas is scenery -- drawable over the picture
     * without becoming part of it. Forgetting the cursor on the way out
     * matters: a stroke that began on the canvas, left over the toolbar and
     * came back would otherwise be joined by a straight line across the gap.
     */
        if (m_canvas != 0 && hit_rid != m_canvas)
        {
            if (ev.action == INPUT_MOTION) m_cursor_seen = false;
            return;
        }

        InputEvent local = ev;
        local.x = static_cast<int16_t>(hit_local.x);
        local.y = static_cast<int16_t>(hit_local.y);
        HandleEvent(local);
    }

    // Drive the tool's stroke state machine from ONE input event, commit the
    // sample to the active layer, and stamp live feedback onto the bound view
    // surface. One event rather than a buffer of them, because the stream hands
    // them over one at a time (ConsumeInput below).
    //
    // Public because it is the seam a test can reach: the stream path needs a
    // live producer, and the state machine is worth asserting on without one.
    /*
 * ── view space and document space ────────────────────────────────────────
 *
 * Everything arriving here is in VIEW pixels -- that is what a window reports
 * and what PickAt translated into the pane's frame. Everything that MARKS is in
 * DOCUMENT pixels, because that is where the layers are. The projection between
 * them belongs to the surface (PaintSurface), so these two are the only place
 * the conversion happens and every marking path below is document-space by the
 * time it runs.
 *
 * Unprojected when there is no surface bound, which is the identity and is what
 * the headless tests and paint_surface.etcs both rely on.
 */
    int32_t to_doc_x(int32_t vx) const { return m_surface ? m_surface->ViewToDocX(vx) : vx; }
    int32_t to_doc_y(int32_t vy) const { return m_surface ? m_surface->ViewToDocY(vy) : vy; }

    /*
 * ── IS THE GESTURE STILL HELD ────────────────────────────────────────────
 *
 * Asked once per routed event while a button is believed down, and this is the
 * ACCESS that HeldCharge is built around (ontology/InputSource.h): asking costs a
 * unit, and the platform confirming the button tops it back up. When the charge
 * runs out, the release the platform never delivered is made HERE and sent down
 * the same path a real one takes -- flush, end, drop -- so nothing after this
 * point knows a release was ever missing.
 *
 * THREE ANSWERS FROM THE EVENT. A stated mask that names the button confirms. A
 * stated mask that does NOT is the release itself, and ends the hold at once. A
 * source that could not say (INPUT_BUTTONS_STATED clear) is where the charge
 * earns its keep: motion while believed held IS the drag and confirms, and only
 * the events that carry no evidence either way -- keys, a wheel notch -- spend
 * it. So a platform with no button report keeps the old behaviour almost
 * exactly, and one that reports gets the truth without a delay.
 */
    bool still_held(HeldCharge& charge, uint16_t button, const InputEvent& ev)
    {
        switch (input_button_state(ev, button))
        {
            case 1:  charge.Confirm(); return true;
            // A platform that REPORTED the button up is not evidence to weigh,
            // it is the release -- weighing it spent the charge one sample at a
            // time and each of those samples was a dab of phantom ink. The
            // charge is for the absence of information, not for contradicting it.
            case 0:  charge.Release(); return false;
            default:
                if (ev.action == INPUT_MOTION) { charge.Confirm(); return true; }
                return charge.Held();
        }
    }

    // A carry is a change from its lift, not its drop -- the lift is what
    // clears the source -- so the snapshot goes before it.
    bool lift_for_carry()
    {
        if (!m_document) return false;
        // Already floating -- a paste, or a carry a lost release left in the
        // air -- is carried as it is: the lift happened, and so did the
        // snapshot that went before it.
        if (m_document->selection().lifted()) return true;
        m_document->Remember();
        return m_document->LiftSelection();
    }

    // The release that should have arrived. Built from the last routed position
    // rather than the triggering event's, because the triggering event may be a
    // key or a scroll and carry no position at all.
    void release_lapsed(uint16_t button)
    {
        InputEvent up{};
        up.key     = button;
        up.action  = INPUT_BUTTON_UP;
        up.buttons = INPUT_BUTTONS_STATED;   // and nothing held: that is the point
        up.x = static_cast<int16_t>(m_pending_view_x);
        up.y = static_cast<int16_t>(m_pending_view_y);
        ETCS_LOG("PaintInput", "button " << button << " lapsed with no release -- releasing it.");
        HandleEvent(up);
    }

    // How many unconfirmed accesses a held button survives. Low here because a
    // phantom drag costs a line across the picture; something that reads a hold
    // as a MODE raises it. See HeldCharge.
    void SetHoldCapacity(uint16_t n) { m_pan_hold.SetCapacity(n); m_stroke_hold.SetCapacity(n); }

    void BindPages(ETCS::RID pages)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPages", pages);
        if (!raw) return;
        m_pages = static_cast<PaintPages*>(raw->getTrueType());
    }


    void HandleEvent(const InputEvent& ev)
    {
        /*
     * ── lapsed holds first ────────────────────────────────────────────────
     *
     * Before any gesture branch reads the state it holds, that state is re-judged
     * against this event. The button's OWN press and release are exempt: they are
     * the statements, not questions about them.
     */
        const bool own_button_edge =
            (ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_BUTTON_UP);
        if (m_panning && !(own_button_edge && ev.key == m_pan_button)
            && !still_held(m_pan_hold, m_pan_button, ev))
            release_lapsed(m_pan_button);
        const bool primary_live = (m_tool && m_tool->active()) || m_text_drag != 0 || m_text_resize != 0 || m_sel_carry
                                || m_anim_drag;
        if (primary_live && !(own_button_edge && ev.key == m_stroke_button)
            && !still_held(m_stroke_hold, m_stroke_button, ev))
            release_lapsed(m_stroke_button);

        /*
     * ── the right button ─────────────────────────────────────────────────
     *
     * It does not draw. It CLEARS whatever gesture is in flight and then pans,
     * which are two halves of one idea: the right button is how you get out of
     * something and move somewhere else, and neither of those should leave a
     * mark. An anchored shape abandoned this way is abandoned completely --
     * CancelStroke rather than EndStroke, so nothing is committed.
     *
     * Handled before anything else because it must not fall through: reaching
     * the ordinary paths with a right button held is how a pan ends up drawing
     * a line across the picture.
     */
        // Pan: right (GLFW 1) or middle (GLFW 2). Middle is the natural pan
        // button in the browser -- right opens the context menu there.
        if ((ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_BUTTON_UP)
            && paint_is_pan_button(ev.key))
        {
            if (ev.action == INPUT_BUTTON_DOWN)
            {
                m_panning  = true;
                m_pan_button = ev.key;
                m_pan_hold.Press();
                m_pan_from_x = ev.x;
                m_pan_from_y = ev.y;
                m_last_motion_ms = 0;    // allow an immediate first pan sample
                if (m_tool && m_tool->active()) m_tool->CancelStroke();
                repaint_view();          // drop any preview the cancelled gesture left
            }
            else if (ev.key == m_pan_button || m_pan_button == 0)
            {
                flush_coalesced_motion(); // apply the last pending pan delta
                m_panning = false;
                m_pan_button = 0;
                m_pan_hold.Release();
            }
            return;
        }
        if (ev.action == INPUT_MOTION && m_panning)
        {
            // Accumulate view-space samples; rebuild the sheet on the coalesce
            // interval only (see PAINT_MOTION_COALESCE_DEFAULT_MS).
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;
            m_motion_pending = true;
            m_motion_kind = MotionKind::Pan;
            if (motion_coalesce_due())
                flush_coalesced_motion();
            return;
        }

        /*
     * ── the wheel ────────────────────────────────────────────────────────
     *
     * ZOOM ABOUT THE POINTER, which is the only thing a wheel can sensibly mean
     * over a projected document -- zooming about anything else walks what you
     * were looking at off the edge.
     *
     * The notch arrives on the pointer ring with its delta in x/y
     * (ontology/InputSource.h::INPUT_SCROLL), and the position to hold fixed is
     * the last one routed -- a wheel carries no position of its own, for the
     * same reason a button press used to carry none.
     *
     * IN VIEW SPACE, unconverted: ZoomAt solves the pan so that the document
     * point currently under that view point stays under it, so handing it a
     * document coordinate would hold the wrong thing still.
     */
        if (ev.action == INPUT_SCROLL)
        {
            if (!m_surface) return;
            const float factor = (ev.y > 0 || ev.x > 0) ? 1.25f : 0.8f;
            m_surface->ZoomBy(factor, RoutedCursorX(), RoutedCursorY());
            m_surface->Render();
            ETCS_LOG("PaintInput", "wheel -> " << m_surface->zoomPercent() << "%"
                     << " about " << RoutedCursorX() << "," << RoutedCursorY());
            return;
        }

        // The document draws box outlines only while the text tool is held, and
        // nothing tells it when the tool changes -- the palette sets the kind
        // straight on the tool. So it is reconciled here, where every event
        // passes, rather than by a notification that does not exist.
        sync_text_affordance();

        if (ev.action == INPUT_MOTION && m_text_resize != 0 && m_document)
        {
            // Its corner, following the pointer. The width is what the lines
            // wrap to, so the text reflows as it goes.
            PaintTextBox* b = m_document->FindTextBox(m_text_resize);
            if (!b) { m_text_resize = 0; return; }
            b->w = std::max(8, to_doc_x(ev.x) - b->x);
            b->h = std::max(8, to_doc_y(ev.y) - b->y);
            repaint_view();
            return;
        }

        if (ev.action == INPUT_MOTION && m_text_drag != 0 && m_document)
        {
            // Carrying a box. Not a stroke, not a preview, and deliberately not
            // coalesced: it is one field assignment and a re-composite, which is
            // what every other tool's sample already costs.
            PaintTextBox* b = m_document->FindTextBox(m_text_drag);
            if (!b) { m_text_drag = 0; return; }
            b->x = to_doc_x(ev.x) - m_text_grab_x;
            b->y = to_doc_y(ev.y) - m_text_grab_y;
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            repaint_view();
            return;
        }

        if (ev.action == INPUT_MOTION && m_anim_drag && m_anim)
        {
            // Carrying the animation camera: the region follows the hand by
            // the offset it was taken at, as a text box does.
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_anim->MoveRegionTo(m_cursor_x - m_anim_grab_x, m_cursor_y - m_anim_grab_y);
            return;
        }

        if (ev.action == INPUT_MOTION && m_sel_carry)
        {
            /*
         * Carrying a selection. Not a stroke -- the tool holds no anchor --
         * but COALESCED where the text box's carry is not: every sample here
         * re-composites the document AND resamples the lifted pixels over it,
         * which is the full-view rebuild the anchored kinds are throttled for
         * (paint_tool_default_coalesce_ms), and the select tool is one of them.
         */
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;
            m_motion_pending = true;
            m_motion_kind = MotionKind::Carry;
            if (motion_coalesce_due())
                flush_coalesced_motion();
            return;
        }

        if (ev.action == INPUT_MOTION)
        {
            // Position is still taken from the event (not integrated). Only the
            // expensive follow-up -- preview rebuild / segment stamp -- is
            // coalesced so a high-rate pointer does not clear+composite every
            // sample (see PAINT_MOTION_COALESCE_DEFAULT_MS).
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_cursor_seen = true;
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;

            if (m_tool && m_tool->active())
            {
                m_tool->MoveStroke(m_cursor_x, m_cursor_y);
                m_motion_pending = true;
                m_motion_kind = MotionKind::Stroke;
                if (motion_coalesce_due())
                    flush_coalesced_motion();
            }
        }
        else if (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN)
        {
            /*
         * NOTHING STARTS ON A VIEW-ONLY PAGE (PaintDocument::SetReadOnly). The
         * verbs refuse too, but a stroke is drawn as it goes and committed at
         * the end, so refusing here is what keeps the pointer from leaving ink
         * the room never sees. Pan and zoom were handled above and still work.
         */
            if (m_document && m_document->readOnly())
            {
                ETCS_LOG("PaintInput", "view only -- ask the host for drawing.");
                return;
            }
            // Nor while the room puts the picture right (PaintDocument::Syncing).
            if (m_document && m_document->syncing())
            {
                ETCS_LOG("PaintInput", "a moment -- this page is catching up with the room.");
                return;
            }
            /*
         * A BUTTON BRINGS ITS OWN POSITION (ontology/InputSource.h), so it
         * does not have to wait for one to have arrived -- and should not,
         * since a click on a fresh window is a perfectly ordinary first
         * event. A key press still does, because a key knows nothing about
         * where the pointer is.
         */
            if (ev.action == INPUT_BUTTON_DOWN)
            {
                m_cursor_x = to_doc_x(ev.x);
                m_cursor_y = to_doc_y(ev.y);
                m_cursor_seen = true;
                // ONE CHARGE FOR WHATEVER THIS PRESS STARTS -- a stroke, a text
                // box carry, a selection carry. They are different gestures with
                // one thing in common: the same release ends all of them, so the
                // same lapse must too.
                m_stroke_button = ev.key;
                m_stroke_hold.Press();
            }
            /*
         * A PRESS INSIDE AN EXISTING TEXT BOX SELECTS IT, and starts no gesture.
         *
         * Without this the only thing the text tool could do is make new boxes,
         * and every box already placed would be permanently out of reach -- which
         * is what "previously made text boxes should be selectable" rules out.
         * Checked before BeginStroke because a selection is not a stroke and must
         * not leave the tool holding an anchor it will later commit.
         */
            if (m_tool && m_cursor_seen && m_document
             && m_tool->kind() == PaintToolKind::Glyph)
            {
                const uint32_t hit_box = m_document->TextBoxAt(m_cursor_x, m_cursor_y);
                /*
             * THE OPEN BOX'S CORNER RESIZES IT. Measured in view pixels, like the
             * handle it is drawn as, so it is the same target at any zoom.
             */
                if (hit_box != 0 && hit_box == m_document->selectedTextBox() && m_surface)
                    if (const PaintTextBox* b = m_document->FindTextBox(hit_box))
                    {
                        const int32_t cx = m_surface->DocToViewX(b->x + b->w), cy = m_surface->DocToViewY(b->y + b->h);
                        const int32_t vx = m_surface->DocToViewX(m_cursor_x), vy = m_surface->DocToViewY(m_cursor_y);
                        if (vx >= cx - PaintDocument::TEXT_HANDLE_PX - 2 && vy >= cy - PaintDocument::TEXT_HANDLE_PX - 2)
                        {
                            m_text_resize = hit_box;
                            repaint_view();
                            return;
                        }
                    }
                if (hit_box != 0)
                {
                    m_document->SelectTextBox(hit_box);
                    /*
                 * AND IT BECOMES DRAGGABLE FROM HERE. A press inside a box is
                 * ambiguous until the pointer either moves or does not, so it
                 * arms a move rather than choosing between the two: hold still
                 * and it was a selection, drag and the box follows. The grab
                 * OFFSET is what makes it follow rather than jump -- moving the
                 * box's corner to the pointer would teleport it by however far
                 * in you happened to press.
                 */
                    if (const PaintTextBox* b = m_document->FindTextBox(hit_box))
                    {
                        m_text_drag   = hit_box;
                        m_text_grab_x = m_cursor_x - b->x;
                        m_text_grab_y = m_cursor_y - b->y;
                    }
                    ETCS_LOG("PaintInput", "text box " << hit_box << " selected for typing");
                    repaint_view();
                    return;
                }
            }

            // A carry whose release landed on scenery never reached here
            // (RouteEvent drops those before HandleEvent) and is still in the
            // air. This press ends it where it hovers, so the pixels are back
            // in a layer before anything else is decided about them.
            if (m_sel_carry)
            {
                m_sel_carry = false;
                if (m_document) m_document->DropSelection();
            }

            /*
         * A PRESS INSIDE THE SELECTION PICKS IT UP, and starts no gesture --
         * the same shape as the press inside a text box above, and for the
         * same reason: what a press means depends on what is under it, and a
         * region you can see is a thing you expect to be able to take hold
         * of. The pixels leave the layer NOW (PaintDocument::LiftSelection),
         * so the hole is visible from the first sample of the drag rather
         * than appearing at the release. The grab offset is what makes the
         * region follow the hand instead of snapping its corner to it.
         *
         * A press OUTSIDE it falls through and begins a new region, which
         * replaces the old one at the first sample.
         *
         * UNLESS A MODIFIER IS HELD, and that exception is what makes add and
         * subtract usable at all. ctrl and shift say "change this region"
         * (PaintSelectOp), and the region being changed is the one you are
         * standing on -- a subtract starts inside what it takes away from
         * almost by definition. Without this the gesture is swallowed whole:
         * the press picks the region up, the drag carries it, and the release
         * drops it somewhere new, which is the one thing that was not asked
         * for. It is also why a subtract appeared to do nothing rather than to
         * do something wrong -- a carry logs no selection at all.
         */
            /*
         * THE MOVE TOOL IS THE PAN, REACHED BY THE LEFT BUTTON. Nothing new
         * under it: it enters the same m_panning state a right or middle drag
         * does and the same motion handler moves the picture.
         *
         * IT EXISTS FOR TOUCH. Pan has always been the right or middle button
         * (paint_is_pan_button), and a phone has neither -- so on a tablet the
         * picture could not be moved at all. A tool that pans on an ordinary
         * drag is the one shape that works with a single contact point.
         *
         * The raw event position, not m_cursor_*: a pan is measured in VIEW
         * pixels, and the cursor has already been projected into the document.
         * Panning by a document delta would move the picture by more or less
         * than the finger, depending on the zoom.
         */
            if (m_tool && m_tool->kind() == PaintToolKind::Move)
            {
                m_panning    = true;
                m_pan_button = 0;               // the left button has no pan id
                m_pan_hold.Press();
                m_pan_from_x = ev.x;
                m_pan_from_y = ev.y;
                m_last_motion_ms = 0;           // allow an immediate first sample
                return;
            }

            /*
         * A PRESS INSIDE THE ANIMATION CAMERA CARRIES IT, as a press inside a
         * selection carries that: the camera is a thing on the page you can
         * take hold of and put somewhere else, then snap again. Outside it the
         * press falls through and a drag chooses a new one.
         */
            if (m_tool && m_cursor_seen && m_anim
             && m_tool->kind() == PaintToolKind::Animate
             && m_anim->RegionHas(m_cursor_x, m_cursor_y))
            {
                m_anim_drag   = true;
                m_anim_grab_x = m_cursor_x - m_anim->regionX();
                m_anim_grab_y = m_cursor_y - m_anim->regionY();
                return;
            }

            if (m_tool && m_cursor_seen && m_document
             && m_tool->kind() == PaintToolKind::Select
             && !paint_modifiers().ctrl() && !paint_modifiers().shift()
             && m_document->SelectionContains(m_cursor_x, m_cursor_y)
             && lift_for_carry())
            {
                m_sel_carry  = true;
                m_sel_grab_x = m_cursor_x;
                m_sel_grab_y = m_cursor_y;
                ETCS_LOG("PaintInput", "selection lifted at " << m_cursor_x << "," << m_cursor_y);
                repaint_view();
                return;
            }

            if (m_tool && m_cursor_seen)
            {
                // History, at the ONE point a continuous stroke starts writing.
                // An anchored kind writes at release and is remembered there
                // (commit_anchored); doing both here would spend an entry on a
                // preview that may commit nothing.
                //
                // OPENS A Dab ENTRY rather than taking a snapshot: the points
                // arrive afterwards, through PaintDocument::ApplyBrush, and the
                // release seals it. An entry that never gets a point -- a press
                // that did not move and marked nothing -- is dropped at the seal
                // rather than recorded as an empty stroke.
                if (m_document && !paint_kind_is_anchored(m_tool->kind()) && !paint_kind_is_placed(m_tool->kind()))
                {
                    const bool smudge = (m_tool->kind() == PaintToolKind::Smudge);
                    m_document->RememberOp(smudge ? PaintOpKind::Smudge : PaintOpKind::Dab, m_tool->brush());
                    // A smudge's first point marks nothing but starts the path
                    // every later step carries from; it goes in the entry now.
                    if (smudge) m_document->StrokeTo(m_cursor_x, m_cursor_y);
                }
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;

                const PaintToolKind k = m_tool->kind();
                /*
                 * WHICH KIND OF SELECTION GESTURE THIS IS, decided once, here.
                 * ctrl adds the region to what is already selected, shift takes
                 * it away, neither replaces. The modifiers come from the key
                 * stream because the event carries none (PaintModifierKeys).
                 * Read at the press and held for the whole drag: letting go of
                 * ctrl halfway would otherwise turn an add into a replace and
                 * lose the region it was adding to.
                 */
                if (k == PaintToolKind::Select && m_document)
                {
                    m_sel_op = paint_modifiers().ctrl()  ? PaintSelectOp::Add
                             : paint_modifiers().shift() ? PaintSelectOp::Subtract
                                                         : PaintSelectOp::Replace;
                    m_document->BeginSelectionGesture(m_sel_op);
                }
                // A PLACED tool is finished here: one point is the whole
                // gesture, so it commits on the press and the release that
                // follows has nothing left to do.
                if (paint_kind_is_placed(k))       apply_placed(k, m_cursor_x, m_cursor_y);
                // The wand is the one select mode with nothing to drag: the
                // press names the colour, so the region is taken here and the
                // drag that may follow changes nothing (shape_selection).
                else if (k == PaintToolKind::Select && m_tool->mode() == PaintSelectMode::Wand
                      && m_document)
                {
                    m_document->SelectColor(m_cursor_x, m_cursor_y, m_tool->tolerance());
                    repaint_view();
                }
                // An anchored tool marks nothing yet -- the press is only where
                // the shape starts. Smudge likewise: it carries pixels from
                // somewhere, and on the first sample there is no somewhere.
                else if (!paint_kind_is_anchored(k) && k != PaintToolKind::Smudge)
                    apply_sample(m_cursor_x, m_cursor_y);
            }
        }
        else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP)
        {
            if (ev.action == INPUT_BUTTON_UP) m_stroke_hold.Release();
            // Letting go of the move tool ends the pan the press began. Before
            // everything else, because a pan started no stroke and has nothing
            // below here to commit.
            if (m_panning && m_tool && m_tool->kind() == PaintToolKind::Move)
            {
                flush_coalesced_motion();       // apply the last pending delta
                m_panning = false;
                m_pan_hold.Release();
                return;
            }
            // Letting go of a carried box. Checked before the commit below,
            // because a carry never began a stroke and there is nothing to commit.
            if (m_text_drag != 0)
            {
                ETCS_LOG("PaintInput", "text box " << m_text_drag << " moved");
                m_text_drag = 0;
                return;
            }
            if (m_anim_drag)
            {
                m_anim_drag = false;
                return;
            }
            if (m_text_resize != 0)
            {
                ETCS_LOG("PaintInput", "text box " << m_text_resize << " resized");
                m_text_resize = 0;
                return;
            }
            /*
         * Letting go of a carried selection: THE DROP, which is the one place
         * the select tool writes to the document. Drained first so the pixels
         * land where the last sample put them, not where the last flush did.
         */
            if (m_sel_carry)
            {
                flush_coalesced_motion();
                m_sel_carry = false;
                if (m_document && m_document->DropSelection())
                    ETCS_LOG("PaintInput", "selection dropped");
                repaint_view();
                return;
            }
            /*
         * THE COMMIT, and the only place an anchored tool ever writes to the
         * document. Read the anchor before EndStroke, which is what clears the
         * gesture.
         *
         * Ruler commits nothing by construction (paint_kind_commits), so
         * letting go of it simply removes the measurement -- which is what a
         * ruler you have finished with should do. Select commits nothing
         * either, but unlike the ruler it leaves something behind: the region,
         * stated one last time from the release position.
         */
            // Drain any motion held by the coalesce timer so the release
            // position is the one that was committed / previewed last.
            flush_coalesced_motion();
            if (m_tool)
            {
                const PaintToolKind k = m_tool->kind();
                // commit_anchored opens its OWN entry now -- it is the only
                // thing here that knows which shape is about to be drawn, and
                // an entry opened before that is known could only be a
                // snapshot. A glyph commit places a box rather than pixels and
                // still takes the undescribed path, inside place_text_box.
                if (m_tool->active() && paint_kind_is_anchored(k) && paint_kind_commits(k))
                    commit_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                    m_cursor_x, m_cursor_y);
                else if (m_tool->active() && k == PaintToolKind::Select)
                    end_selection(m_tool->anchorX(), m_tool->anchorY(),
                                  m_cursor_x, m_cursor_y);
                // A click is too small to be a region, and brings a closed
                // window back instead.
                else if (m_tool->active() && k == PaintToolKind::Animate && m_anim
                         && !m_anim->SetRegion(m_tool->anchorX(), m_tool->anchorY(),
                                               m_cursor_x, m_cursor_y))
                    m_anim->Open();
                m_tool->EndStroke();
                /*
                 * THE PREVIEW LIVES ON THE VIEW, so whatever the drag drew
                 * there has to go whether or not anything was committed -- and
                 * that is now true of every kind, not only the anchored ones.
                 *
                 * It was anchored-only while the live dab and the mark were the
                 * same pixels in the same colour: a freehand trail left behind
                 * looked exactly like the stroke it was previewing, so nothing
                 * ever noticed. Two changes made the dab a DIFFERENT picture
                 * from the mark. A selection clips the mark and not the dab, so
                 * the trail ran on past the region while the document stopped
                 * at its edge. And the eraser's dab is a neutral nib over a mark
                 * that is transparent (paint_stamp_surface), so an erased stroke
                 * stayed on screen as a grey smear.
                 *
                 * Both are one bug -- the view disagreeing with the document --
                 * and the release is the moment that has to end with them
                 * agreeing. Once per stroke, against once per motion sample,
                 * so this is not the path the live dab exists to protect.
                 *
                 * AND THE NOTEBOOK ENTRY IS SEALED HERE, for the same reason
                 * and at the same moment: the release is where the stroke's
                 * content is finally complete, so it is where the record of it
                 * stops being open. The two invariants are the same invariant.
                 */
                (void)k;
                if (m_document) m_document->SealOp();
                repaint_view();
            }
        }
    }

    /*
 * THE SAME EVENTS, FROM A SCRIPT. Pointer/Press/Release build an InputEvent and
 * hand it to HandleEvent -- the identical path a real device takes, one
 * function call later in it.
 *
 * Worth exporting on its own merits rather than only for the test: a paint
 * program wants scripted strokes (macros, replay, a demo that draws itself),
 * and this is what that is. It also makes the stroke state machine reachable
 * without a producer, which is the difference between a state machine that is
 * asserted on and one that is only ever watched.
 *
 * Deliberately NOT a second implementation. Anything these did differently
 * from the stream path would be a test of the wrong thing.
 *
 * WHICH IS WHY THEY ROUTE. These called HandleEvent directly, so a machine with
 * a root bound routed everything that arrived on an edge and nothing that
 * arrived from a script -- and routing is a property of how the machine is
 * CONFIGURED (BindRoot), not of which transport delivered the event. RouteEvent
 * falls through to HandleEvent when no root is bound, so an unrouted machine
 * behaves exactly as it did.
 *
 * It is also what makes a browser toolbar work at all: a canvas beside the
 * window is a separate DOM element, its clicks are not in the window's stream,
 * and the page hands them here through PaintInput.Pointer/Press
 * (etcs_web_call, loaders/etcs.cc).
 */
    void ScriptPointer(int32_t x, int32_t y)
    {
        InputEvent ev{};
        ev.action = INPUT_MOTION;
        ev.x = static_cast<int16_t>(x);
        ev.y = static_cast<int16_t>(y);
        // Both halves of what the routed consumer does with a position, in the
        // same order: record it as the routed cursor (a press carries no
        // coordinates and routes on the last position THIS path delivered --
        // see NoteRoutedCursor) and then route it.
        NoteRoutedCursor(ev.x, ev.y);
        RouteEvent(ev);
    }

    /*
     * A PRESS CARRIES NO POSITION -- x/y are meaningful for INPUT_MOTION only
     * (ontology/InputSource.h) -- so routing one on its own coordinates picks
     * whatever sits at the origin, which is nothing. The last position this path
     * delivered is where the press happened, which is the same assumption the
     * routed key edge makes (ConsumeRouted) and the same one absolute positions
     * make safe. Filled in here so the two paths route a press identically.
     */
    void ScriptPress()
    {
        InputEvent ev{};
        ev.action = INPUT_DOWN;
        ev.x = static_cast<int16_t>(RoutedCursorX());
        ev.y = static_cast<int16_t>(RoutedCursorY());
        RouteEvent(ev);
    }

    void ScriptRelease()
    {
        InputEvent ev{};
        ev.action = INPUT_UP;
        ev.x = static_cast<int16_t>(RoutedCursorX());
        ev.y = static_cast<int16_t>(RoutedCursorY());
        RouteEvent(ev);
    }

    /*
 * A KEY, WHILE A BOX HOLDS THE FOCUS.
 *
 * Returns whether it was consumed, and that answer is the "steals all keyboard
 * input" part: a selected box swallows the keystroke, so nothing downstream sees
 * it, and with nothing selected every key falls through untouched.
 *
 * NO MODIFIERS YET, because the event does not carry any -- InputEvent reserves a
 * byte for them and the window layer does not fill it (ontology/InputSource.h).
 * So letters arrive unshifted and are taken as lowercase, which is the common
 * case for typing; capitals need that byte carried through first, and inventing a
 * shift state here from key-down/key-up pairs would be a second source of truth
 * for something the event should simply say.
 */
    bool KeyDown(uint16_t key)
    {
        if (!m_document) return false;
        /*
     * A NAME BEING TYPED IN THE LAYER WINDOW TAKES EVERY KEY, ahead of the
     * chords and the text boxes both. That is what "captures input" means: while
     * the field is open ctrl+z is a z, Delete is a character and not a verb, and
     * nothing the keyboard does reaches the picture. It closes on Enter or
     * Escape (PaintLayerPanel::KeyIn), and only the panel knows it is open.
     */
        if (m_panel && m_panel->KeyIn(key)) return true;
        if (m_page_panel && m_page_panel->KeyIn(key)) return true;
        if (m_visitors && m_visitors->KeyIn(key)) return true;
        // GLFW's codes. Named rather than compared as bare numbers, because a
        // bare 259 in a paint program is unreadable.
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        constexpr uint16_t KEY_DELETE = 261;

        /*
     * ── chords ───────────────────────────────────────────────────────────
     *
     * Read before the text box gets the key, because ctrl+z in a box means the
     * document's undo, not a letter. GLFW's codes: the letter keys are their
     * ASCII uppercase. Each is glue to a document verb -- a script or the
     * terminal reaches the same thing by name.
     *
     *   ctrl+z          undo
     *   ctrl+shift+z    redo     (the two redo spellings, both honoured: the
     *   ctrl+y          redo      request named them as "the latter two")
     *   ctrl+c / x / v  copy, cut, paste the selection
     */
/*
     * ── chords ───────────────────────────────────────────────────────────
     *
     * Read before the text box gets the key, because ctrl+z in a box means the
     * document's undo, not a letter. GLFW's codes: the letter keys are their
     * ASCII uppercase. Each is glue to a document verb -- a script or the
     * terminal reaches the same thing by name.
     *
     *   ctrl+z          undo
     *   ctrl+shift+z    redo     (the two redo spellings, both honoured: the
     *   ctrl+y          redo      request named them as "the latter two")
     *   ctrl+c / x / v  copy, cut, paste the selection
     *   ctrl+PageUp     the previous page      (PaintPages::Prev)
     *   ctrl+PageDown   the next page          (PaintPages::Next)
     *
     * PageUp/PageDown and not ctrl+Tab, which is what a browser means by
     * "next tab" and what the window uses as its capture escape -- a chord
     * the substrate takes first is not a chord this program has.
     */
        constexpr uint16_t KEY_PAGE_UP = 266, KEY_PAGE_DOWN = 267;
        if (paint_modifiers().ctrl() && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN))
        {
            if (!m_pages) { ETCS_LOG("PaintInput", "chord ctrl+Page" << (key == KEY_PAGE_UP ? "Up" : "Down") << " -- no pages bound (BindPages)."); return true; }
            // A switch replaces the layers under everything: the carry, the
            // panel's rows and the view all describe the page that was.
            const bool did = (key == KEY_PAGE_UP) ? m_pages->Prev() : m_pages->Next();
            if (did)
            {
                m_sel_carry = false;
                if (m_panel) m_panel->Refresh();
                repaint_view();
            }
            return true;
        }

        if (paint_modifiers().ctrl())
        {
            constexpr uint16_t KEY_C = 67, KEY_V = 86, KEY_X = 88, KEY_Y = 89, KEY_Z = 90;
            bool did = false, handled = true;
            switch (key)
            {
            case KEY_Z: did = paint_modifiers().shift() ? m_document->Redo() : m_document->Undo(); break;
            case KEY_Y: did = paint_modifiers().shift() ? m_document->RedoAlt() : m_document->Redo(); break;
            case KEY_C: did = m_document->CopySelection(); break;
            case KEY_X: did = m_document->CutSelection(); break;
            case KEY_V: did = m_document->PasteSelection(); break;
            default:    handled = false; break;
            }
            if (handled)
            {
                if (did) { m_sel_carry = false; repaint_view(); }
                ETCS_LOG("PaintInput", "chord ctrl+" << static_cast<char>(key)
                         << (did ? "" : " -- nothing to do"));
                return true;
            }
        }

        const uint32_t sel = m_document->selectedTextBox();
        if (sel == 0)
        {
            /*
         * ESCAPE DROPS THE SELECTION when no box has the keyboard. The only
         * other way out is to hold the select tool and click beside the
         * region, which is two things to know for what is one intention.
         * Only this key, and only while there is a selection, so with nothing
         * selected every key still reaches nobody. A carry in flight lands
         * where it hovers rather than being lost (PaintDocument::ClearSelection).
         */
            if (key == KEY_ESCAPE && m_document->hasSelection())
            {
                m_document->ClearSelection();
                m_sel_carry = false;
                ETCS_LOG("PaintInput", "selection cleared");
                repaint_view();
                return true;
            }
            if (key == KEY_DELETE && m_document->hasSelection())
            {
                m_sel_carry = false;
                ETCS_LOG("PaintInput", "selection " << (m_document->DeleteSelection() ? "deleted" : "not deleted"));
                repaint_view();
                return true;
            }
            return false;
        }
        PaintTextBox* b = m_document->FindTextBox(sel);
        if (!b) { m_document->SelectTextBox(0); return false; }

        /*
     * ESCAPE LETS GO, ENTER STARTS A LINE. The box is a column now, and a
     * caption of two lines is typed, not made of two boxes. Letting go is also
     * the bar's `ok` and any press off the box.
     */
        if (key == KEY_ESCAPE)
        {
            m_document->SelectTextBox(0);
            repaint_view();
            return true;
        }
        if (key == KEY_ENTER)
        {
            b->text.push_back('\n');
            m_document->TextEdited(sel);
            repaint_view();
            return true;
        }
        if (key == KEY_BACKSPACE)
        {
            if (!b->text.empty()) b->text.pop_back();
            m_document->TextEdited(sel);
            repaint_view();
            return true;
        }
        if (key == KEY_DELETE)
        {
            // The box itself, since there is no caret to delete forward from.
            m_document->RemoveTextBox(sel);
            repaint_view();
            return true;
        }

        const char ch = paint_key_to_char_shifted(key, paint_modifiers().shift());
        if (ch == 0) return true;          // consumed: a modifier or a function key
        b->text.push_back(ch);
        m_document->TextEdited(sel);
        repaint_view();
        return true;
    }

    bool StrokeActive() const { return m_tool && m_tool->active(); }

    PaintDocument* document() const { return m_document; }
    PaintTool* tool() const { return m_tool; }
    PaintSurface* surface() const { return m_surface; }
    int32_t cursorX() const { return m_cursor_x; }
    int32_t cursorY() const { return m_cursor_y; }

private:

    enum class MotionKind : uint8_t { None, Pan, Stroke, Carry };

    static double now_ms()
    {
        using clock = ::std::chrono::steady_clock;
        return ::std::chrono::duration<double, ::std::milli>(
            clock::now().time_since_epoch()).count();
    }

    bool motion_coalesce_due()
    {
        double interval = (m_tool)
            ? m_tool->motionCoalesceMs()
            : PAINT_MOTION_COALESCE_DEFAULT_MS;
        const double t = now_ms();
        if (m_last_motion_ms <= 0.0 || (t - m_last_motion_ms) >= interval)
        {
            m_last_motion_ms = t;
            return true;
        }
        return false;
    }

    /*
     * Apply the latest pending pointer sample to the document / view. Called
     * when the coalesce interval elapses and again on button-up so the final
     * tip is never discarded. Between flushes, motion events only update the
     * pending coordinates.
     *
     * THE WINDOW RESTARTS WHEN THE FLUSH ENDS, not when it began. Stamped at
     * the start only, a flush slower than the interval left every motion queued
     * behind it already "due", so each one paid for a full preview of its own
     * and the backlog never collapsed: an anchored drag whose preview took
     * 400ms against a 100ms window handled all twelve samples of a short drag
     * one by one, and the press after it waited two seconds for its turn.
     * Measured from the end, what queued during a slow flush arrives inside
     * the window and only moves the pending point -- the queue drains at
     * routing speed and the next flush draws where the pointer is now.
     */
    void flush_coalesced_motion()
    {
        if (!m_motion_pending) return;
        m_motion_pending = false;
        struct Restamp { PaintInput& in; ~Restamp() { in.m_last_motion_ms = now_ms(); } } restamp{ *this };

        if (m_motion_kind == MotionKind::Pan)
        {
            if (m_surface)
            {
                m_surface->PanBy(m_pending_view_x - m_pan_from_x,
                                 m_pending_view_y - m_pan_from_y);
                repaint_view();
            }
            m_pan_from_x = m_pending_view_x;
            m_pan_from_y = m_pending_view_y;
            m_cursor_seen = false;
            m_motion_kind = MotionKind::None;
            return;
        }

        if (m_motion_kind == MotionKind::Carry)
        {
            // Where the lift hovers: the pointer less where it took hold, in
            // document space, since the offset is applied to document pixels.
            if (m_document && m_sel_carry)
            {
                m_document->SetSelectionOffset(to_doc_x(m_pending_view_x) - m_sel_grab_x,
                                               to_doc_y(m_pending_view_y) - m_sel_grab_y);
                repaint_view();
            }
            m_motion_kind = MotionKind::None;
            return;
        }

        if (m_motion_kind == MotionKind::Stroke && m_tool && m_tool->active())
        {
            /*
             * WHICH OF THE THREE THINGS A DRAG DOES -- same split as before,
             * but once per coalesce window instead of once per OS sample.
             * apply_segment still interpolates from m_last_* to the tip, so a
             * fast stroke stays continuous rather than dotted.
             */
            const PaintToolKind k = m_tool->kind();
            if (k == PaintToolKind::Select)
                shape_selection(m_tool->anchorX(), m_tool->anchorY(),
                                m_cursor_x, m_cursor_y);
            else if (paint_kind_is_anchored(k))
                preview_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                 m_cursor_x, m_cursor_y);
            else if (k == PaintToolKind::Smudge)
                apply_smudge(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
            else if (!paint_kind_is_placed(k))
                apply_segment(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
            m_last_x = m_cursor_x;
            m_last_y = m_cursor_y;
        }
        m_motion_kind = MotionKind::None;
    }

    void apply_sample(int32_t x, int32_t y)
    {
        if (!m_tool) return;
        const PaintBrushState& brush = m_tool->brush();
        if (m_document) m_document->ApplyBrush(x, y, brush);
        if (m_surface) m_surface->StampBrush(x, y, brush);
    }

    // Stamp along the segment between two samples, spaced so consecutive
    // stamps overlap by half a radius. Spacing by the BRUSH rather than by a
    // fixed step means a wide brush costs no more stamps than it needs and a
    // one-pixel brush still draws a continuous line.
    void apply_segment(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_tool) return;
        const float step = std::max(1.0f, m_tool->brush().size_px * 0.5f);
        const float dx = static_cast<float>(x1 - x0);
        const float dy = static_cast<float>(y1 - y0);
        const int   n  = static_cast<int>(std::sqrt(dx * dx + dy * dy) / step);

        for (int i = 1; i <= n; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n + 1);
            apply_sample(x0 + static_cast<int32_t>(dx * t),
                         y0 + static_cast<int32_t>(dy * t));
        }
        apply_sample(x1, y1);
    }

    /*
 * ── the four gesture shapes ──────────────────────────────────────────────
 *
 * PREVIEW GOES TO THE VIEW, COMMIT GOES TO THE DOCUMENT, and that split is what
 * makes an anchored tool possible at all. apply_sample writes to both because a
 * freehand mark is final the instant it is made; a shape is not final until the
 * button comes up, so everything before that lands only on the surface the next
 * composite overwrites (PaintSurface::Render re-blits every layer).
 */
    void preview_anchored(PaintToolKind kind, int32_t ax, int32_t ay,
                          int32_t bx, int32_t by)
    {
        if (!m_surface) return;
        repaint_view();                       // wipe the previous frame's preview
        const ETCS::RID target = m_surface->target();
        if (target == 0) return;
        Surface_* view = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!view || !m_tool) return;
        // ONE statement for the whole outline. Every DrawRect is a FillRect that
        // marks, and unbatched each mark walks to the root (ontology/
        // ObservableBase.h) -- an outline is hundreds of rects, which made the
        // marking, not the pixels, most of what a preview cost.
        etcs_observed_batch outline(static_cast<ETCS::Entity*>(view));

        /*
     * BACK INTO VIEW SPACE TO DRAW IT. The anchor and the cursor are document
     * coordinates (everything past HandleEvent's conversion is), and the preview
     * goes onto the view surface -- so the projection runs the other way here.
     * The nib width scales with it for the same reason the live stamp does: a
     * preview drawn at document width would not match the mark it is previewing.
     */
        const float z = m_surface->zoom();
        const int32_t vax = m_surface->DocToViewX(ax), vay = m_surface->DocToViewY(ay);
        const int32_t vbx = m_surface->DocToViewX(bx), vby = m_surface->DocToViewY(by);
        const PaintColor& c = m_tool->brush().color;
        const int w = std::max(1, static_cast<int>(m_tool->brush().size_px * z));
        switch (kind)
        {
        case PaintToolKind::Line:
            preview_line(view, vax, vay, vbx, vby, c, w);
            break;
        case PaintToolKind::Rect:
            preview_line(view, vax, vay, vbx, vay, c, w);
            preview_line(view, vbx, vay, vbx, vby, c, w);
            preview_line(view, vbx, vby, vax, vby, c, w);
            preview_line(view, vax, vby, vax, vay, c, w);
            break;
        case PaintToolKind::Ellipse:
            preview_ellipse(view, vax, vay, vbx, vby, c, w);
            break;
        case PaintToolKind::Shape:
            switch (m_tool->shape())
            {
            case PaintShapeMode::Rect:
                preview_line(view, vax, vay, vbx, vay, c, w);
                preview_line(view, vbx, vay, vbx, vby, c, w);
                preview_line(view, vbx, vby, vax, vby, c, w);
                preview_line(view, vax, vby, vax, vay, c, w);
                break;
            case PaintShapeMode::Ellipse:
                preview_ellipse(view, vax, vay, vbx, vby, c, w);
                break;
            default:
            {
                // In VIEW space, so the projected outline is the one committed.
                std::vector<std::pair<int32_t, int32_t>> v;
                paint_shape_vertices(m_tool->shape(), ax, ay, bx, by, v);
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const auto& p0 = v[i]; const auto& p1 = v[(i + 1) % v.size()];
                    preview_line(view, m_surface->DocToViewX(p0.first), m_surface->DocToViewY(p0.second),
                                 m_surface->DocToViewX(p1.first), m_surface->DocToViewY(p1.second), c, w);
                }
                break;
            }
            }
            break;
        case PaintToolKind::Ruler:
            preview_ruler(view, vax, vay, vbx, vby, ax, ay, bx, by, z);
            break;
        case PaintToolKind::Glyph:
            preview_line(view, vax, vay, vbx, vay, c, w);
            preview_line(view, vbx, vay, vbx, vby, c, w);
            preview_line(view, vbx, vby, vax, vby, c, w);
            preview_line(view, vax, vby, vax, vay, c, w);
            break;
        case PaintToolKind::Animate:
        {
            // The frame being chosen, in the page's highlight rather than the
            // brush's colour: this drag marks nothing.
            const PaintColor hi{ 0.35f, 0.55f, 0.95f, 1.0f };
            preview_line(view, vax, vay, vbx, vay, hi, 2);
            preview_line(view, vbx, vay, vbx, vby, hi, 2);
            preview_line(view, vbx, vby, vax, vby, hi, 2);
            preview_line(view, vax, vby, vax, vay, hi, 2);
            break;
        }
        default: break;
        }
        paint_mark_pixel_path(target);
    }

    void commit_anchored(PaintToolKind kind, int32_t ax, int32_t ay,
                         int32_t bx, int32_t by)
    {
        if (!m_document) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer || !m_tool) return;
        const PaintBrushState& brush = m_tool->brush();
        (void)layer;

        /*
     * THE ENTRY IS BUILT HERE, because this is the first point at which the
     * shape is known -- the press knew only that something anchored had begun
     * -- and PERFORMED: the document lands it through the same code a replay
     * runs and writes it down (PaintDocument::Perform).
     */
        auto shape = [&](PaintOpKind k)
        {
            PaintOp op = m_document->OpFor(k, brush);
            op.addPoint(ax, ay);
            op.addPoint(bx, by);
            m_document->Perform(std::move(op));
        };

        switch (kind)
        {
        case PaintToolKind::Line:    shape(PaintOpKind::Line);    break;
        case PaintToolKind::Rect:    shape(PaintOpKind::Rect);    break;
        case PaintToolKind::Ellipse: shape(PaintOpKind::Ellipse); break;
        case PaintToolKind::Shape:
            switch (m_tool->shape())
            {
            case PaintShapeMode::Rect:    shape(PaintOpKind::Rect);    break;
            case PaintShapeMode::Ellipse: shape(PaintOpKind::Ellipse); break;
            default:
            {
                // The RING rather than the shape mode: a replaying viewer must
                // not have to own a second copy of paint_shape_vertices, and a
                // mode added later would then draw as whatever that viewer's
                // build thought the name meant. Vertices are the wire form for
                // exactly the reason points are a stroke's.
                std::vector<std::pair<int32_t, int32_t>> v;
                paint_shape_vertices(m_tool->shape(), ax, ay, bx, by, v);
                PaintOp op = m_document->OpFor(PaintOpKind::Poly, brush);
                for (const auto& p : v) op.addPoint(p.first, p.second);
                m_document->Perform(std::move(op));
                break;
            }
            }
            break;
        // A glyph commit places a BOX, not pixels, and a box is the document's
        // rather than the layer's -- see PaintTextBox and place_text_box. It is
        // recorded as a Text entry when it is let go (SelectTextBox).
        case PaintToolKind::Glyph:   place_text_box(ax, ay, bx, by); break;
        default: break;
        }
        m_document->SealOp();
    }

    /*
 * A PLACED TOOL IS ONE CLICK AND ONE COMMIT, straight to the document -- there
 * is no drag to preview and nothing to take back on release.
 */
    void apply_placed(PaintToolKind kind, int32_t x, int32_t y)
    {
        if (!m_document || !m_tool) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer) return;

        if (kind == PaintToolKind::Fill)
        {
            // A fill's whole input is the point it starts from, the brush and
            // the tolerance -- performed, so it lands through the replay's own
            // code (the eraser's transparent ink included: ApplyOp derives it
            // from the entry's brush).
            PaintOp op = m_document->OpFor(PaintOpKind::Fill, m_tool->brush(), m_tool->tolerance());
            op.addPoint(x, y);
            m_document->Perform(std::move(op));
            ETCS_LOG("PaintInput", "fill at " << x << "," << y);
        }
        else if (kind == PaintToolKind::Eyedrop)
        {
            /*
         * THE COLOUR UNDER THE PRESS, as seen -- every visible layer composited
         * at that pixel, not the active layer's byte, because what the user
         * pointed at is the picture. Through the wheel when there is one, so the
         * swatch slot the wheel was aimed at takes the colour exactly as a pick
         * on the disc would; otherwise straight onto the tool. Then the kind that
         * was in use comes back: an eyedropper is a one-press detour, not a
         * mode you have to find your way out of.
         */
            float c[4] = { 0, 0, 0, 0 };
            if (m_document->SampleAt(x, y, c))
            {
                if (m_wheel) m_wheel->Apply(c[0], c[1], c[2], c[3] > 0.0f ? 1.0f : 0.0f);
                else         m_tool->SetColor(c[0], c[1], c[2], 1.0f);
                ETCS_LOG("PaintInput", "eyedrop at " << x << "," << y << " -> "
                         << c[0] << ", " << c[1] << ", " << c[2] << " (a " << c[3] << ")");
            }
            if (m_wheel) m_wheel->EndPick();
            else         m_tool->SetKind("brush");
        }
        repaint_view();
    }

    /*
 * ── PLACING AND EDITING A TEXT BOX ───────────────────────────────────────
 *
 * The glyph tool used to prompt on the terminal for a string and rasterise it
 * into the active layer. Two things were wrong with that, and they were the same
 * thing twice: the text stopped being text the instant it landed, and the typing
 * happened somewhere other than where the text was going.
 *
 * Now a drag places a BOX (PaintTextBox, held by the document) and the box takes
 * the keyboard. A press inside an existing box selects it instead of starting a
 * new one, so every box ever placed stays reachable as long as the tool is held.
 *
 * A TINY DRAG IS A CLICK, and a click on nothing is a deselect. Without that
 * there is no way to stop typing into a box except by choosing another tool.
 */
    /*
 * Outlines on while the text tool is held, off otherwise -- and a box stops being
 * selected when the tool is put down, because a selection that survives the tool
 * would swallow keystrokes with nothing on screen to explain why.
 */
    void sync_text_affordance()
    {
        if (!m_document) return;
        const bool want = (m_tool && m_tool->kind() == PaintToolKind::Glyph);
        if (want == m_text_affordance) return;
        m_text_affordance = want;
        m_document->ShowTextBoxes(want);
        if (!want) m_document->SelectTextBox(0);
        repaint_view();
    }

    void place_text_box(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document) return;
        const int32_t x0 = std::min(ax, bx), y0 = std::min(ay, by);
        const int32_t w  = std::abs(bx - ax), h = std::abs(by - ay);

        if (w < 6 || h < 6)
        {
            // Too small to be a box: treat it as a click, which selects whatever
            // is under it and otherwise clears the selection.
            const uint32_t hit = m_document->TextBoxAt(ax, ay);
            m_document->SelectTextBox(hit);
            if (hit) ETCS_LOG("PaintInput", "text box " << hit << " selected for typing");
            else     ETCS_LOG("PaintInput", "text selection cleared");
            repaint_view();
            return;
        }

        /*
     * EMPTY, AND IN THE TOOL'S COLOUR.
     *
     * Empty because the next thing that happens is typing: a box that arrives
     * holding the tool's default string means the first keystroke appends to
     * "Text" instead of starting the caption, and the user has to clear it before
     * saying anything. PaintTool::SetText is still how a script can put a string
     * in one (doc.SetTextBoxText), which is where a default belongs -- with the
     * caller that wants it, not with every box.
     */
        const PaintColor c = m_tool ? m_tool->brush().color
                                    : PaintColor{ 0.08f, 0.08f, 0.10f, 1.0f };
        // No snapshot in front of it: a box is recorded as a Text entry when it
        // is let go (PaintDocument::SelectTextBox), which is its undo step.
        const uint32_t id = m_document->AddTextBoxColoured(x0, y0, w, h,
                                                           c.r, c.g, c.b, c.a);
        m_document->SelectTextBox(id);
        repaint_view();
    }

    /*
 * ── DEFINING A SELECTION ─────────────────────────────────────────────────
 *
 * THE PREVIEW IS THE SELECTION. The shape tools draw a stand-in on the view
 * while dragging and commit the real thing on release; a region has no pixels
 * to commit, so there is nothing for a stand-in to stand in for. Every flush
 * simply re-states the region from the anchor and the tip, and the render path
 * draws it (PaintDocument::draw_selection). What you see while dragging is
 * therefore what you will have when you let go, by construction rather than by
 * two drawings agreeing -- and it costs a mask fill per flush, which at ten a
 * second is nothing against the re-composite that follows it.
 *
 * The wand is stated once, at the press, because the drag carries no
 * information it wants; re-flooding the same seed ten times a second would
 * give the same region back each time.
 */
    void shape_selection(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document || !m_tool) return;
        switch (m_tool->mode())
        {
        case PaintSelectMode::Rect:    m_document->SelectRect(ax, ay, bx, by);    break;
        case PaintSelectMode::Ellipse: m_document->SelectEllipse(ax, ay, bx, by); break;
        case PaintSelectMode::Lasso:   m_document->SelectPath(m_tool->points());  break;
        case PaintSelectMode::Wand:    return;
        }
        repaint_view();
    }

    /*
 * A CLICK IS A DESELECT, as it is for the text tool: a drag that went nowhere
 * names no region, and with nothing named the region that was there goes. It
 * is the only way out of a selection with the pointer, so it has to exist.
 *
 * Measured over the whole path rather than anchor-to-release, because a lasso
 * ends where it began -- that is what closing a loop means -- and would read
 * as a click every time.
 */
    void end_selection(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document || !m_tool) return;
        if (m_tool->mode() == PaintSelectMode::Wand) { m_document->EndSelectionGesture(); return; }

        int32_t lx = ax, rx = ax, ty = ay, by2 = ay;
        for (const PaintStrokePoint& p : m_tool->points())
        {
            lx = std::min(lx, p.x); rx = std::max(rx, p.x);
            ty = std::min(ty, p.y); by2 = std::max(by2, p.y);
        }
        if (rx - lx < 2 && by2 - ty < 2)
        {
            // A BARE click is the deselect; a MODIFIED one is not. Holding
            // ctrl or shift says "change this region", and the change a
            // zero-area drag describes is nothing -- so the region stands.
            // Without this, a misjudged press while adding would wipe
            // everything the user had built up, which is the one outcome an
            // additive mode must not have.
            if (m_sel_op == PaintSelectOp::Replace)
            {
                m_document->ClearSelection();
                ETCS_LOG("PaintInput", "selection cleared");
            }
            m_document->EndSelectionGesture();
            return;
        }
        shape_selection(ax, ay, bx, by);
        const PaintSelection& s = m_document->selection();
        ETCS_LOG("PaintInput", "selected " << s.count << " px ("
                 << paint_select_mode_name(m_tool->mode())
                 << (m_sel_op == PaintSelectOp::Add ? ", added"
                   : m_sel_op == PaintSelectOp::Subtract ? ", subtracted" : "") << ")");
        m_document->EndSelectionGesture();
    }


    // Smudge carries pixels, so it needs both ends of the step rather than one
    // point -- see PaintLayer::SmudgeDab. Strength is fixed rather than exposed
    // until there is a control for it; hardness is the closest existing dial and
    // means something else.
    void apply_smudge(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_document || !m_tool) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer) return;
        (void)x0; (void)y0;                       // the step starts where the entry's last point is
        m_document->StrokeTo(x1, y1);
        repaint_view();
    }

    // Re-composite the document onto the view. What makes a preview a preview:
    // the view is rebuilt from the layers, so anything drawn straight onto it
    // since the last one is gone.
    void repaint_view()
    {
        if (m_surface) m_surface->Render();
        if (m_text_bar) m_text_bar->Follow();
    }

    // A preview segment, as a run of small rects rather than brush dabs -- the
    // view surface is somebody else's and DrawRect is the primitive every
    // Surface has. Thickness follows the nib so the preview reads as the same
    // weight the commit will land at.
    static void preview_line(Surface_* view, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, const PaintColor& c, int w)
    {
        // A horizontal or vertical run is exactly the union of the squares the
        // walk below would stamp along it, so it is drawn as that one rect.
        // Every edge of a rectangle, a text box and the animation frame is one
        // of these, and the walk spends a rect per half-nib -- per PIXEL at the
        // 2px width the frame uses.
        if (x0 == x1 || y0 == y1)
        {
            view->DrawRect(std::min(x0, x1) - w / 2, std::min(y0, y1) - w / 2,
                           static_cast<uint32_t>(std::abs(x1 - x0) + w),
                           static_cast<uint32_t>(std::abs(y1 - y0) + w),
                           c.r, c.g, c.b, c.a);
            return;
        }
        const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        const int steps = std::max(1, std::max(dx, dy) / std::max(1, w / 2));
        for (int i = 0; i <= steps; ++i)
            view->DrawRect(x0 + (x1 - x0) * i / steps - w / 2,
                           y0 + (y1 - y0) * i / steps - w / 2,
                           static_cast<uint32_t>(w), static_cast<uint32_t>(w),
                           c.r, c.g, c.b, c.a);
    }

    static void preview_ellipse(Surface_* view, int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1, const PaintColor& c, int w)
    {
        const double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
        const double rx = std::abs(x1 - x0) * 0.5, ry = std::abs(y1 - y0) * 0.5;
        const int steps = std::clamp(static_cast<int>(std::max(rx, ry)), 24, 512);
        for (int i = 0; i <= steps; ++i)
        {
            const double t = (2.0 * 3.14159265358979323846 * i) / steps;
            view->DrawRect(static_cast<int32_t>(cx + rx * std::cos(t)) - w / 2,
                           static_cast<int32_t>(cy + ry * std::sin(t)) - w / 2,
                           static_cast<uint32_t>(w), static_cast<uint32_t>(w),
                           c.r, c.g, c.b, c.a);
        }
    }

    /*
 * THE RULER, which commits nothing and is therefore entirely this function.
 *
 * Two things are drawn: the measured span, and TICKS along it. The ticks are
 * what make it a ruler rather than a line with a length -- they are placed every
 * 10 device pixels with every tenth drawn long, so the pane's own size and width
 * can be read off it directly by counting, which is what it is for.
 *
 * The span's length is reported to the log as well, because a number you can
 * read is worth more than a number you have to count to, and the marks are for
 * aligning things rather than for arithmetic.
 */
    void preview_ruler(Surface_* view, int32_t vax, int32_t vay, int32_t vbx, int32_t vby,
                       int32_t ax, int32_t ay, int32_t bx, int32_t by, float zoom)
    {
        const PaintColor guide{ 0.95f, 0.80f, 0.20f, 0.85f };
        preview_line(view, vax, vay, vbx, vby, guide, 1);

        /*
     * TICKS ARE SPACED IN DOCUMENT PIXELS AND DRAWN IN VIEW PIXELS, which is the
     * whole of making a ruler projection-aware. A tick every 10 view pixels would
     * measure the screen; what a ruler is for is measuring the PICTURE, so the
     * spacing is 10 document pixels and it visibly opens out as you zoom in.
     *
     * The reported length is likewise the document's, so the number a ruler
     * gives you is the number a stroke would be -- it does not change when you
     * scroll the wheel, which would make it useless for the one job it has.
     */
        const double ddx = bx - ax, ddy = by - ay;
        const double doc_len = std::sqrt(ddx * ddx + ddy * ddy);
        const double vdx = vbx - vax, vdy = vby - vay;
        const double view_len = std::sqrt(vdx * vdx + vdy * vdy);
        if (view_len < 1.0 || doc_len < 1.0) return;

        const double ux = vdx / view_len, uy = vdy / view_len;
        const double nx = -uy, ny = ux;           // unit normal, for the tick marks
        const double spacing = std::max(2.0, 10.0 * zoom);   // 10 document px, on screen

        int mark = 0;
        for (double d = 0.0; d <= view_len; d += spacing, ++mark)
        {
            const bool major = (mark % 10) == 0;   // every 100 document pixels
            const double t = major ? 7.0 : 3.0;
            const double px0 = vax + ux * d, py0 = vay + uy * d;
            preview_line(view,
                         static_cast<int32_t>(px0 - nx * t), static_cast<int32_t>(py0 - ny * t),
                         static_cast<int32_t>(px0 + nx * t), static_cast<int32_t>(py0 + ny * t),
                         guide, 1);
        }
        ETCS_LOG("PaintInput", "ruler " << ax << "," << ay << " -> " << bx << "," << by
                 << "  dx=" << static_cast<int32_t>(ddx)
                 << " dy=" << static_cast<int32_t>(ddy)
                 << " len=" << static_cast<int32_t>(doc_len + 0.5) << "px (document)"
                 << " at " << static_cast<int32_t>(std::lround(zoom * 100.0f)) << "%");
    }

    PaintDocument* m_document = nullptr;
    PaintTool* m_tool = nullptr;
    PaintSurface* m_surface = nullptr;
    // Routing, all optional. Zero means "not routed", which is the
    // pre-toolbar behaviour and still what paint_surface.etcs wants.
    PaintPalette* m_palette = nullptr;
    ETCS::RID m_root   = 0;   // the frame the input channel speaks in
    ETCS::RID m_canvas = 0;   // the one node whose frame is the picture's
    bool      m_on_palette = false;
    int32_t   m_win_x = 0;    // the pointer in the WINDOW's frame -- see
    int32_t   m_win_y = 0;    // NoteRoutedCursor for why this is not m_cursor_x.
    int32_t m_cursor_x = 0;
    int32_t m_cursor_y = 0;
    // The previous sample, for joining one to the next.
    int32_t m_last_x = 0;
    int32_t m_last_y = 0;
    // Whether a position has arrived at all -- without it, a button press
    // before the first motion event would begin a stroke at the origin.
    bool    m_cursor_seen = false;
    PaintLayerPanel* m_panel = nullptr;
    PaintPagePanel*  m_page_panel = nullptr;   // see BindPagePanel
    PaintVisitors*   m_visitors   = nullptr;   // see BindVisitors
    PaintTextBar*    m_text_bar   = nullptr;   // see BindTextBar
    uint32_t         m_text_resize = 0;         // a box whose corner is being dragged
    PaintAnimation*  m_anim = nullptr;         // see BindAnimation
    uint64_t         m_panel_rev = 0;      // the document revision the panel last showed
    std::vector<ETCS::RID> m_redo_alt;      // see BindRedoAlt
    int              m_redo_alt_shown = -1; // what the control shows; the frame thread's alone
    // Whether the document is currently showing its text-box outlines, so the
    // reconcile above is a comparison rather than a call per event.
    bool m_text_affordance = false;
    // The box being carried, and where inside it the pointer took hold. 0 is
    // "nothing is being carried" -- see the press branch.
    uint32_t m_text_drag   = 0;
    bool     m_anim_drag   = false;      // carrying the animation camera
    int32_t  m_anim_grab_x = 0, m_anim_grab_y = 0;
    int32_t  m_text_grab_x = 0;
    int32_t  m_text_grab_y = 0;
    // The selection being carried, and where inside it the pointer took hold
    // -- the text box's carry, for a region. See the press branch.
    bool     m_sel_carry  = false;
    // What the selection drag in progress means -- read from the modifiers at
    // the press and held until the release (see the press handler).
    PaintSelectOp m_sel_op = PaintSelectOp::Replace;
    int32_t  m_sel_grab_x = 0;
    int32_t  m_sel_grab_y = 0;
    // Whatever leaf claiming Glyphs the script bound -- RenderProvider's
    // TextLabel today. By RID and resolved per use, since it is another
    // module's entity (see place_glyphs).
    ETCS::RID m_glyphs = 0;
    PaintColorWheel* m_wheel = nullptr;
    // The window currently on top and the order it will go back to -- see
    // FocusWindow. Base is meaningless while m_focus_pane is 0.
    ETCS::RID m_focus_pane = 0;
    int32_t   m_focus_base = 0;
    // The pane extent the windows were last clamped into -- see paneResized.
    uint32_t  m_pane_w = 0;
    uint32_t  m_pane_h = 0;
    bool    m_is_wheel_pane = false;
    bool    m_on_panel = false;
    // The right-button pan. Tracked in VIEW pixels because that is the frame a
    // drag is felt in -- see the motion branch.
    bool     m_panning = false;
    uint16_t m_pan_button = 0;
    // The two holds this input can be in, each re-judged per event while live.
    // See still_held. Capacity is deliberately small: see SetHoldCapacity.
    PaintPages*      m_pages = nullptr;   // quick switch -- see BindPages
    HeldCharge m_pan_hold{ 4 };
    HeldCharge m_stroke_hold{ 4 };
    uint16_t   m_stroke_button = 0;
    int32_t  m_pan_from_x = 0;
    int32_t  m_pan_from_y = 0;   // a press landed on the panel; its release is not a stroke

    // Motion coalesce -- interval from PaintTool::motionCoalesceMs().
    bool       m_motion_pending = false;
    MotionKind m_motion_kind    = MotionKind::None;
    int32_t    m_pending_view_x = 0;
    int32_t    m_pending_view_y = 0;
    double     m_last_motion_ms = 0.0;
};

#endif // PAINTPROVIDER_PAINTINPUT_H__
