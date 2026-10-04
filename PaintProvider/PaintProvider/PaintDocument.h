#ifndef PAINTPROVIDER_PAINTDOCUMENT_H__
#define PAINTPROVIDER_PAINTDOCUMENT_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintNotebook.h"   // in order: everything above this in the module is visible here

class PaintDocument : public DeletableBase<PaintDocument>
{
public:
    WIRE_TYPE_IDENTITY(PaintDocument);

    PaintDocument() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(uint32_t w, uint32_t h, const std::string& name = "Untitled")
    {
        m_width = w;
        m_height = h;
        m_name = name;
        m_active_layer = nullptr;
        return true;
    }

    void SetName(const std::string& name) { m_name = name; }

    /*
 * THE STACK, read from the TYPED CHILDREN this document actually holds.
 *
 * Membership is parenthood: a layer belongs to a document by being spawned as
 * its child (`doc.spawn(PaintProvider::PaintLayer ink)`), so there is no second
 * list to keep in step and no way for "in the document" and "in the tree" to
 * disagree. That is what the trait is for -- see PaintLayer::Subject.
 *
 * ORDERED BY THE LAYERS' OWN RELATION, and read straight out of the list that
 * holds them: getOrderedTypedChildRefs reads this document's ordered view,
 * which RIDList keeps in the pointee's operator< order when the concrete type
 * declares one, and PaintLayer does. So the composite order, the layer
 * window's row order and Layer_::Neighbourhood are not three implementations
 * of one rule -- they are one read.
 *
 * FILTERED BY TAG, because a document may hold children that are not layers and
 * an order across two concrete types is a question with no answer (core/
 * RIDList.h). Within the tag it is a real order, which is all this needs.
 */
    void OrderedLayers(std::vector<PaintLayer*>& out) const
    {
        out.clear();
        std::vector<ETCS::Entity::ChildRef> kids;   // no tag copied per layer
        this->getOrderedTypedChildRefs(kids);
        out.reserve(kids.size());
        static const ETCS::Buffer kLayerTag("PaintLayer");
        for (const auto& [tag, rid] : kids)
        {
            if (!(*tag == kLayerTag)) continue;
            ETCS::Entity* child = this->getTypedChild(*tag, rid);
            if (!child) continue;
            if (auto* l = static_cast<PaintLayer*>(child->getTrueType())) out.push_back(l);
        }
    }

    /*
 * Move a layer to a stated depth and renumber the rest to match.
 *
 * The layer window drags a row; what it means is "this one is now Nth from the
 * bottom", which is a statement about the WHOLE stack rather than about one key.
 * Renumbering densely from the resulting order is what makes the next drag mean
 * the same thing -- leaving gaps or duplicates would make a second drag land
 * somewhere the user did not point at. One SetOrder per layer, and each one
 * marks the holding list stale (Orderable_::Reorder), so the rebuild happens
 * once on the next ordered read rather than once per layer.
 */
    void MoveLayerTo(ETCS::RID layer_rid, int32_t depth)
    {
        if (refuse_read_only("restack")) return;
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* moved = static_cast<PaintLayer*>(raw->getTrueType());
        if (!moved) return;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        auto it = std::find(stack.begin(), stack.end(), moved);
        if (it == stack.end()) return;
        stack.erase(it);
        const int32_t slot = std::clamp(depth, 0, static_cast<int32_t>(stack.size()));
        stack.insert(stack.begin() + slot, moved);

        // ONE ENTRY FOR THE WHOLE RESTACK, not one per renumbered layer: the
        // stack before, then the stack after, performed.
        PaintOp op = rosterNow();
        for (size_t i = 0; i < stack.size(); ++i)
            for (PaintOp::Face& f : op.roster)
                if (f.key == keyOf(stack[i])) f.order = static_cast<int32_t>(i);
        sealOpenOp();
        keyframeRoster();
        PerformStack(std::move(op));
    }

    /*
 * ── merging two layers into one ──────────────────────────────────────────
 *
 * MERGE DOWN puts this layer's pixels onto the one beneath and drops this one;
 * MERGE UP is the same act read from the other end, and the two are one
 * function because the only thing that differs is which of the pair survives.
 *
 * WHICH DIRECTION THE PIXELS GO IS NOT THE SAME AS WHICH LAYER SURVIVES, and
 * that is the whole subtlety. Merging down, the upper layer goes over the lower
 * and the lower keeps the result -- one composite straight into its bytes.
 * Merging up, the upper still goes over the lower, but the UPPER is what
 * survives, so the result has to be built somewhere else and moved in. Getting
 * this backwards produces a merge that looks right until one of the two has
 * transparency, which is every interesting case.
 *
 * THE SOURCE'S OPACITY IS BAKED IN, because after the merge there is no layer
 * left to carry it. The survivor keeps its own, unspent: it is still a layer
 * and still has one.
 *
 * A HIDDEN SOURCE IS REFUSED. Merging ink nobody can see into a layer they can
 * is a change whose whole effect is invisible until it is too late to undo it
 * cheaply -- and the fix is one click, so saying so beats guessing.
 *
 * UNDO RESTORES THE PIXELS, NOT THE LAYER. Remember() takes the survivor's
 * bytes, so ctrl+z puts the picture back; the layer that was merged away is
 * detached, not deleted (RemoveLayer's own note), and nothing here re-attaches
 * it. Same limitation RemoveLayer has carried all along, stated rather than
 * discovered.
 */
    bool MergeLayer(ETCS::RID layer_rid, int direction)
    {
        if (refuse_read_only("merge")) return false;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return false;
        auto* self_layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!self_layer) return false;

        auto it = std::find(stack.begin(), stack.end(), self_layer);
        if (it == stack.end()) return false;
        const size_t idx = static_cast<size_t>(it - stack.begin());

        // direction < 0 is "down", toward the base of the stack.
        const size_t other = (direction < 0) ? (idx ? idx - 1 : idx) : idx + 1;
        if ((direction < 0 && idx == 0) || other >= stack.size())
        {
            ETCS_LOG("PaintDocument", "'" << self_layer->name() << "' has nothing "
                     << (direction < 0 ? "below" : "above") << " it to merge with.");
            return false;
        }

        PaintLayer* upper = (direction < 0) ? self_layer : stack[other];
        PaintLayer* lower = (direction < 0) ? stack[other] : self_layer;
        PaintLayer* keep  = (direction < 0) ? lower : upper;
        PaintLayer* gone  = (direction < 0) ? upper : lower;

        if (!gone->visible())
        {
            ETCS_LOG("PaintDocument", "'" << gone->name() << "' is hidden -- show it "
                     "before merging, or its ink lands where nobody asked for it.");
            return false;
        }

        /*
         * THE MERGED RASTER IS COMPUTED, NOT WRITTEN: lower, then upper over it
         * at the upper's opacity, into a buffer the survivor's size. The merge
         * is then one stack entry -- the stack without the gone layer, carrying
         * those bytes for the survivor -- performed like any stack change
         * (PerformStack), which is also exactly what its replay does. Both
         * halves on one entry, so undo takes the merge off in one step and a
         * redo brings the paint back with the plane.
         */
        if (lower->PixelWidth() != upper->PixelWidth() || lower->PixelHeight() != upper->PixelHeight())
        {
            ETCS_LOG("PaintDocument", "merge: '" << upper->name() << "' and '"
                     << lower->name() << "' are different sizes -- refused.");
            return false;
        }
        std::vector<uint8_t> merged;
        if (!lower->SnapshotBytes(merged)) return false;
        paint_composite_raw_scaled_bytes(merged.data(), lower->width(), lower->height(),
                                         lower->width() * 4,
                                         upper->PixelData(), upper->width(), upper->height(),
                                         0, 0, upper->width(), upper->height(),
                                         upper->opacity());

        if (m_sel.lifted()) DropSelection();       // a carry in flight lands first, as its own step
        recordStructure("merge");                  // every raster and the stack before
        PaintOp op = rosterNow();
        const uint64_t gone_key = keyOf(gone);
        op.roster.erase(std::remove_if(op.roster.begin(), op.roster.end(),
                        [&](const PaintOp::Face& f) { return f.key == gone_key; }), op.roster.end());
        op.layer = keyOf(keep);
        op.order = keep->order();
        op.w = keep->PixelWidth();
        op.h = keep->PixelHeight();
        op.bytes = std::move(merged);
        const std::string went = gone->name();
        PerformStack(std::move(op));
        SetActiveLayer(keep->getRID());
        etcs_mark_observed(keep);
        ETCS_LOG("PaintDocument", "merged '" << went << "' into '" << keep->name() << "'.");
        return true;
    }

/*
 * ── has anything changed, and since when ─────────────────────────────────
 *
 * A COUNT, NOT A FLAG. "Dirty" as a bool has to be cleared by whoever saved,
 * and two savers -- the page store and, one day, an autosave -- would clear
 * it for each other. A revision that only ever climbs lets each reader keep
 * the number it last saw and compare; nothing is reset and nobody's answer
 * depends on who asked before them (PaintPages::dirty).
 *
 * ONE SEAM, the same one the history has: Remember() is called at every
 * pixel commit, so it calls this. The structural verbs -- reorder, rename,
 * remove, import, the text boxes, a layer's own properties through
 * touch_document -- call it directly, because they change what a page IS and
 * Remember does not see them.
 */
    void Touch() { ++m_revision; }
    uint64_t revision() const { return m_revision; }

    // The layer's own setter records the change (PaintLayer::SetName) and
    // refuses it on a view-only page.
    void RenameLayer(ETCS::RID layer_rid, const std::string& name)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        if (auto* l = static_cast<PaintLayer*>(raw->getTrueType())) l->SetName(name);
    }

    /*
 * Out of the stack, not out of existence -- which is exactly what
 * Entity::detachFromParent already means: downward reachability ends, the
 * upward link and the entity itself remain. So a removed layer is still a live
 * entity a script holds a name for and can re-parent or inspect; it is simply
 * no longer one of the views this document composites.
 *
 * Deleting it is a different verb with a different meaning, and it is the one
 * Deletable already provides.
 */
    /*
 * RECORDED, so it comes back. This used to be the other one-way door: the
 * layer was detached, nothing in the notebook said the stack had changed, and
 * undo could restore every raster on a plane that was no longer there. It is
 * one keyframe pass and one metadata entry, and it buys undo for a delete.
 */
    // The stack without it, performed (PerformStack) -- reconcile is what
    // takes it out, here as on every member.
    void RemoveLayer(ETCS::RID layer_rid)
    {
        if (refuse_read_only("remove a layer")) return;
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!layer) return;
        // The bottom of the stack is the page's ground and stays: a document
        // with no layer at all has nothing to draw on, and "delete the base"
        // is almost always a slip. ClearLayer is the verb for emptying it.
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        if (std::find(stack.begin(), stack.end(), layer) == stack.end()) return;
        if (stack.front() == layer)
        {
            ETCS_LOG("PaintDocument", "layer '" << layer->name()
                     << "' is the base of '" << m_name << "' and is not removable -- ClearLayer empties it.");
            return;
        }
        if (m_sel.lifted()) DropSelection();
        recordStructure("remove layer");
        PaintOp op = rosterNow();
        const uint64_t key = keyOf(layer);
        op.roster.erase(std::remove_if(op.roster.begin(), op.roster.end(),
                        [&](const PaintOp::Face& f) { return f.key == key; }), op.roster.end());
        PerformStack(std::move(op));
    }

    /*
 * HOVER ISOLATION: the named layer at full strength, every other one dimmed.
 *
 * Stated as one call over the whole stack rather than as a dim per row, because
 * the property being set is a property of the STACK -- "exactly one of you is the
 * subject" -- and setting it per row makes leaving a row a second, separate
 * bookkeeping problem that gets it wrong the moment the pointer skips a row.
 * Naming a layer that is not here, or 0, is therefore how you say "nobody is"
 * and is the same call as ClearIsolate.
 */
    void IsolateLayer(ETCS::RID layer_rid, float dim)
    {
        PaintLayer* subject = nullptr;
        if (layer_rid != 0)
        {
            if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid))
                subject = static_cast<PaintLayer*>(raw->getTrueType());
        }
        const float other = std::clamp(dim, 0.0f, 1.0f);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* l : stack)
        {
            l->SetDim((subject && l == subject) ? 1.0f : (subject ? other : 1.0f));
            // A hidden subject comes up as the rest goes down, to the strength
            // they left: at the panel's 0.25 it shows at 0.75 -- plainly there,
            // and still plainly not a layer that is switched on.
            l->SetPeek((subject && l == subject && !l->visible()) ? 1.0f - other : 0.0f);
        }
    }

    void ClearIsolate() { IsolateLayer(0, 1.0f); }

    /*
 * The stack as text, bottom to top, for a layer window to build rows from and
 * for a test to assert on. RID first because that is what a row has to hold to
 * name this layer again -- every other verb here takes one.
 */
    void Report() const
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        ETCS_LOG("PaintDocument", "'" << m_name << "' " << stack.size() << " layer(s), bottom first:");
        for (auto* l : stack)
            ETCS_LOG("PaintDocument", "  RID:" << l->getRID()
                     << " order=" << l->order()
                     << " '" << l->name() << "'"
                     << (l->visible() ? " visible" : " hidden")
                     << " opacity=" << l->opacity()
                     << " dim=" << l->dim()
                     << " inked=" << l->InkedPixels()
                     << (l == m_active_layer ? "  <- active" : ""));
        // The text boxes are content too, and the only way to read one back --
        // they are the one thing here whose state is a string rather than pixels.
        if (!m_text.empty())
            ETCS_LOG("PaintDocument", "  " << m_text.size() << " text box(es)"
                     << (m_text_show ? ", outlines shown" : "")
                     << (m_text_sel ? ", editing " + std::to_string(m_text_sel) : ""));
        for (const PaintTextBox& b : m_text)
            ETCS_LOG("PaintDocument", "  text " << b.id << " at " << b.x << "," << b.y
                     << " " << b.w << "x" << b.h << " = \"" << b.text << "\"");
        // The selection by extent and count -- the count is what a test asserts
        // on, since a wand and a rectangle over the same box differ only there.
        if (!m_sel.empty())
            ETCS_LOG("PaintDocument", "  selection " << m_sel.x0 << "," << m_sel.y0
                     << " .. " << m_sel.x1 << "," << m_sel.y1 << " = " << m_sel.count << " px"
                     << (m_sel.lifted() ? "  lifted, at +" + std::to_string(m_sel.dx)
                                          + "," + std::to_string(m_sel.dy) : std::string()));

        // The notebook, in one line: what a test asserts on and what tells an
        // operator whether a shared session is actually recording anything.
        // Marks and snapshots counted apart because the two have completely
        // different costs, and a run whose entries are all snapshots is a run
        // where something is taking the undescribed path every time.
        size_t marks = 0, snaps = 0, points = 0;
        for (const PaintOp& o : m_book.ops())
        {
            if (o.marks()) { ++marks; points += o.points(); } else ++snaps;
        }
        ETCS_LOG("PaintDocument", "  notebook: " << m_book.size() << " entr(ies) to seq "
                 << m_book.head() << " -- " << marks << " mark(s) over " << points
                 << " point(s), " << snaps << " snapshot(s); at " << m_cursor
                 << ", " << undoDepth() << " back / " << redoDepth() << " forward"
                 << (m_open_live ? ", one open" : ""));
    }

    /*
 * THE CARRY LANDS BEFORE THE GROUND MOVES. A lift is cut from the active layer
 * and dropped onto the active layer (LiftSelection / DropSelection), and the
 * whole of that contract is that the two are the same layer. Nothing enforced
 * it: pressing a row in the layer window is a different pane's input, so the
 * canvas's drag state is untouched, and a selection lifted from Ink and then
 * dropped after clicking Paper wrote Ink's pixels into Paper. That is the
 * "select bleeds across layers" case, and it is a property of this verb rather
 * than of the panel -- every caller that can change the active layer has it,
 * including the exported verb and a page load.
 *
 * Dropping rather than refusing: the pixels are somewhere the user put them,
 * so they land where they are, on the layer they came from. Touch() because
 * which row is highlighted is part of what the window shows.
 */
    void SetActiveLayer(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        PaintLayer* next = static_cast<PaintLayer*>(raw->getTrueType());
        if (next == m_active_layer) return;
        if (m_sel.lifted()) DropSelection();
        m_active_layer = next;
        Touch();
    }

    void ClearLayer(ETCS::RID layer_rid, float r, float g, float b, float a)
    {
        if (refuse_read_only("clear a layer")) return;
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!layer) return;
        // As an entry (Clear), performed: the colour is the whole of its input.
        PaintOp op;
        op.kind  = PaintOpKind::Clear;
        op.layer = keyOf(layer);
        op.order = layer->order();
        op.brush.color = PaintColor{ r, g, b, a };
        Perform(std::move(op));
    }

    /*
 * ── the text boxes this document contains ────────────────────────────────
 *
 * Kept in creation order, which is also their draw order and their pick order
 * reversed: the last one placed is drawn on top, so it is the one a click
 * inside two overlapping boxes means.
 *
 * Addressed by an id rather than an index, because removing one would silently
 * renumber the others and a selection is held across edits.
 */
    uint32_t AddTextBox(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        return AddTextBoxColoured(x, y, w, h, 0.08f, 0.08f, 0.10f, 1.0f);
    }

    /*
 * A NEW BOX IS AN EDIT THAT HAS NOT ENDED: it is recorded when it is let go
 * (SelectTextBox), with whatever was typed into it, so placing a box and typing
 * a caption is one step of undo -- and a box let go empty is simply dropped,
 * never recorded, because an empty box is a click that missed. Its font and
 * size are the last ones the bar set, so a second caption matches the first.
 */
    uint32_t AddTextBoxColoured(int32_t x, int32_t y, int32_t w, int32_t h,
                                float r, float g, float bl, float a)
    {
        if (refuse_read_only("add text")) return 0;
        Touch();
        PaintTextBox b;
        b.rgba[0] = r; b.rgba[1] = g; b.rgba[2] = bl; b.rgba[3] = a;
        b.x = x; b.y = y;
        b.w = (w < 1) ? 1 : w;
        b.h = (h < 1) ? 1 : h;
        b.font = m_text_font;
        b.size = m_text_size;
        b.id = ++m_text_seq;
        b.key = (m_author.empty() ? std::string("-") : m_author) + "." + std::to_string(b.id);
        m_text.push_back(b);
        m_text_fresh = b.key;
        ETCS_LOG("PaintDocument", "text box " << b.id << " at " << b.x << "," << b.y
                 << " " << b.w << "x" << b.h);
        return b.id;
    }

    PaintTextBox* FindTextBox(uint32_t id)
    {
        for (auto& b : m_text) if (b.id == id) return &b;
        return nullptr;
    }
    const PaintTextBox* FindTextBox(uint32_t id) const
    {
        for (const auto& b : m_text) if (b.id == id) return &b;
        return nullptr;
    }

    // By verb: the whole string at once, recorded as an edit of its own unless
    // the box is open, in which case it is part of that edit.
    bool SetTextBoxText(uint32_t id, const std::string& text)
    {
        if (refuse_read_only("edit text")) return false;
        PaintTextBox* b = FindTextBox(id);
        if (!b) return false;
        b->text = text;
        if (id != m_text_sel) record_text(*b, false);
        Touch();
        return true;
    }

    /*
 * GONE, AS ONE STEP OF UNDO. A box that was never recorded -- placed and not
 * yet let go -- just goes; there is nothing in the history to undo.
 */
    bool RemoveTextBox(uint32_t id)
    {
        if (refuse_read_only("remove text")) return false;
        for (auto it = m_text.begin(); it != m_text.end(); ++it)
            if (it->id == id)
            {
                const PaintTextBox gone = *it;
                const bool was_open = (m_text_sel == id);
                if (was_open) m_text_sel = 0;
                m_text.erase(it);
                if (gone.key == m_text_fresh) m_text_fresh.clear();
                else record_text(gone, true);
                if (was_open && sharing()) hold_line(PaintOpKind::Free, gone.key, m_author);
                Touch();
                return true;
            }
        return false;
    }

    // Topmost box containing a document-space point, or 0. Reverse order, so the
    // answer matches what is drawn on top.
    uint32_t TextBoxAt(int32_t dx, int32_t dy) const
    {
        for (auto it = m_text.rbegin(); it != m_text.rend(); ++it)
            if (dx >= it->x && dy >= it->y
             && dx < it->x + it->w && dy < it->y + it->h)
                return it->id;
        return 0;
    }

    size_t textBoxCount() const { return m_text.size(); }

    /*
 * ── the open box's type ──────────────────────────────────────────────────
 *
 * What the text bar sets (PaintTextBar). Each changes the box in place -- part
 * of the edit that ends when the box is let go -- and becomes the style the
 * next new box starts with.
 */
    static constexpr uint32_t TEXT_SIZE_MIN = 6, TEXT_SIZE_MAX = 400;

    bool SetTextFont(uint32_t id, uint32_t font)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->font = m_text_font = font;
        Touch();
        return true;
    }
    bool SetTextSize(uint32_t id, uint32_t size)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->size = m_text_size = std::clamp(size, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
        Touch();
        return true;
    }
    bool SetTextColor(uint32_t id, float r, float g, float bl, float a)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->rgba[0] = r; b->rgba[1] = g; b->rgba[2] = bl; b->rgba[3] = a;
        Touch();
        return true;
    }

    // Whatever leaf claiming Glyphs draws them -- the document needs its own,
    // because it is what renders them, and it may be rendered with no input
    // machine attached at all.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }
    ETCS::RID glyphs() const { return m_glyphs; }

    /*
 * EDITING AFFORDANCES ARE A VIEW STATE, so they are set from outside rather than
 * inferred here: the document has no opinion about which tool is in hand. The
 * input machine turns this on while the text tool is held (PaintInput), which is
 * what makes every existing box visible and therefore selectable.
 */
    void ShowTextBoxes(bool on)   { m_text_show = on; }

    /*
 * ── OPENING AND LETTING GO OF A BOX ──────────────────────────────────────
 *
 * Selecting a box OPENS it: the keys go into it and the bar comes up over it.
 * Letting go -- Escape, a press elsewhere, the bar's `ok`, another box, the
 * page's idle timer in a session -- ENDS THE EDIT, and that is the moment it
 * is recorded: one Text entry with the box as it now stands, if anything about
 * it changed. So undo takes back a whole edit, and a session sees each edit
 * when it is finished and never half of one.
 *
 * IN A SHARED SESSION, OPEN IS CLAIMED. The page asks the node, which gives
 * each box to the first person who asks and to nobody else until they let go;
 * a claim refused comes back as TextDenied and the box goes back to how it was.
 * Letting go releases the claim after the edit has been pushed.
 */
    void SelectTextBox(uint32_t id)
    {
        if (id == m_text_sel) return;
        const uint32_t was = m_text_sel;
        m_text_sel = 0;
        if (was) end_text_edit(was);
        if (id == 0) return;
        const PaintTextBox* b = FindTextBox(id);
        if (!b) return;
        m_text_sel = id;
        m_text_before = *b;
        m_text_touched = std::chrono::steady_clock::now();
        if (sharing())
        {
            // Held by somebody else already, as far as this page knows: no
            // claim goes out, and the box is not opened.
            auto it = m_holders.find(b->key);
            if (it != m_holders.end() && it->second != m_author) { m_text_sel = 0; TextDenied(b->key, it->second); return; }
            hold_line(PaintOpKind::Hold, b->key, m_author);
        }
    }
    uint32_t selectedTextBox() const { return m_text_sel; }

    // A key went into the open box: the hold stays while someone is typing
    // (and is let go when they stop -- PaintShare::Tick).
    void TextEdited(uint32_t id)
    {
        (void)id;
        m_text_touched = std::chrono::steady_clock::now();
    }

    /*
 * THE NODE SAID NO: somebody else is holding this box. It goes back to what it
 * was when it was opened -- anything typed since was typed into a box that was
 * never ours -- and closes, and nothing is recorded or sent.
 */
    void TextDenied(const std::string& key, const std::string& holder)
    {
        PaintTextBox* b = nullptr;
        for (auto& t : m_text) if (t.key == key) b = &t;
        if (!b) return;
        if (b->id == m_text_sel && m_text_before.key == key)
        {
            const uint32_t id = b->id;
            *b = m_text_before;
            b->id = id;
            m_text_sel = 0;
        }
        Touch();
        ETCS_LOG("PaintDocument", "text box " << key << " is " << (holder.empty() ? std::string("someone else") : holder)
                 << "'s until they let go of it.");
    }

    /*
 * ── LAYING A BOX OUT ─────────────────────────────────────────────────────
 *
 * The box's lines, at its own size, as the Glyphs provider measures them:
 * each paragraph (split at the Enters) filled word by word until the next word
 * would pass the box's width, a word wider than the whole box broken where it
 * runs out. In DOCUMENT units, so where a line breaks does not depend on the
 * zoom it is looked at through -- every page in a session wraps a box at the
 * same words because they all measure with the same font files (PaintFonts).
 */
    static void wrap_text(Glyphs_* g, const PaintTextBox& b, std::vector<std::string>& lines)
    {
        lines.clear();
        auto width = [&](const std::string& t)
        { return static_cast<int32_t>(g->MeasureText(t.c_str(), b.font, b.size).width); };
        size_t start = 0;
        while (true)
        {
            const size_t nl = b.text.find('\n', start);
            const std::string para = b.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            std::string line;
            size_t i = 0;
            while (i < para.size())
            {
                // The next word, with the spaces in front of it.
                size_t j = i;
                while (j < para.size() && para[j] == ' ') ++j;
                while (j < para.size() && para[j] != ' ') ++j;
                const std::string word = para.substr(i, j - i);
                if (width(line + word) <= b.w || line.empty())
                {
                    if (width(line + word) <= b.w) { line += word; i = j; continue; }
                    // One word wider than the box: as much of it as fits.
                    size_t k = i;
                    std::string part;
                    while (k < j && (part.empty() || width(part + para[k]) <= b.w)) part += para[k++];
                    lines.push_back(part);
                    i = k;
                    continue;
                }
                lines.push_back(line);
                line.clear();
                i = para.find_first_not_of(' ', i);          // a new line starts at its word
                if (i == std::string::npos) i = para.size();
            }
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    // From one line's top to the next's: the size, and a sixth of it between.
    static int32_t line_step(const PaintTextBox& b)
    {
        return static_cast<int32_t>(b.size) + static_cast<int32_t>(b.size) / 6;
    }

    uint32_t textFont() const { return m_text_font; }
    uint32_t textSize() const { return m_text_size; }

    /*
 * ── the selection ────────────────────────────────────────────────────────
 *
 * ON THE DOCUMENT, like the active layer and the text box being typed into:
 * "what is selected" is a fact about the picture being edited, so two surfaces
 * onto one document show one selection, and it survives the tool being put
 * down. Held as a mask whatever drew it -- see PaintSelection.
 *
 * Four ways in and one representation out. Each Select* replaces the whole
 * selection rather than adding to it, because that is what one gesture means;
 * a mode that adds or subtracts would be a modifier the event does not carry
 * yet (PaintInput::KeyDown says why).
 *
 * All in DOCUMENT coordinates, like every other verb here that names a place.
 */
    bool SelectRect(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!fresh_selection()) return false;
        // Walked over the page only: a drag can run far off it (the pannable
        // region is three pages wide, PaintSurface::ClampPan) and Set would
        // refuse every one of those pixels one at a time.
        const int32_t lx = std::max(std::min(ax, bx), 0);
        const int32_t rx = std::min(std::max(ax, bx), static_cast<int32_t>(m_width) - 1);
        const int32_t ty = std::max(std::min(ay, by), 0);
        const int32_t by2 = std::min(std::max(ay, by), static_cast<int32_t>(m_height) - 1);
        for (int32_t y = ty; y <= by2; ++y)
            for (int32_t x = lx; x <= rx; ++x)
                m_sel.Set(x, y);
        combine_selection();
        return !m_sel.empty();
    }

    // Inscribed in the drag, as the ellipse tool is (PaintLayer::
    // DrawEllipseOutline), so the two agree about which pixels a drag names.
    // Tested at pixel centres, so a 1x1 drag selects its one pixel rather than
    // an ellipse of zero area.
    bool SelectEllipse(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!fresh_selection()) return false;
        const int32_t lx = std::min(ax, bx), rx = std::max(ax, bx);
        const int32_t ty = std::min(ay, by), by2 = std::max(ay, by);
        const double cx = lx + (rx - lx + 1) * 0.5, cy = ty + (by2 - ty + 1) * 0.5;
        const double rw = (rx - lx + 1) * 0.5,      rh = (by2 - ty + 1) * 0.5;
        // The geometry from the whole drag, the walk over the page only -- see
        // SelectRect.
        const int32_t wy0 = std::max(ty, 0), wy1 = std::min(by2, static_cast<int32_t>(m_height) - 1);
        const int32_t wx0 = std::max(lx, 0), wx1 = std::min(rx,  static_cast<int32_t>(m_width)  - 1);
        for (int32_t y = wy0; y <= wy1; ++y)
            for (int32_t x = wx0; x <= wx1; ++x)
            {
                const double u = (x + 0.5 - cx) / rw, v = (y + 0.5 - cy) / rh;
                if (u * u + v * v <= 1.0) m_sel.Set(x, y);
            }
        combine_selection();
        return !m_sel.empty();
    }

    // The wand: the run of colour under the point, on the ACTIVE layer, since
    // that is the layer a carry will cut from and the only one whose colour is
    // the question. Tolerance is the tool's, shared with the fill.
    bool SelectColor(int32_t x, int32_t y, uint32_t tolerance)
    {
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "wand at " << x << "," << y
                     << " with no active layer -- nothing to read the colour from.");
            return false;
        }
        if (!fresh_selection()) return false;
        const size_t n = m_active_layer->FloodMask(x, y, tolerance, m_sel);
        combine_selection();
        ETCS_LOG("PaintDocument", "wand at " << x << "," << y << " -> " << n << " px");
        return !m_sel.empty();
    }

    /*
 * The lasso: the pointer's path, closed back to its start, filled by scanline
 * with the even-odd rule. Even-odd rather than winding because a hand-drawn
 * loop crosses itself, and even-odd gives the crossing a definite answer -- the
 * lobes -- where winding would depend on which way round each loop went.
 * Sampled at row centres against the edges, so a path that doubles back over
 * a row still contributes one span per crossing pair.
 */
    bool SelectPath(const std::vector<PaintStrokePoint>& path)
    {
        if (path.size() < 3) return false;
        if (!fresh_selection()) return false;
        int32_t ty = path.front().y, by = ty;
        for (const auto& p : path) { ty = std::min(ty, p.y); by = std::max(by, p.y); }
        ty = std::max(ty, 0);
        by = std::min(by, static_cast<int32_t>(m_height) - 1);

        std::vector<double> xs;
        const size_t n = path.size();
        for (int32_t y = ty; y <= by; ++y)
        {
            const double yc = y + 0.5;
            xs.clear();
            for (size_t i = 0; i < n; ++i)
            {
                const PaintStrokePoint& a = path[i];
                const PaintStrokePoint& b = path[(i + 1) % n];
                if ((a.y <= yc) == (b.y <= yc)) continue;    // does not cross this row
                xs.push_back(a.x + (yc - a.y) * (b.x - a.x) / static_cast<double>(b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                const int32_t x0 = static_cast<int32_t>(std::ceil(xs[k] - 0.5));
                const int32_t x1 = static_cast<int32_t>(std::floor(xs[k + 1] - 0.5));
                for (int32_t x = x0; x <= x1; ++x) m_sel.Set(x, y);
            }
        }
        combine_selection();
        return !m_sel.empty();
    }

    // Nothing selected. A lift in flight lands where it is first -- clearing
    // must never lose pixels, only the outline round them.
    void ClearSelection()
    {
        if (m_sel.lifted()) DropSelection();
        m_sel.Reset(m_width, m_height);
    }

    // Inside the region as it currently sits -- which, mid-carry, is where the
    // lift hovers rather than where it was cut from.
    bool SelectionContains(int32_t x, int32_t y) const
    {
        if (m_sel.empty()) return false;
        return m_sel.lifted() ? m_sel.at(x - m_sel.dx, y - m_sel.dy) : m_sel.at(x, y);
    }

    /*
 * ── the carry ────────────────────────────────────────────────────────────
 *
 * LIFT, HOVER, DROP. The pixels leave the layer at the lift (PaintLayer::
 * LiftPixels), are drawn from the selection's own buffer at the offset while
 * they hover (draw_lift), and land through the layer's blend at the drop. Three
 * verbs rather than one "move by (dx, dy)" because a pointer does not know the
 * offset until the button comes up, and the picture has to be right at every
 * sample before that; MoveSelection below is the three in a row, for a caller
 * that does know.
 *
 * FROM AND ONTO THE ACTIVE LAYER, not a layer remembered at the lift. Nothing
 * changes the active layer during a drag today, and a layer pointer held across
 * one is the kind of thing a later panel drag would turn into a dangling read.
 */
    bool LiftSelection()
    {
        if (m_sel.empty() || m_sel.lifted()) return false;
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "nothing to lift the selection from -- no active layer.");
            return false;
        }
        m_active_layer->LiftPixels(m_sel, m_sel.lift);
        m_sel.dx = m_sel.dy = 0;
        return true;
    }

    void SetSelectionOffset(int32_t dx, int32_t dy)
    {
        if (!m_sel.lifted()) return;
        m_sel.dx = dx; m_sel.dy = dy;
    }

    bool DropSelection()
    {
        if (!m_sel.lifted()) return false;
        if (m_active_layer)
        {
            keyframeIfDue(m_active_layer);       // a paste's landing has had no keyframe yet; a carry's lift took one
            m_active_layer->DropPixels(m_sel.lift.data(), m_sel.width(), m_sel.height(),
                                       m_sel.x0 + m_sel.dx, m_sel.y0 + m_sel.dy);
            // THE WHOLE CARRY AS ONE ENTRY: the hole the lift left and the
            // place the pixels landed, which is one rectangle spanning both.
            recordPatch(m_active_layer,
                        std::min(m_sel.x0, m_sel.x0 + m_sel.dx), std::min(m_sel.y0, m_sel.y0 + m_sel.dy),
                        std::max(m_sel.x1, m_sel.x1 + m_sel.dx), std::max(m_sel.y1, m_sel.y1 + m_sel.dy));
        }
        else
            ETCS_LOG("PaintDocument", "no active layer to drop the selection onto -- "
                     "the lifted pixels are lost.");
        // The outline follows the pixels, so the same region can be carried
        // again without re-selecting it.
        m_sel.Shift(m_sel.dx, m_sel.dy);
        m_sel.lift.clear();
        m_sel.dx = m_sel.dy = 0;
        return true;
    }

    // Pixels from outside the document onto the active layer at a place, as
    // one step: the bytes go over what is there (PaintLayer::DropPixels) and
    // the rectangle is recorded (Patch). The animation window's "put" is this.
    bool PastePixels(const uint8_t* rgba, uint32_t w, uint32_t h, int32_t x, int32_t y)
    {
        if (refuse_read_only("paste")) return false;
        if (!m_active_layer || !rgba || w == 0 || h == 0) return false;
        keyframeIfDue(m_active_layer);
        m_active_layer->DropPixels(rgba, w, h, x, y);
        recordPatch(m_active_layer, x, y, x + static_cast<int32_t>(w) - 1, y + static_cast<int32_t>(h) - 1);
        return true;
    }

    // The scripted carry: what a drag does, in one call.
    bool MoveSelection(int32_t dx, int32_t dy)
    {
        keyframeIfDue(m_active_layer);
        if (!LiftSelection()) return false;
        SetSelectionOffset(dx, dy);
        return DropSelection();
    }

    const PaintSelection& selection() const { return m_sel; }
    bool hasSelection() const { return !m_sel.empty(); }

    /*
 * ── one gesture, three meanings ──────────────────────────────────────────
 *
 * The press says which (PaintInput::begin_selection reads the modifiers), and
 * it holds for the whole drag -- letting go of ctrl halfway through a drag
 * would otherwise turn an add into a replace and lose what was there.
 *
 * The base is the selection AT THE PRESS, kept whole because every motion
 * sample recombines against it. Sized to the page here rather than trusted,
 * since a document with nothing selected has an empty mask and the combine
 * indexes both.
 */
    void BeginSelectionGesture(PaintSelectOp op)
    {
        m_sel_op = op;
        if (op == PaintSelectOp::Replace) { m_sel_base.clear(); return; }
        const size_t need = static_cast<size_t>(m_width) * m_height;
        m_sel_base = m_sel.mask;
        if (m_sel_base.size() != need) m_sel_base.assign(need, 0);
    }

    void EndSelectionGesture()
    {
        m_sel_op = PaintSelectOp::Replace;
        m_sel_base.clear();
    }

    /*
 * ── the clipboard ────────────────────────────────────────────────────────
 *
 * OWNED BY THE DOCUMENT, not the input: a copy taken with one input machine
 * has to be pasteable by another, and the document is the one thing every
 * consumer of the picture already reaches. It holds the selection's SHAPE as
 * well as its bytes, so a paste puts back the same outline and not a rectangle
 * around it -- a lasso'd copy pastes as the lasso.
 *
 * WHERE A PASTE GOES: it FLOATS over where the copy came from, as a lift that
 * has not been dropped -- the same state a carry is in between press and
 * release, so no fourth state is needed. It cannot land at once: landing on
 * its own source is source-over onto identical bytes, which changes nothing,
 * and the next carry then takes the original along with it -- a paste that
 * duplicates nothing. Floating, the original stays where it is, the next drag
 * inside carries only the copy, and everything that already lands a carry in
 * flight (a new selection, Escape, a tool change) lands the paste the same way.
 *
 * AFTER A RESIZE the bytes are still the bytes; the mask is re-stated against
 * the document's current size at paste, and whatever falls outside it is
 * clipped by DropPixels. A layer that has gone away between copy and paste
 * costs nothing: the paste lands on whichever layer is active THEN.
 */
    bool CopySelection()
    {
        if (m_sel.empty() || !m_active_layer) return false;
        m_clip.mask = m_sel.mask;
        m_clip.w = m_sel.w; m_clip.h = m_sel.h;
        m_clip.x0 = m_sel.x0; m_clip.y0 = m_sel.y0; m_clip.x1 = m_sel.x1; m_clip.y1 = m_sel.y1;
        m_clip.count = m_sel.count;
        // A lifted region's bytes are in its own buffer, not the layer.
        if (m_sel.lifted()) m_clip.lift = m_sel.lift;
        else                m_active_layer->CopyPixels(m_sel, m_clip.lift);
        m_clip.dx = m_clip.dy = 0;
        return !m_clip.lift.empty();
    }

    bool PasteSelection()
    {
        if (refuse_read_only("paste")) return false;
        if (m_clip.lift.empty() || !m_active_layer) return false;
        if (m_sel.lifted()) DropSelection();      // a carry in flight lands first
        // The clip's mask, restated in the current document's space.
        m_sel.Reset(width(), height());
        for (int32_t y = m_clip.y0; y <= m_clip.y1; ++y)
            for (int32_t x = m_clip.x0; x <= m_clip.x1; ++x)
                if (m_clip.at(x, y)) m_sel.Set(x, y);
        if (m_sel.empty()) return false;
        // The lift IS the clip's bytes, cut to the bbox the restated mask has --
        // identical to the clip's unless the document shrank.
        m_sel.lift.assign(static_cast<size_t>(m_sel.width()) * m_sel.height() * 4, 0);
        const uint32_t cw = m_clip.width();
        for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
            for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
            {
                if (!m_sel.at(x, y)) continue;
                const size_t si = (static_cast<size_t>(y - m_clip.y0) * cw + (x - m_clip.x0)) * 4;
                const size_t di = (static_cast<size_t>(y - m_sel.y0) * m_sel.width() + (x - m_sel.x0)) * 4;
                if (si + 4 <= m_clip.lift.size()) ::std::memcpy(&m_sel.lift[di], &m_clip.lift[si], 4);
            }
        m_sel.dx = m_sel.dy = 0;
        return true;     // floating: see above
    }

    // The Delete key: the pixels go and the outline stays, so the same region
    // can be filled or pasted into next. Cut without the copy.
    bool DeleteSelection()
    {
        if (refuse_read_only("delete")) return false;
        if (m_sel.empty() || !m_active_layer) return false;
        if (m_sel.lifted()) { m_sel.lift.clear(); m_sel.dx = m_sel.dy = 0; }
        else
        {
            keyframeIfDue(m_active_layer);
            if (!LiftSelection()) return false;
            m_sel.lift.clear();
        }
        // The lift cleared the region; a carry that was in flight cleared it
        // when it was lifted. Either way the region is what changed.
        recordPatch(m_active_layer, m_sel.x0, m_sel.y0, m_sel.x1, m_sel.y1);
        return true;
    }

    // Copy, then take the pixels: the lift clears them and the buffer is let go.
    bool CutSelection()
    {
        if (refuse_read_only("cut")) return false;
        if (!CopySelection()) return false;
        if (!m_sel.lifted())
        {
            keyframeIfDue(m_active_layer);
            if (!LiftSelection()) return false;
        }
        m_sel.lift.clear();
        m_sel.dx = m_sel.dy = 0;
        recordPatch(m_active_layer, m_sel.x0, m_sel.y0, m_sel.x1, m_sel.y1);
        return true;
    }

    bool hasClip() const { return !m_clip.lift.empty(); }

    /*
 * ── history, as the notebook ─────────────────────────────────────────────
 *
 * THE SAME SEAM, A DIFFERENT STORE. Remember() is still what every committed
 * change calls before it lands, and every one of its call sites is unchanged.
 * What it records is now an entry in the notebook (PaintNotebook, above)
 * rather than one of three whole-layer snapshots, so the depth cap is gone,
 * undo is a replay, and the same object answers a viewer.
 *
 * Remember() IS THE KEYFRAME BEFORE A CHANGE, taken when the active layer is
 * due one (keyframeIfDue). Every change is described now -- a stroke by its
 * path (RememberOp), the rest by the rectangle it left (recordPatch) or the
 * colour it laid (ClearLayer) -- so nothing is recorded as bytes alone any
 * more, and the whole-layer keyframe is only ever the bound on a replay.
 *
 * WHAT IT COSTS: one snapshot per SNAPSHOT_EVERY marks per layer. At 1024x768
 * that is 3 MB every sixteen changes -- and Compact() can throw the prefix
 * away, which the old store could not do at all because three snapshots deep
 * has no prefix to throw.
 */
    void Remember()
    {
        Touch();
        keyframeIfDue(m_active_layer);
    }

    // The keyframe a mark is preceded by when its layer is due one, BEFORE the
    // change -- one taken after cannot undo it. RememberOp asks this for a
    // stroke; a change stated after the fact (recordMark) asks it itself,
    // first, because only the caller knows when "before" is.
    void keyframeIfDue(PaintLayer* layer)
    {
        sealOpenOp();
        if (!layer) return;
        if (m_book.snapshotDue(keyOf(layer))) appendSnapshot(layer);
    }

    /*
 * THE DESCRIBING SEAM. Same moment as Remember(), one fact richer: the caller
 * knows what it is about to do, so the notebook records the operation instead
 * of the pixels.
 *
 * A snapshot still goes in first when the layer is due one, and BEFORE the
 * mark rather than after -- a keyframe taken after the change it is supposed
 * to be undoable past is a keyframe of the wrong picture.
 */
    void RememberOp(PaintOpKind kind, const PaintBrushState& brush,
                    uint32_t tolerance = 0)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        Touch();
        sealOpenOp();
        if (!m_active_layer) return;
        if (m_book.snapshotDue(keyOf(m_active_layer)))
            appendSnapshot(m_active_layer);

        m_open = PaintOp{};
        m_open.kind      = kind;
        m_open.layer     = keyOf(m_active_layer);
        m_open.order     = m_active_layer->order();
        m_open.author    = m_author;
        m_open.brush     = brush;
        m_open.tolerance = tolerance;
        m_open.clip      = clipOf(m_sel);
        m_open_live      = true;
    }

    // The selection as an entry carries it (PaintOp::Clip): its bounding box
    // and the mask inside it. Nothing when nothing is selected.
    static PaintOp::Clip clipOf(const PaintSelection& sel)
    {
        PaintOp::Clip c;
        if (sel.empty()) return c;
        c.x0 = sel.x0; c.y0 = sel.y0;
        c.w = sel.width(); c.h = sel.height();
        c.mask.assign(static_cast<size_t>(c.w) * c.h, 0);
        for (int32_t y = sel.y0; y <= sel.y1; ++y)
            for (int32_t x = sel.x0; x <= sel.x1; ++x)
                if (sel.at(x, y)) c.mask[(static_cast<size_t>(y - sel.y0) * c.w) + (x - sel.x0)] = 1;
        return c;
    }

    // A point on the open entry. Called by ApplyBrush for a Dab, and by the
    // anchored commits for their corners -- one path, so an entry that was
    // opened and never given a point is an entry that marked nothing.
    void NoteOpPoint(int32_t x, int32_t y)
    {
        if (m_open_live) m_open.addPoint(x, y);
    }

    // Seal the open entry. Called at every stroke release, and again by the
    // next seam, which is what closes one a lost release left in the air.
    void SealOp() { std::lock_guard<std::recursive_mutex> hold(m_doc_mu); sealOpenOp(); }

    /*
     * A CHANGE STATED AFTER THE FACT, for the inputs that are not a brush
     * along a path: a rectangle of pixels as it now is (Patch), or a whole
     * layer cleared (Clear). Same seam as RememberOp -- seal, keyframe when
     * the layer is due one, then the entry -- with the entry built by the
     * caller, since only it knows the rectangle. No keyframe here: the change
     * has happened by now, and a keyframe of it could not undo it -- the
     * caller asked keyframeIfDue before it changed anything.
     */
    void recordMark(PaintOp op)
    {
        Touch();
        sealOpenOp();
        op.author = m_author;
        setCursor(m_book.Append(std::move(op), m_cursor));
    }

    // The rectangle (x0..x1, y0..y1 inclusive) of `layer` as it is now, as an
    // entry. Clipped to the layer; nothing to say when nothing of it is on it.
    void recordPatch(PaintLayer* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!layer) return;
        const int32_t pw = static_cast<int32_t>(layer->PixelWidth()), ph = static_cast<int32_t>(layer->PixelHeight());
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, pw - 1); y1 = std::min(y1, ph - 1);
        if (x1 < x0 || y1 < y0) return;
        PaintOp op;
        op.kind  = PaintOpKind::Patch;
        op.layer = keyOf(layer);
        op.order = layer->order();
        op.w = static_cast<uint32_t>(x1 - x0 + 1);
        op.h = static_cast<uint32_t>(y1 - y0 + 1);
        op.addPoint(x0, y0);
        if (!layer->ReadRect(x0, y0, op.w, op.h, op.bytes)) return;
        recordMark(std::move(op));
    }

    const PaintNotebook& notebook() const { return m_book; }
    PaintNotebook&       notebook()       { return m_book; }

    // Start the record over. The three callers are the three acts that make
    // every existing entry describe a picture that no longer exists: a resize
    // re-states every raster, New clears them all, and DestroyLayers takes them
    // away. Each used to drop two snapshot stacks and now drops one notebook,
    // which is the same sentence with less of it.
    void ClearHistory()
    {
        m_open      = PaintOp{};
        m_open_live = false;
        m_book.Clear();
        setCursor(0);
        m_text_fresh.clear();
        /*
     * THE BOXES THAT OUTLIVE THE HISTORY ARE STATED AT ITS START -- a resize
     * keeps its captions -- as keyframes, which undo steps over. Without them
     * the first undo afterwards would rebuild the boxes from a path that never
     * mentions them, and take every caption away.
     */
        for (const PaintTextBox& b : m_text) record_text(b, false, true);
    }

    // The picture as it stands goes to the room whole with the next Emit: what
    // a session opens with (write_baseline).
    void Announce() { std::lock_guard<std::recursive_mutex> hold(m_doc_mu); m_page_changed = true; }
    // Who authors entries made on this document from now on. Empty means this
    // page, which is what a document nobody is sharing keeps writing.
    void SetAuthor(const std::string& who)
    {
        m_author = who; setCursor(m_cursor);   // a session does not fork
        if (who.empty()) { m_holders.clear(); m_outbox.clear(); m_record = 0; m_owner.clear(); }
    }
    const std::string& author() const { return m_author; }

    /*
 * ── the notebook through a FILE, and why not through the call buffer ─────
 *
 * A page hands the runtime arguments through etcs_web_call, whose payload is
 * an ETCS::Buffer -- 256 bytes. One stroke does not fit, let alone a
 * snapshot. So the notebook crosses the same way an imported image already
 * does: the page writes the bytes into the browser's own filesystem and
 * passes a PATH, and the runtime reads the file.
 *
 * That is not a workaround for this feature, it is the established answer to
 * this exact question in this codebase (PaintCanvasMenu::OfferImport), and
 * reusing it means a shared session needs no new bridge, no new buffer size
 * and no second way for bulk data to reach the runtime.
 *
 * ExportOps writes what this page made and has not yet sent, which is the
 * only thing a writer has to push.
 */
    size_t ExportOps(const std::string& path)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o)
        {
            ETCS_LOG("PaintDocument", "ExportOps: cannot open '" << path << "'.");
            return 0;
        }
        const size_t n = export_ops(o);
        if (!o) { ETCS_LOG("PaintDocument", "ExportOps: write to '" << path << "' failed."); return 0; }
        return n;
    }
    // What this page made and has not sent, one line each -- see ExportOps.
    size_t export_ops(std::ostream& o)
    {
        /*
     * ALONG THE CANONICAL PATH, which is the only thing a session replays.
     * Sequence order would hand a viewer entries from a branch this document
     * abandoned -- strokes that were undone here would appear there, which is
     * the exact opposite of what an undo means. (In a session an undo is an
     * entry on the path, retract, so the path does not fork there.)
     *
     * A PAGE-LEVEL CHANGE MADE HERE -- New, a resize, another page opened --
     * is a new page for everyone, and goes out as one: a baseline, which is
     * the whole page stated (write_baseline), after which everything on the
     * path counts as sent. This page SAYS when that happened (m_page_changed);
     * it used to be inferred from a `since` that no longer sat on the path,
     * and a Page entry that arrived from the room emptied the notebook and
     * made that inference for every member at once -- each then re-sent its
     * whole page, emptying the others again, for as long as the room lasted.
     */
        /*
         * A RESTATE STARTS THE ROOM'S HISTORY AGAIN, and this page's with it.
         * Every other member takes the stated page as a new page (become_page)
         * whose history begins there, and an undo names a stroke by its
         * author's count of strokes along the path -- so a count that went on
         * from before the page here would name, there, a stroke that is not
         * on the path at all. Not while a stroke is open: it waits a push.
         */
        if (m_restate && !m_open_live)
        {
            ClearHistory();
            PaintOp stack = rosterNow();
            stack.keyframe = true;
            stack.author   = m_author;
            setCursor(m_book.Append(std::move(stack), m_cursor));
            m_page_changed = true;
            m_restate = false;
        }
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);

        size_t n = 0;
        if (m_page_changed)
        {
            ETCS_LOG("PaintDocument", "ExportOps: this page changed -- the room gets it whole.");
            ensureKeys();
            n = write_baseline(o);
            // The whole page stands for everything that made it.
            for (const PaintOp* op : chain) { const_cast<PaintOp*>(op)->sent = true; const_cast<PaintOp*>(op)->confirmed = true; }
            m_page_changed = false;
        }
        else
        {
            for (const PaintOp* op : chain)
            {
                if (op->sent) continue;
                /*
             * ONLY WHAT THIS PAGE MADE. Anything else on the path came from
             * the room and is marked on arrival (AcceptOp); a keyframe is
             * marked when it is taken (appendSnapshot), since it is this
             * page's own cache of its derived picture and on another member
             * would overwrite what that member derived. So an unmarked entry
             * is this page's, and this is the belt to those braces.
             */
                if (op->kind == PaintOpKind::Snapshot || (!m_author.empty() && op->author != m_author))
                {
                    const_cast<PaintOp*>(op)->sent = true;
                    continue;
                }
                const std::string line = paint_op_encode(*op);
                o << line << "\n";
                const_cast<PaintOp*>(op)->sent = true;
                const_cast<PaintOp*>(op)->wire = paint_line_rest(line);
                ++n;
            }
        }
        // Holds and releases, after the edits they follow.
        for (const std::string& line : m_outbox) { o << line << "\n"; ++n; }
        m_outbox.clear();
        if (n)
            ETCS_LOG("PaintDocument", "ExportOps: " << n << " entr(ies) along a " << chain.size() << "-entry path.");
        return n;
    }
    // Whether Emit has anything to send (under m_doc_mu).
    bool emit_due() const
    {
        if (m_page_changed || !m_outbox.empty() || (m_restate && !m_open_live)) return true;
        if (m_author.empty()) return false;
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        for (const PaintOp* op : chain)
            if (!op->sent && op->kind != PaintOpKind::Snapshot && op->author == m_author) return true;
        return false;
    }

    /*
 * THE WHOLE PAGE, FOR A ROOM THAT HAS NOTHING OF IT YET: a Page entry (extent
 * and stack) and a keyframe of every layer, bottom to top. What a session
 * opens with, and what a divergence re-sends (ExportOps).
 *
 * Every layer, not every layer the notebook touched: a page loaded from the
 * store or opened from a file has pixels no entry describes, and a history
 * pushed from zero sent none of them -- the joiner got the strokes and not the
 * picture under them.
 *
 * Everything on the path is marked sent after it (PaintOp::sent): the whole
 * page went, so every entry that made it is in the record.
 */
    size_t ExportBaseline(const std::string& path)
    {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { ETCS_LOG("PaintDocument", "ExportBaseline: cannot open '" << path << "'."); return 0; }
        ensureKeys();
        const size_t n = write_baseline(o);
        if (!o) { ETCS_LOG("PaintDocument", "ExportBaseline: write to '" << path << "' failed."); return 0; }
        // The whole page went, so everything that made it is in the record.
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        for (const PaintOp* op : chain) { const_cast<PaintOp*>(op)->sent = true; const_cast<PaintOp*>(op)->confirmed = true; }
        m_page_changed = false;
        ETCS_LOG("PaintDocument", "ExportBaseline: " << m_width << "x" << m_height << ", "
                 << (n ? n - 1 : 0) << " layer keyframe(s) at " << m_cursor << " -> '" << path << "'.");
        return n;
    }

    /*
 * EVERY LINE IS APPLIED AND KEPT, in the order it arrives. A line that does
 * not parse is skipped and said so rather than aborting the batch: a viewer
 * that drops one entry shows a slightly wrong picture, and a viewer that
 * stops reading shows a frozen one. The first is recoverable by the next
 * snapshot and the second is not recoverable at all.
 */
    size_t ImportOps(const std::string& path, uint64_t since = 0, const uint64_t* seed = nullptr)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            ETCS_LOG("PaintDocument", "ImportOps: cannot open '" << path << "'.");
            return 0;
        }
        // A read from zero is the record from its start, whatever this page
        // held: the chain starts again with it -- and so does the picture (a
        // Page entry replaces the document, become_page), so this page's OWN
        // lines are applied like everybody's: what it drew is in the record,
        // and nowhere else any more -- except a page this one just stated
        // (write_baseline), whose lines are chained and left alone.
        //
        // A read that starts where the record now starts (`seed`: its chain
        // there -- a record keeps itself from its last checkpoint on,
        // ontology/Record.h) is the same restart, from that point.
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        const bool restart = (since == 0) || seed;
        if (restart) { m_chain = seed ? *seed : 0; m_chain_seq = since; }
        std::string line;
        size_t taken = 0, bad = 0, mine = 0;
        while (std::getline(f, line))
        {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            /*
             * EVERY LINE IS CHAINED, and only the others' are applied. The
             * page hands over the whole read, its own lines included: a
             * writer applied its strokes as it made them, so taking the
             * copy the node sends back would draw each one twice -- but the
             * chain is over the record as the NODE holds it, own lines and
             * all, or the two could never agree.
             */
            m_chain = XXH3_64bits_withSeed(line.data(), line.size(), m_chain);
            {
                std::istringstream head(line);
                uint64_t seq = 0; std::string kind, author;
                if (head >> seq >> kind >> author)
                {
                    m_chain_seq = seq;
                    const bool own = !m_author.empty() && author == m_author;
                    /*
                     * THE PAGE THIS ONE STATED, read back: its picture is what
                     * those lines were made from, so they are chained and not
                     * applied -- the Page line and the keyframes after it. Own
                     * lines ahead of it were in flight when it was stated and
                     * are in it already; applied again they would draw twice.
                     */
                    if (own && !m_stated.empty())
                    {
                        if (kind == "page" && paint_line_rest(line) == m_stated) m_stated.clear();
                        else { ++mine; continue; }
                    }
                    if (own && m_stated_left) { --m_stated_left; ++mine; continue; }
                    // One of ours coming back: the entry already drawn here is
                    // now the record's, in the place the node gave it. A line of
                    // ours that is not in flight is history (a read from zero, a
                    // push from an earlier page of ours) and is taken below like
                    // anybody's.
                    if (own && confirm_own(paint_line_rest(line))) { ++mine; continue; }
                }
            }
            PaintOp op;
            if (!paint_op_decode(line, op))
            {
                ++bad;
                ETCS_LOG("PaintDocument", "ImportOps: unreadable entry: '"
                         << line.substr(0, 80) << (line.size() > 80 ? "..." : "") << "'");
                continue;
            }
            /*
             * THROUGH THE VERB, as a call: the replay of an entry is the ETCS
             * call Accept with the entry as its input, the same dispatch any
             * other change to this document takes -- so a replay is in the
             * trace like the change it replays, rather than a function this
             * one happens to run.
             */
            ETCS::Buffer ref;
            PaintOpRef::Emit(ref, &op);
            this->call(ETCS::Buffer("PaintDocument.Accept"), ref);
            ++taken;
        }
        finish_rewind();
        m_unreadable = bad;
        ETCS_LOG("PaintDocument", "ImportOps: " << taken << " entr(ies) from '" << path
                 << "'" << (mine ? ", " + std::to_string(mine) + " of this page's own confirmed" : "")
                 << (bad ? ", " + std::to_string(bad) + " unreadable and skipped" : "")
                 << "; at " << m_book.head() << ", record chain " << std::hex << m_chain
                 << std::dec << " at " << m_chain_seq << ".");
        return taken;
    }

    uint64_t recordChain()    const { return m_chain; }
    size_t   unreadable()     const { return m_unreadable; }   // in the last ImportOps
    uint64_t recordChainSeq() const { return m_chain_seq; }

    /*
     * ── the record as streams ────────────────────────────────────────────
     *
     * `doc.Emit() -> record.Take()` sends what this page makes as it makes it,
     * one entry per message, and `record.Follow(0) -> doc.Absorb()` takes the
     * record back in, every member's lines in the one order the record put
     * them. The file-and-fetch pair above (ExportOps, ImportOps) is what this
     * replaces for a session: a stream is ordered and whole or it has ended,
     * so nothing here reads a chain to find a missed line. What is still
     * compared is the PICTURE, through presence (PaintShare) -- a member whose
     * picture is not the owner's at the same head asks for the page whole.
     *
     * THE RECORD'S LINE IS THE LINK'S: "<seq> <author> <entry>". The author
     * is the name the line came in under (the link's hello), which is what
     * this page passes over as its own and what everyone else counts undo
     * ordinals by; the entry's own author field is the same name written by
     * the same page, and the record's is the one believed.
     *
     * The host's own Page line, coming back, is where the record starts over
     * (Record::Checkpoint): everything before it is history nobody replays.
     */
    // Emit's wait: 1 when there is something to send, 0 when there is not
    // yet, -1 when this page has stopped sharing.
    int emitState() const
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (!sharing()) return -1;
        return emit_due() ? 1 : 0;
    }
    // The lines due, marked sent -- a line that then does not go is pending
    // and comes off with RevertPending.
    std::vector<std::string> takeEmit()
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        std::ostringstream o;
        export_ops(o);
        std::vector<std::string> lines;
        std::istringstream in(o.str());
        std::string line;
        while (std::getline(in, line)) if (!line.empty()) lines.push_back(line);
        return lines;
    }
    // One of the record's lines, as Follow sends them; a restart line first
    // when the record now starts later than this page asked for.
    void absorb(const std::string& msg)
    {
        uint64_t checkpoint_at = 0;
        absorb_locked(msg, checkpoint_at);
        // Another module's verb, so not under this document's lock: a verb
        // waits on the runtime's ordering, and a thread that wants this lock
        // meanwhile would be the one to run it.
        if (checkpoint_at) checkpoint(checkpoint_at);
    }
    void absorb_locked(const std::string& msg, uint64_t& checkpoint_at)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (msg.compare(0, 2, "~ ") == 0)
        {
            unsigned long long b = 0;
            if (std::sscanf(msg.c_str() + 2, "%llu", &b) == 1) m_chain_seq = b;
            ETCS_LOG("PaintDocument", "the record starts at " << m_chain_seq << " now.");
            return;
        }
        // "<seq> <author> <entry...>"
        const size_t a = msg.find(' ');
        const size_t b = a == std::string::npos ? a : msg.find(' ', a + 1);
        if (b == std::string::npos) return;
        const uint64_t    seq    = std::strtoull(msg.c_str(), nullptr, 10);
        const std::string author = msg.substr(a + 1, b - a - 1);
        std::string line = msg.substr(b + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        m_chain_seq = seq;
        std::string kind;
        { std::istringstream head(line); uint64_t s0 = 0; std::string a0; head >> s0 >> kind >> a0; }
        const bool own = !m_author.empty() && author == m_author;
        if (kind == "hold" || kind == "free") { absorb_hold(line, author); return; }
        if (own && !m_stated.empty())
        {
            if (kind == "page" && paint_line_rest(line) == m_stated) { m_stated.clear(); checkpoint_at = seq; }
            else return;
        }
        if (own && m_stated_left) { --m_stated_left; return; }
        if (own && confirm_own(paint_line_rest(line))) { finish_rewind(); return; }
        PaintOp op;
        if (!paint_op_decode(line, op))
        {
            ++m_unreadable;
            ETCS_LOG("PaintDocument", "Absorb: unreadable entry: '" << line.substr(0, 80) << (line.size() > 80 ? "..." : "") << "'");
            return;
        }
        op.author = author;                       // the record's word, not the entry's
        ETCS::Buffer ref;
        PaintOpRef::Emit(ref, &op);
        this->call(ETCS::Buffer("PaintDocument.Accept"), ref);
        finish_rewind();
    }
    // Who holds which box, from the record's order -- see PaintOpKind::Hold.
    void absorb_hold(const std::string& line, const std::string& author)
    {
        PaintOp op;
        if (!paint_op_decode(line, op) || op.box.key.empty()) return;
        auto it = m_holders.find(op.box.key);
        if (op.kind == PaintOpKind::Hold)
        {
            if (it == m_holders.end() || it->second == author) { m_holders[op.box.key] = author; return; }
            if (author == m_author) TextDenied(op.box.key, it->second);
            return;
        }
        // A release: by the holder, or by the owner on a member's behalf.
        if (it == m_holders.end()) return;
        if (it->second == author || (author == m_owner && it->second == op.target)) m_holders.erase(it);
    }
    // Where the record starts over: the host's own Page line, back from it.
    void checkpoint(uint64_t seq)
    {
        if (!m_record || m_author != m_owner || !seq) return;
        ETCS::Entity* rec = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_record);
        if (!rec) return;
        ETCS::Buffer arg;
        arg.writeString(std::to_string(seq).c_str());
        rec->call(ETCS::Buffer("Ledger.Checkpoint"), arg);
        ETCS_LOG("PaintDocument", "the record is checkpointed at this page (" << seq << ").");
    }
    // The session's record and its owner, for checkpoints and releases.
    void SetRecord(ETCS::RID record, const std::string& owner) { m_record = record; m_owner = owner; }
    // The PaintShare this page is in a session through, told when a stream ends.
    void SetShare(ETCS::RID share) { m_share = share; }
    ETCS::Entity* share() const { return m_share ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_share) : nullptr; }
    // The owner's release of every box a member who left was holding.
    void ReleaseHeld(const std::string& member)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (m_author != m_owner) return;
        for (auto& [key, who] : m_holders)
            if (who == member) hold_line(PaintOpKind::Free, key, member);
    }
    // The box this page is typing into has been quiet this long (ms), or 0.
    long textIdleMs() const
    {
        if (!m_text_sel) return 0;
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_text_touched).count());
    }

    // Whether everything this page made is in the record: nothing on the
    // path still waiting to go (PaintOp::sent), no stroke open, no page-level
    // change the room has not been told of. Its picture is only the record's
    // picture when this answers yes -- a stroke is in its maker's picture
    // before it is anywhere else.
    bool settled() const
    {
        if (m_author.empty()) return true;
        if (m_open_live || m_page_changed) return false;
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        for (const PaintOp* o : chain) if (pending(o)) return false;
        return !m_text_sel;
    }

    /*
     * WHAT THE PICTURE IS, as one number: the document's own state surface and
     * subtree (Entity::getHash -- the layers as children, their flags), every
     * layer's pixels in stack order, and every text box. Two members at the
     * same record head that answer differently have diverged, whichever of
     * them is right, and that is a question the record chain cannot ask: it
     * says what was received, this says what was made of it.
     *
     * The pixels are hashed whole on every call rather than cached on a dirty
     * edge. A page asks once per presence tick (index.html, SHARE_VIEW_MS),
     * and a few megabytes through XXH3 is a millisecond or two -- cheaper than
     * being wrong about which write paths mark, which is the very thing this
     * exists to catch.
     */
    // The parts of PictureHash, one line per layer, for finding WHICH part two
    // members disagree on. A report, not a hash: it is what a divergence is
    // chased with.
    std::string PictureReport() const
    {
        std::ostringstream o;
        o << "picture " << m_width << "x" << m_height << " boxes " << m_text.size();
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const uint8_t* px = l->PixelData();
            o << " | layer RID " << l->getRID() << " order " << l->order() << " '" << l->name() << "' opacity " << l->opacity()
              << " visible " << l->visible() << " " << l->PixelWidth() << "x" << l->PixelHeight()
              << " px " << std::hex << (px ? XXH3_64bits(px, l->PixelBytes()) : 0) << std::dec;
        }
        return o.str();
    }

    uint64_t PictureHash() const
    {
        // NOT OVER Entity::getHash(). The node hash carries identity -- the
        // layers' RIDs, this page's own flags such as `readonly` -- and two
        // members with one picture have different identities by construction.
        // Only what the picture IS goes in: the extent, each layer's place,
        // opacity, visibility and pixels, and the boxes.
        uint64_t h = XXH3_64bits(&m_width, sizeof(m_width));
        h = XXH3_64bits_withSeed(&m_height, sizeof(m_height), h);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const int32_t  order   = l->order();
            const float    opacity = l->opacity();
            const uint8_t  visible = l->visible() ? 1 : 0;
            h = XXH3_64bits_withSeed(&order,   sizeof(order),   h);
            h = XXH3_64bits_withSeed(&opacity, sizeof(opacity), h);
            h = XXH3_64bits_withSeed(&visible, sizeof(visible), h);
            const uint8_t* px = l->PixelData();
            if (px) h = XXH3_64bits_withSeed(px, l->PixelBytes(), h);
        }
        for (const PaintTextBox& b : m_text)
        {
            std::string t = b.key + '\x1f' + b.text + '\x1f' + std::to_string(b.x) + ',' + std::to_string(b.y)
                          + ',' + std::to_string(b.w) + ',' + std::to_string(b.h) + ','
                          + std::to_string(b.font) + ',' + std::to_string(b.size);
            for (float c : b.rgba) t += ',' + std::to_string(c);
            h = XXH3_64bits_withSeed(t.data(), t.size(), h);
        }
        return h;
    }

    /*
 * ── UNDO AND REDO WALK THE TREE ──────────────────────────────────────────
 *
 * Undo is "stand on my parent", redo is "stand on my most recent child", and
 * between them that is the entire model. Neither exchanges buffers with the
 * other -- a snapshot stack had to, because it can only move a state from one
 * pile to another, and a tree can simply name a node.
 *
 * WHAT A BRANCH IS. Draw, undo, draw again: the second stroke's parent is the
 * cursor, which already had a child, so it becomes a SECOND child rather than
 * erasing the first. Nothing is discarded by an undo and nothing is discarded
 * by the change that follows one -- ctrl+z walks back to the fork and ctrl+y
 * comes forward down whichever branch was made most recently.
 *
 * MOST RECENT, WITH NO TIE TO BREAK, because sequences are unique and handed
 * out in the order things actually happened. "Most recent" is `max(seq)` over
 * the children and needs no timestamp and no policy.
 *
 * THIS REPLACED A SEPARATE HIGH-WATER MARK. `m_redo_from` was a second
 * variable recording where the future used to reach, kept in step with the
 * cursor by hand and cleared by every new change -- which is how it expressed
 * "the untaken future is gone", the one thing a tree does not have to say.
 * Now "is there anything to redo" is "does the cursor have children", asked of
 * the structure rather than of a variable beside it.
 *
 * SNAPSHOTS ARE STEPPED OVER, not stopped on. A keyframe is an entry in the
 * chain but not a thing anybody did, so landing on one would make ctrl+z
 * sometimes do nothing visible -- which reads as a broken key, not as a
 * subtlety. Both directions skip to the nearest entry that MARKED something.
 */
    bool Undo()
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (refuse_read_only("undo")) return false;
        // An open box's edit is a step like any other: ended, and so recorded,
        // before the undo that may take it back.
        if (m_text_sel) SelectTextBox(0);
        sealOpenOp();
        if (shared()) return retract(PaintOpKind::Undo);
        // NOT "0 means the head". Every append sets the cursor, so zero is
        // genuinely "before anything" -- and reading it as the head would make
        // an undo on a fully wound-back document leap to the top and undo the
        // newest entry instead of answering that there is nothing left.
        const uint64_t cur = m_cursor;

        // The newest marking entry at or above the cursor, walking parents --
        // stepping over any keyframes sitting between here and it.
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(cur, chain);
        const PaintOp* mark = nullptr;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if ((*it)->marks()) { mark = *it; break; }

        if (!mark) { ETCS_LOG("PaintDocument", "nothing to undo"); return false; }
        setCursor(mark->parent);
        return replayTo(m_cursor, "undo");
    }

    bool Redo()    { return redoAlong(0); }
    /*
     * THE OTHER WAY FORWARD. Undo has one direction; redo has as many as
     * there are branches under the cursor, and one of them is chosen for it
     * -- the most recent. Drawing after an undo makes a second child, and
     * from then on the first is a picture nothing can reach: redo takes the
     * newer, and the older sits in the tree with no key that names it. This
     * takes the second-newest, which is the one that was the future before
     * the last stroke was. Shown only when it exists (redoBranches).
     */
    bool RedoAlt() { return redoAlong(1); }

    // The first marking entry down each branch under the cursor, newest
    // branch first: what redo and its alternative step to.
    void redoTargets(std::vector<uint64_t>& out) const
    {
        out.clear();
        std::vector<const PaintOp*> kids, next;
        m_book.ChildrenOf(m_cursor, kids);
        for (const PaintOp* kid : kids)
        {
            // Down the most-recent child each time, until something that
            // marked lands under us. A run of keyframes has one child each,
            // so this is one step in every ordinary case.
            const PaintOp* walk = kid;
            for (size_t guard = 0; walk && guard <= m_book.size(); ++guard)
            {
                if (walk->marks()) { out.push_back(walk->seq); break; }
                m_book.ChildrenOf(walk->seq, next);
                walk = next.empty() ? nullptr : next.front();
            }
        }
    }
    /*
     * WHETHER THE HISTORY FORKS WHERE IT STANDS, published on every move of the
     * cursor -- and every entry moves it -- so a reader on another thread (the
     * frame edge, PaintInput's redo-alt control) asks an atomic and never the
     * notebook the input thread is appending to. Decided here, where the move
     * happens, rather than by whichever pane's input made it: a key goes to the
     * input of the pane it landed on, and only one of them holds the control.
     */
    void setCursor(uint64_t c)
    {
        m_cursor = c;
        m_forked.store(redoBranches() >= 2 ? 1 : 0, std::memory_order_release);
    }
    int forked() const { return m_forked.load(std::memory_order_acquire); }

    size_t redoBranches() const
    {
        if (shared()) return 0;     // a session's path does not fork (retract)
        std::vector<uint64_t> t;
        redoTargets(t);
        return t.size();
    }

    bool redoAlong(size_t branch)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (refuse_read_only("redo")) return false;
        if (m_text_sel) SelectTextBox(0);
        sealOpenOp();
        if (shared()) return branch == 0 ? retract(PaintOpKind::Redo) : false;
        std::vector<uint64_t> targets;
        redoTargets(targets);
        if (branch >= targets.size())
        {
            ETCS_LOG("PaintDocument", (branch ? "no other way forward from here" : "nothing to redo"));
            return false;
        }
        setCursor(targets[branch]);
        return replayTo(m_cursor, branch ? "redo (other branch)" : "redo");
    }

    // Steps available each way, for the layer panel's readout and the two work
    // functions that print them. Ancestors that marked, and the marking entries
    // reachable forward down the most-recent branch.
    size_t undoDepth() const
    {
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        size_t n = 0;
        for (const PaintOp* o : chain) if (o->marks()) ++n;
        return n;
    }

    size_t redoDepth() const
    {
        size_t n = 0;
        uint64_t walk = m_cursor;
        std::vector<const PaintOp*> kids;
        for (size_t guard = 0; guard <= m_book.size(); ++guard)
        {
            m_book.ChildrenOf(walk, kids);
            if (kids.empty()) break;
            if (kids.front()->marks()) ++n;
            walk = kids.front()->seq;
        }
        return n;
    }

    // How many branches fork off the cursor, which is the one thing about the
    // tree a person can otherwise only discover by pressing redo and being
    // surprised. The panel can say "2 futures" with this.
    size_t branchesHere() const
    {
        std::vector<const PaintOp*> kids;
        m_book.ChildrenOf(m_cursor, kids);
        return kids.size();
    }

    /*
 * REPLAY ONE ENTRY ONTO THIS DOCUMENT. The viewer's whole job, and the second
 * half of undo's.
 *
 * Deliberately NOT a second implementation of any mark: every branch here ends
 * in the same PaintLayer primitive the live path calls, with the brush the
 * entry carries. A replayed stroke that drew itself differently from the one
 * it is replaying would be a test of the wrong thing and a viewer showing a
 * different picture.
 */
    // One point of a stroke's entry, landed: a dab at it, or a smear carried
    // to it from the point before. What StrokeTo does as the point arrives and
    // what ApplyOp does for every point of a sealed entry.
    static void apply_step(PaintLayer* layer, const PaintOp& op, size_t i)
    {
        const int32_t x = op.pts[i * 2], y = op.pts[i * 2 + 1];
        if (op.kind == PaintOpKind::Smudge)
        {
            if (i == 0) return;                // the first point is where the carry starts
            layer->SmudgeDab(op.pts[i * 2 - 2], op.pts[i * 2 - 1], x, y,
                             op.brush, PaintInput_SMUDGE_STRENGTH);
            return;
        }
        layer->DrawBrush(x, y, op.brush);
    }

    bool ApplyOp(const PaintOp& op)
    {
        // A box names no layer; neither does a page, a hand on a box (absorbed
        // by absorb_hold before this is reached, never a mark on a picture),
        // nor a retraction, which re-derives the path (AcceptOp) rather than
        // drawing anything.
        if (op.kind == PaintOpKind::Text) { apply_text_state(op.box, op.removed); return true; }
        if (op.kind == PaintOpKind::Page || op.kind == PaintOpKind::Hold || op.kind == PaintOpKind::Free
         || op.retraction()) return true;
        PaintLayer* layer = layerFor(op);
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "replay: no layer for entry " << op.seq
                     << " (key " << op.layer << ", order " << op.order
                     << ") -- dropped.");
            return false;
        }

        // UNDER THE ENTRY'S OWN CLIP, not this page's selection: the mark
        // lands where it was allowed to when it was made (PaintOp::Clip),
        // and anywhere when it carried none. The layer's binding is put back
        // after, since the active layer's is this page's live selection.
        struct Clipped
        {
            PaintLayer* layer; const PaintSelection* was; PaintSelection sel; bool on = false;
            Clipped(PaintLayer* l, const PaintOp& o, uint32_t w, uint32_t h) : layer(l), was(l->clip())
            {
                sel.Reset(w, h);
                if (!o.clip.empty())
                    for (uint32_t y = 0; y < o.clip.h; ++y)
                        for (uint32_t x = 0; x < o.clip.w; ++x)
                            if (o.clip.mask[static_cast<size_t>(y) * o.clip.w + x])
                                sel.Set(o.clip.x0 + static_cast<int32_t>(x), o.clip.y0 + static_cast<int32_t>(y));
                on = !sel.empty();
                layer->BindClip(on ? &sel : nullptr);
            }
            ~Clipped() { layer->BindClip(was); }
        } clipped(layer, op, m_width, m_height);

        switch (op.kind)
        {
        case PaintOpKind::Snapshot:
            if (!layer->RestoreBytes(op.bytes))
            {
                ETCS_LOG("PaintDocument", "replay: entry " << op.seq
                         << " is " << op.bytes.size() << " bytes for a layer that is not that size"
                         << " -- dropped.");
                return false;
            }
            return true;

        case PaintOpKind::Dab:
        case PaintOpKind::Smudge:
            for (size_t i = 0; i < op.points(); ++i) apply_step(layer, op, i);
            return true;

        case PaintOpKind::Clear:
            layer->Clear(op.brush.color.r, op.brush.color.g, op.brush.color.b, op.brush.color.a);
            return true;

        case PaintOpKind::Patch:
            if (op.points() < 1) return false;
            if (!layer->WriteRect(op.pts[0], op.pts[1], op.w, op.h, op.bytes))
            {
                ETCS_LOG("PaintDocument", "replay: entry " << op.seq << " patches "
                         << op.w << "x" << op.h << " with " << op.bytes.size() << " bytes -- dropped.");
                return false;
            }
            return true;

        case PaintOpKind::Line:
            if (op.points() < 2) return false;
            layer->StrokeLine(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Rect:
            if (op.points() < 2) return false;
            layer->DrawRectOutline(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Ellipse:
            if (op.points() < 2) return false;
            layer->DrawEllipseOutline(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Poly:
        {
            const size_t n = op.points();
            if (n < 2) return false;
            for (size_t i = 0; i < n; ++i)
            {
                const size_t j = (i + 1) % n;
                layer->StrokeLine(op.pts[i * 2], op.pts[i * 2 + 1],
                                  op.pts[j * 2], op.pts[j * 2 + 1], op.brush);
            }
            return true;
        }

        case PaintOpKind::Page:          // all answered above
        case PaintOpKind::Text:
        case PaintOpKind::Undo:
        case PaintOpKind::Redo:
        case PaintOpKind::Hold:
        case PaintOpKind::Free:
            return true;

        case PaintOpKind::Layers:
            // The roster half is applied by reconcileLayers, before any raster.
            // What is left here is the raster half, which only a merge has.
            if (op.bytes.empty()) return true;
            if (!layer->RestoreBytes(op.bytes))
            {
                ETCS_LOG("PaintDocument", "replay: entry " << op.seq
                         << " carries bytes for a layer that is not that size -- dropped.");
                return false;
            }
            return true;

        case PaintOpKind::Fill:
        {
            if (op.points() < 1) return false;
            const PaintColor ink = (op.brush.blend == PaintBlendMode::Erase)
                                   ? PaintColor{ 0.0f, 0.0f, 0.0f, 0.0f }
                                   : op.brush.color;
            layer->FloodFill(op.pts[0], op.pts[1], ink, op.tolerance);
            return true;
        }
        }
        return false;
    }

    // Take an entry somebody else made -- a host's push, a file being reopened
    // -- into this notebook AND onto the picture. The one door for arriving
    // history, so a viewer and a reload are the same code path.
    bool AcceptOp(PaintOp op)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        op.sent = true;                   // it came from the record; it is the record's
        op.confirmed = true;
        /*
         * THE ROOM FIRST. Anything drawn here that the record does not hold
         * yet -- a pending entry, a stroke still being made, an open box --
         * was drawn on a picture the room is now changing ahead of it. So the
         * page steps back to its last confirmed entry, takes the room's
         * entries in the record's order, and puts its own back after them
         * (rewind, then finish_rewind at the end of the read): every member
         * applies the same entries in the same order, the one who made them
         * included, and nobody waits for a round trip to see their own mark.
         */
        if (!m_rewound && has_pending()) rewind("the room's entries came first");
        if (m_rewound)
        {
            if (op.kind == PaintOpKind::Page) { become_page(op); return true; }
            setCursor(m_book.Append(std::move(op), m_cursor));
            return true;                  // drawn by the replay at the end of the read
        }
        /*
     * A PAGE ENTRY REPLACES THE DOCUMENT rather than adding to it: the size,
     * the stack and nothing on it, history and text boxes gone. The keyframes
     * that follow it in the same baseline fill the layers. Not appended -- it
     * is where this notebook now starts.
     */
        if (op.kind == PaintOpKind::Page)
        {
            become_page(op);
            return true;
        }
        /*
         * A RETRACTION IS APPLIED BY REPLAY. It names an entry already on this
         * path; appending it and re-deriving the path is what takes the entry
         * out of the picture, on every member alike (PaintNotebook::
         * EffectivePath). Nothing to draw, so nothing goes through ApplyOp.
         */
        if (op.retraction())
        {
            setCursor(m_book.Append(std::move(op), m_cursor));
            return replayTo(m_cursor, "retraction");
        }
        /*
         * A KEYFRAME OF OUR OWN, when this layer is due one, BEFORE the mark
         * lands -- the same rule RememberOp keeps for a mark made here. The
         * record carries no keyframes past the baseline (ExportOps: a writer's
         * whole-layer snapshot was overwriting whatever the others had drawn
         * on that layer since it was taken), so a member keeps its own, of the
         * picture as IT has derived it, and a replay stays bounded.
         */
        if (op.marks() && shared())
            if (PaintLayer* l = layerFor(op))
                if (m_book.snapshotDue(keyOf(l))) appendSnapshot(l);
        // A change to the STACK made elsewhere has to change this stack too;
        // the raster half of the entry (a merge's) lands on the result.
        if (op.structural()) reconcileLayers(&op);
        const bool ok = ApplyOp(op);
        setCursor(m_book.Append(std::move(op), m_cursor));
        Touch();
        return ok;
    }

    bool shared() const { return !m_author.empty(); }

    /*
     * THE NODE REFUSED: a push came back READ ONLY (or not at all). What was
     * drawn here and not confirmed is not in the picture everybody else has,
     * so it comes off this one too: back to the last confirmed entry, and the
     * record's picture from there. The remote decides who draws; this page
     * draws first and is corrected, rather than guessing and refusing.
     */
    bool RevertPending()
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        m_stated.clear();                   // a page not taken is not coming back
        m_stated_left = 0;
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        size_t n = 0;
        for (const PaintOp* o : chain) if (pending(o)) ++n;
        if (!n) return false;
        rewind("refused by the room");
        m_stash.clear();
        finish_rewind();
        ETCS_LOG("PaintDocument", "the room did not take " << n << " entr(ies) of this page's -- taken off here too.");
        return true;
    }

private:
    bool pending(const PaintOp* o) const
    {
        return !m_author.empty() && o->author == m_author && !o->confirmed && o->kind != PaintOpKind::Snapshot;
    }
    bool has_pending() const
    {
        if (m_open_live || m_text_sel) return true;
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        for (const PaintOp* o : chain) if (pending(o)) return true;
        return false;
    }

    /*
     * STEP BACK TO THE LAST CONFIRMED ENTRY. What is in the air (an open
     * stroke, an open box) is held; every pending entry is set aside in order;
     * the confirmed entries after the first pending one -- the room's, taken
     * while this page's were in flight -- are kept, since the record put them
     * first; this page's own keyframes past that point are dropped, being of a
     * picture drawn in the wrong order. The picture is left as it was until
     * finish_rewind replays the path once, whatever arrived meanwhile.
     */
    void rewind(const char* why)
    {
        if (m_rewound) return;
        m_held_stroke = m_open_live;
        if (m_open_live) { m_held_open = m_open; m_open = PaintOp{}; m_open_live = false; }
        m_held_box = false;
        if (m_text_sel)
            if (const PaintTextBox* b = FindTextBox(m_text_sel))
            {
                m_held_box = true;
                m_held_text = *b;
                m_held_before = m_text_before;
                m_held_fresh = m_text_fresh;
            }

        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        size_t i0 = chain.size();
        for (size_t i = 0; i < chain.size(); ++i) if (pending(chain[i])) { i0 = i; break; }
        m_stash.clear();
        std::vector<PaintOp> keep;
        for (size_t i = i0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (o->kind == PaintOpKind::Snapshot) continue;
            if (pending(o)) m_stash.push_back(*o);
            else            keep.push_back(*o);
        }
        setCursor(i0 < chain.size() ? chain[i0]->parent : m_cursor);
        for (PaintOp& k : keep) setCursor(m_book.Append(std::move(k), m_cursor));
        m_rewound = true;
        ETCS_LOG("PaintDocument", "rewound (" << why << "): " << m_stash.size()
                 << " of this page's entr(ies) set aside" << (m_held_stroke ? ", a stroke in the air" : "")
                 << (m_held_box ? ", an open box" : "") << ".");
    }

    /*
     * A LINE OF OURS, READ BACK: the oldest pending entry it matches (by its
     * wire form) is confirmed where the record put it. Pending entries before
     * the match were refused by the node (a box somebody else held) and are
     * dropped -- which takes a rewind, since they are drawn.
     */
    bool confirm_own(const std::string& rest)
    {
        if (!m_rewound)
        {
            std::vector<const PaintOp*> chain;
            m_book.ChainTo(m_cursor, chain);
            std::vector<PaintOp*> waiting;
            for (const PaintOp* o : chain) if (pending(o) && o->sent) waiting.push_back(const_cast<PaintOp*>(o));
            if (waiting.empty()) return false;
            if (waiting.front()->wire == rest)
            {
                waiting.front()->confirmed = true;
                return true;
            }
            bool later = false;
            for (PaintOp* w : waiting) if (w->wire == rest) later = true;
            if (!later) return false;
            rewind("the room refused part of a push");
        }
        for (size_t k = 0; k < m_stash.size(); ++k)
        {
            if (!m_stash[k].sent || m_stash[k].wire != rest) continue;
            if (k) ETCS_LOG("PaintDocument", k << " entr(ies) of this page's were refused by the room -- dropped.");
            PaintOp e = std::move(m_stash[k]);
            e.confirmed = true;
            m_stash.erase(m_stash.begin(), m_stash.begin() + static_cast<std::ptrdiff_t>(k) + 1);
            setCursor(m_book.Append(std::move(e), m_cursor));
            return true;
        }
        return false;
    }

    // Put this page's own back after the room's, replay the path once, and
    // put back what was in the air.
    void finish_rewind()
    {
        if (!m_rewound) return;
        for (PaintOp& p : m_stash) setCursor(m_book.Append(std::move(p), m_cursor));
        m_stash.clear();
        m_rewound = false;
        replayTo(m_cursor, "record order");
        if (m_held_stroke)
        {
            m_open = m_held_open;
            m_open_live = true;
            if (PaintLayer* l = layerFor(m_open))
                for (size_t i = 0; i < m_open.points(); ++i) apply_step(l, m_open, i);
            m_held_stroke = false;
        }
        if (m_held_box)
        {
            PaintTextBox* at = nullptr;
            for (auto& t : m_text) if (t.key == m_held_text.key) at = &t;
            if (at) { const uint32_t id = at->id; *at = m_held_text; at->id = id; }
            else    { m_text.push_back(m_held_text); m_text.back().id = ++m_text_seq; at = &m_text.back(); }
            m_text_sel    = at->id;
            m_text_before = m_held_before;
            m_text_fresh  = m_held_fresh;
            m_held_box = false;
        }
        Touch();
    }
public:

    /*
     * THE UNDO EDGE (PaintOpKind::Undo). Names this page's newest entry that
     * still stands -- its own, never anybody else's: in a room a stroke is its
     * author's to take back, and an undo that reached across authors would
     * have every writer's ctrl+z erasing whoever drew last. Appended to the
     * path like an arriving one and applied the same way, so what this page
     * shows after its own undo is exactly what the others will show after
     * reading it. Redo names the newest of this page's entries that IS
     * retracted, and puts it back.
     */
    bool retract(PaintOpKind kind)
    {
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        const bool undo = (kind == PaintOpKind::Undo);
        const uint32_t k = PaintNotebook::Newest(chain, m_author, !undo);
        if (k == 0)
        {
            ETCS_LOG("PaintDocument", (undo ? "nothing of yours to undo" : "nothing of yours to redo"));
            return false;
        }
        PaintOp edge;
        edge.kind    = kind;
        edge.author  = m_author;
        edge.target  = m_author;
        edge.ordinal = k;
        setCursor(m_book.Append(std::move(edge), m_cursor));
        Touch();
        return replayTo(m_cursor, undo ? "undo" : "redo");
    }

    /*
 * ── VIEW ONLY ────────────────────────────────────────────────────────────
 *
 * A page that shows a picture and takes no edits (a viewer, a kiosk). A
 * lowercase state flag on the document; every verb that edits refuses while
 * it is up (refuse_read_only). What ARRIVES is not an edit made here --
 * AcceptOp works below these verbs -- so a room's changes still land. A
 * shared session does NOT raise it: a reader draws, the node refuses the push,
 * and RevertPending takes the marks off, so the node stays the one judge.
 */
    void SetReadOnly(bool on)
    {
        if (on) this->addTag("readonly");
        else    this->removeTag(ETCS::Buffer("readonly"));
        const char* said = on ? "view only -- this page follows the session and takes no edits."
                              : "editable.";
        ETCS_LOG("PaintDocument", said);
    }
    bool readOnly() const { return const_cast<PaintDocument*>(this)->hasTag(ETCS::Buffer("readonly")); }

    /*
 * ── OUT OF STEP ──────────────────────────────────────────────────────────
 *
 * The page found this picture is not the room's and is reading the record
 * again from its start. Until that lands the canvas starts no stroke -- ink
 * put down on a picture about to be replaced is ink drawn against the wrong
 * one -- and `syncing` is up on the document for whatever shows the wait
 * (the throbber watches it, boot_paint_panels.etcs). What is already pending
 * survives the read (rewind), so nothing made before the pause is lost.
 */
    void Syncing(bool on)
    {
        if (on == syncing()) return;
        if (on) this->addTag("syncing");
        else    this->removeTag(ETCS::Buffer("syncing"));
        ETCS_LOG("PaintDocument", (on ? "out of step with the room -- reading the record again."
                                      : "in step with the room again."));
    }
    bool syncing() const { return const_cast<PaintDocument*>(this)->hasTag(ETCS::Buffer("syncing")); }

    /*
 * THE WHOLE PAGE AGAIN, with the next push (ExportOps' baseline): what the
 * owner does when a member says reading the record from its start did not
 * put it right -- a line it cannot read, or a picture that differs after it.
 * The record is replayed from a page nobody has to derive.
 */
    void Restate()
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        m_restate = true;
        ETCS_LOG("PaintDocument", "a member cannot rebuild this page from the record -- it goes to the room whole.");
    }

    uint64_t notebookHead() const { return m_book.head(); }

    void RenderToSurface(ETCS::RID target, int32_t x, int32_t y, float zoom = 1.0f)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* layer : stack)
        {
            if (!layer->onScreen()) continue;   // a peek draws a hidden layer -- see PaintLayer::SetPeek
            // 1.0, NOT layer->opacity(): BlitTo multiplies by m_opacity itself
            // (see its alpha), so passing it here drew every layer at opacity
            // SQUARED -- a layer set to 50% showed at 25% on screen while the
            // export, which composites once (CompositeVisible), showed it at
            // 50%. The picture and the file disagreed about the same number.
            layer->BlitTo(target, x, y, 0, 0, 1.0f, zoom);
            // The lifted pixels are the active layer's, so they are drawn at
            // its depth -- over it, under whatever is stacked above it -- which
            // is where they will be once dropped.
            if (layer == m_active_layer && m_sel.lifted()) draw_lift(target, x, y, zoom, layer);
        }

        // Over the layers, because a text box is not in any of them -- it is
        // still text and is drawn from its string every frame. See PaintTextBox.
        draw_text_boxes(target, x, y, zoom);
        // And over everything, because an outline is chrome: it says where the
        // region is and must not be hidden by a layer above the one it is on.
        draw_selection(surface, x, y, zoom);
    }

    /*
 * ── raw images in and out ────────────────────────────────────────────────
 *
 * BY PATH, and the page and the terminal call the same three verbs: see the
 * PAM note above PaintImage for why a path is the one interface the browser
 * and the desktop share.
 *
 * IMPORT IS A NEW LAYER, made the way the boot script makes one. addTag<> is
 * what `doc.spawn(PaintProvider::PaintLayer)` reduces to -- _make_child_
 * PaintLayer is `parent->addTag<PaintLayer>()` (ETCS_API.h) -- so the layer is
 * this document's typed child like every other, and membership is parenthood
 * with nothing to keep in step (OrderedLayers). A module may not spawn ANOTHER
 * module's type (PaintLayerPanel's note); its own it spawns exactly this way,
 * as Shell and ChessNode do.
 *
 * SIZED TO THE IMAGE, not the page: a layer's raster is its own (PaintLayer::
 * Create), the blit clips at the page edge, and resampling on the way in would
 * throw away pixels the user may yet pan into view. On top and active, because
 * what was just brought in is what is about to be worked on.
 *
 * RECORDED (import_layer): undo takes the layer away again, and a session
 * gets the picture on it.
 */
    bool ImportImage(const std::string& path)
    {
        if (refuse_read_only("import")) return false;
        PaintImage img;
        std::string why;
        if (!paint_image_read(path, img, why))
        {
            ETCS_LOG("PaintDocument", "import " << path << ": " << why);
            return false;
        }
        return import_layer(img, path);
    }

    /*
 * THE IMAGE AS THE PAGE: a new canvas the image's size (New), and the image
 * on it as a layer above the paper. A layer rather than the paper's own
 * pixels, so a picture with transparency keeps it and "undo the import" is
 * still the row's delete; the page's size is what changes, which is what
 * "open this picture" means as opposed to "add it to the one I have"
 * (ImportImage). Both are offered when a file comes in
 * (PaintCanvasMenu::OfferImport).
 */
    bool ImportCanvas(const std::string& path)
    {
        if (refuse_read_only("open as canvas")) return false;
        PaintImage img;
        std::string why;
        if (!paint_image_read(path, img, why))
        {
            ETCS_LOG("PaintDocument", "import " << path << ": " << why);
            return false;
        }
        if (!New(img.w, img.h)) return false;
        return import_layer(img, path);
    }

private:
    bool import_layer(const PaintImage& img, const std::string& path)
    {
        /*
     * PAGE-SIZED, WITH THE IMAGE DROPPED INTO IT -- not a raster the size of
     * the file.
     *
     * A layer used to be created at the image's extent, on the reasoning that
     * a layer's raster is its own and resampling on the way in would throw
     * pixels away. What that actually bought was a layer whose pixels were
     * second class: every selection operation works in DOCUMENT coordinates
     * and lands through the layer's own raster, so lifting the image and
     * moving it wrote the pixels back outside the raster's bounds, where
     * DropPixels clips -- and the picture vanished. Fill, smudge and paste had
     * the same edge.
     *
     * Nothing is thrown away that was ever going to be shown: the composite
     * clips to the page (CompositeVisible), so anything outside it was already
     * invisible. The path for "keep all of it" is the other answer to the
     * import prompt -- ImportCanvas makes the PAGE the image's size first, and
     * then page-sized is exactly the image.
     */
        std::vector<uint8_t> bytes(static_cast<size_t>(m_width) * m_height * 4, 0);
        paint_composite_raw_scaled_bytes(bytes.data(), m_width, m_height, m_width * 4,
                                         img.rgba.data(), img.w, img.h,
                                         0, 0, img.w, img.h, 1.0f);

        /*
         * RECORDED AND PERFORMED like a merge: the stack before, then one entry
         * -- the stack with the new face on top, carrying its pixels -- which
         * reconcile makes the layer from and the raster half fills, here and on
         * every member. Undo takes the layer away; a session gets the picture.
         */
        if (m_sel.lifted()) DropSelection();
        recordStructure("import");
        PaintOp op = rosterNow();
        int32_t top = 0;
        for (const PaintOp::Face& f : op.roster) top = std::max(top, f.order + 1);
        const uint64_t key = newKey();
        op.roster.push_back(PaintOp::Face{ top, 1.0f, true, paint_path_stem(path), key });
        op.layer = key;
        op.order = top;
        op.w = m_width;
        op.h = m_height;
        op.bytes = std::move(bytes);
        PerformStack(std::move(op));
        PaintLayer* layer = layerByKey(key);
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "import " << path << ": could not spawn a layer under '"
                     << m_name << "'.");
            return false;
        }
        m_active_layer = layer;
        ETCS_LOG("PaintDocument", "imported " << path << " " << img.w << "x" << img.h
                 << " -> layer '" << layer->name() << "' RID:" << layer->getRID()
                 << " order=" << top << ", active, on a " << m_width << "x" << m_height
                 << " raster"
                 << ((img.w > m_width || img.h > m_height)
                     ? " (clipped to the page -- 'new canvas' keeps all of it)" : ""));
        return true;
    }
public:

    /*
 * THE COMPOSITE IS WHAT RenderToSurface SHOWS, less the view: the visible
 * layers bottom first, each at its own opacity, and a lift in flight at the
 * active layer's depth -- the same walk, through the same blend
 * (paint_composite_raw_scaled_bytes is what BlitTo and draw_lift land through
 * on a host-backed view; 1:1 here, since a file has no zoom). Onto a
 * transparent page, so a picture with no paper layer leaves with its alpha
 * rather than over a colour nobody asked for.
 *
 * NOT THE DIM: that is a hover (SetDim) and a file is precisely the trace a
 * hover must not leave. NOT THE TEXT BOXES: they are strings drawn through a
 * Glyphs target by RID (draw_text_boxes) and a buffer has no RID; the export
 * log counts them so a file that lost its captions says so. Not the selection
 * outline, which is chrome.
 */
    bool CompositeVisible(std::vector<uint8_t>& out, size_t* shown = nullptr) const
    {
        if (m_width == 0 || m_height == 0) return false;
        out.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        size_t n = 0;
        for (auto* layer : stack)
        {
            if (!layer->visible()) continue;
            ++n;
            paint_composite_raw_scaled_bytes(out.data(), m_width, m_height, m_width * 4,
                                             layer->PixelData(), layer->width(), layer->height(),
                                             0, 0, layer->width(), layer->height(),
                                             layer->opacity());
            if (layer == m_active_layer && m_sel.lifted())
                paint_composite_raw_scaled_bytes(out.data(), m_width, m_height, m_width * 4,
                                                 m_sel.lift.data(), m_sel.width(), m_sel.height(),
                                                 m_sel.x0 + m_sel.dx, m_sel.y0 + m_sel.dy,
                                                 m_sel.width(), m_sel.height(),
                                                 layer->opacity());
        }
        if (shown) *shown = n;
        return true;
    }

    /*
 * ONE PIXEL OF WHAT IS SEEN, for the eyedropper. The same walk as
 * CompositeVisible, bottom to top through the visible layers with each one's
 * opacity, over transparent -- and not a call to it, because compositing the
 * whole page to read one pixel is a frame's worth of work for a click.
 * Straight source-over on straight RGBA, which is what the layers hold.
 */
    bool SampleAt(int32_t x, int32_t y, float rgba[4]) const
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= m_width
            || static_cast<uint32_t>(y) >= m_height) return false;
        float dr = 0, dg = 0, db = 0, da = 0;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* layer : stack)
        {
            if (!layer->visible()) continue;
            const uint8_t* px = layer->PixelData();
            if (!px || static_cast<uint32_t>(x) >= layer->width()
                || static_cast<uint32_t>(y) >= layer->height()) continue;
            const uint8_t* p = px + (static_cast<size_t>(y) * layer->width() + x) * 4;
            const float sa = (p[3] / 255.0f) * layer->opacity();
            if (sa <= 0.0f) continue;
            const float sr = p[0] / 255.0f, sg = p[1] / 255.0f, sb = p[2] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa > 0.0f)
            {
                dr = (sr * sa + dr * da * (1.0f - sa)) / oa;
                dg = (sg * sa + dg * da * (1.0f - sa)) / oa;
                db = (sb * sa + db * da * (1.0f - sa)) / oa;
            }
            da = oa;
        }
        rgba[0] = dr; rgba[1] = dg; rgba[2] = db; rgba[3] = da;
        return true;
    }

    bool ExportImage(const std::string& path)
    {
        std::vector<uint8_t> px;
        size_t shown = 0;
        if (!CompositeVisible(px, &shown))
        {
            ETCS_LOG("PaintDocument", "export " << path << ": '" << m_name << "' has no extent.");
            return false;
        }
        std::string why;
        if (!paint_image_write(path, px.data(), m_width, m_height, why))
        {
            ETCS_LOG("PaintDocument", "export " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported " << path << " " << m_width << "x" << m_height
                 << " -> " << path << ", " << shown << " visible layer(s)"
                 << (m_text.empty() ? std::string()
                                    : "; " + std::to_string(m_text.size())
                                      + " text box(es) are not in it"));
        return true;
    }

    // The active layer's own bytes, alpha and all, at its own size -- what
    // SnapshotBytes already hands the history.
    bool ExportLayer(const std::string& path)
    {
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": no active layer.");
            return false;
        }
        std::vector<uint8_t> px;
        if (!m_active_layer->SnapshotBytes(px))
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": '" << m_active_layer->name()
                     << "' has no pixels -- Create it first.");
            return false;
        }
        std::string why;
        if (!paint_image_write(path, px.data(), m_active_layer->width(), m_active_layer->height(), why))
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported layer '" << m_active_layer->name() << "' RID:"
                 << m_active_layer->getRID() << " " << m_active_layer->width() << "x"
                 << m_active_layer->height() << " -> " << path);
        return true;
    }

    /*
 * ── re-stating the page ──────────────────────────────────────────────────
 *
 * ANCHORED, NOT SCALED. A resize here is a change of EXTENT: the picture keeps
 * its pixels and the page grows or shrinks around them, and the anchor says
 * which part of the page stays where it is -- a 3x3 grid, 0 the top-left
 * corner, 4 the centre, 8 the bottom-right, read as (anchor % 3, anchor / 3).
 * Growing from the centre puts new room on every side; growing from the
 * top-left puts it all on the right and the bottom. Resampling is a different
 * operation with a different name and does not exist here yet.
 *
 * EVERY LAYER TAKES THE PAGE'S SIZE, translated by the page's own shift. A layer
 * is allowed its own raster (an import is sized to its image, ImportImage), but
 * once the page is re-stated the honest answer to "how big is this layer" is the
 * page: what an oversize layer kept beyond the old edge was already outside the
 * composite (CompositeVisible clips to the page), and keeping nine separate
 * extents in step across a shift is bookkeeping nobody can see the result of.
 *
 * THE PAPER'S NEW AREA IS PAPER, everything else's is transparent. Which layer
 * is the paper is answered by ground_layer; the colour is the convention the
 * page boots with (boot_paint_panels.etcs: `paper.Clear(1.0, 1.0, 1.0, 1.0)`).
 *
 * NOT UNDOABLE, AND THE HISTORY IS DROPPED RATHER THAN LEFT TO LIE. The store
 * is three whole-layer snapshots of the ACTIVE layer at its current size
 * (Remember), restored only into a buffer of the same size (RestoreBytes is
 * size-checked) -- so no entry it can hold brings back the old extent, and an
 * entry kept across a resize answers every later Undo with "layer size changed
 * -- entry dropped" one press at a time. Clearing them says the same thing
 * once. A resize becomes a step when the history is the replayed event stream
 * the note above Remember describes.
 *
 * The selection and the text boxes go with the pixels, since they are places
 * IN the picture; a lift in flight lands first, on the page it was cut from.
 */
    bool Resize(uint32_t w, uint32_t h, int anchor)
    {
        if (refuse_read_only("resize")) return false;
        Touch();
        if (!extent_ok(w, h, "resize")) return false;
        if (m_sel.lifted()) DropSelection();

        const int32_t dx = anchor_shift(anchor % 3, m_width, w);
        const int32_t dy = anchor_shift(anchor / 3, m_height, h);

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintLayer* paper = ground_layer(stack);
        for (auto* l : stack) l->Rebase(w, h, dx, dy, (l == paper) ? PAPER_CLEAR : nullptr);

        // The mask re-stated in the new page's space, pixel for pixel with the
        // layers it selects on.
        if (!m_sel.empty())
        {
            PaintSelection moved;
            moved.Reset(w, h);
            for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
                for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
                    if (m_sel.at(x, y)) moved.Set(x + dx, y + dy);
            m_sel = std::move(moved);
        }
        else m_sel.Reset(w, h);
        for (PaintTextBox& b : m_text) { b.x += dx; b.y += dy; }

        ClearHistory();
        m_page_changed = true;
        ETCS_LOG("PaintDocument", "'" << m_name << "' " << m_width << "x" << m_height
                 << " -> " << w << "x" << h << " anchored at " << anchor
                 << " (pixels moved by " << dx << "," << dy << "), " << stack.size()
                 << " layer(s)" << (paper ? ", paper '" + paper->name() + "' extended" : "")
                 << "; history dropped -- a resize re-states every raster, so no entry taken before it describes a layer that is still that size.");
        m_width = w;
        m_height = h;
        return true;
    }

    /*
 * A NEW CANVAS: the same layers, the new extent, and nothing on them -- paper
 * to its clear colour, every other layer transparent. The layers stay rather
 * than being removed and re-spawned, because their names, order and the rows a
 * layer window has bound to them are the user's arrangement, not the picture.
 * The text boxes are the picture, so they go; the clipboard is not, so it
 * stays. Not undoable, for the reason Resize gives.
 */
    bool New(uint32_t w, uint32_t h)
    {
        if (refuse_read_only("new")) return false;
        if (!renew(w, h)) return false;
        m_page_changed = true;
        return true;
    }

    // New without the view-only guard: what a session's Page entry does to a
    // follower (become_page), which is the room's change and not an edit here.
    bool renew(uint32_t w, uint32_t h)
    {
        Touch();
        if (!extent_ok(w, h, "new")) return false;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintLayer* paper = ground_layer(stack);
        for (auto* l : stack)
        {
            l->Allocate(w, h);          // idempotent at the same size: Clear is what empties it
            if (l == paper) l->Clear(PAPER_CLEAR[0], PAPER_CLEAR[1], PAPER_CLEAR[2], PAPER_CLEAR[3]);
            else            l->Clear(0.0f, 0.0f, 0.0f, 0.0f);
        }
        m_sel.Reset(w, h);               // the lift goes with it: there is nothing to land on
        m_text.clear();
        m_text_sel = 0;
        ClearHistory();
        m_width = w;
        m_height = h;
        ETCS_LOG("PaintDocument", "'" << m_name << "' new " << w << "x" << h << ", "
                 << stack.size() << " layer(s) cleared"
                 << (paper ? ", paper '" + paper->name() + "' to its clear colour" : ""));
        return true;
    }


    /*
 * ── becoming another page ────────────────────────────────────────────────
 *
 * The two halves a page store needs and nothing else here provides: every
 * layer GONE, and a layer MADE from bytes rather than from a file. Both are
 * the document's because both are statements about its children.
 *
 * DESTROYED, NOT DETACHED. RemoveLayer detaches so a script's name still
 * resolves; a page switch is the opposite case -- nobody holds these layers
 * and the next switch would leak another stack of rasters, 3 MB each at this
 * size. So each goes through the same DestroyEvent a Delete verb fires, keyed
 * the way SqliteLocalDatabase::DeleteConcrete keys its own, which runs the
 * destructor and returns the arena footprint. The RID is read before the
 * event because the pointer is not valid after it.
 *
 * NO HOLD ACROSS THIS: a DestroyEvent waits on the ordering thread, and the
 * ordering thread may be waiting on a lifetime hold -- the one deadlock the
 * mechanism can build (Entity.h, lifetime_hold_depth). A caller that has a
 * Held<> open reads what it needs, drops it, and only then calls here.
 *
 * The history goes with the layers: its entries name them by RID and a step
 * onto a dead one is dropped with a message, but a page that was never
 * painted on has nothing to undo TO, and saying so is more honest than an
 * entry that fails later (the same reason an import is not remembered). The
 * text boxes go too: they are content, and content that belongs to the page
 * being left cannot stay on the one arriving.
 */
    void DestroyLayers()
    {
        m_sel.Reset(m_width, m_height);              // the carry's layer is about to go, lift and all
        m_active_layer = nullptr;
        ClearHistory();
        m_page_changed = true;                       // another page is about to stand here
        m_text.clear();
        m_text_sel = 0;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const std::string key = l->getSourceModule().toString() + ":" + l->getSourceTag().toString();
            const ETCS::RID rid = l->getRID();
            if (!ETCS::DestroyEvent{key.c_str(), l}())
                ETCS_LOG("PaintDocument", "layer RID:" << rid << " refused to be destroyed -- detaching it instead.");
        }
        // Whatever refused is still a child; it must at least leave the stack.
        OrderedLayers(stack);
        for (PaintLayer* l : stack) l->detachFromParent();
        Touch();
    }

    // The import's shape (ImportImage) with the file taken out: a typed child,
    // sized to its bytes, with its own name, key, visibility and opacity, made
    // ACTIVE if asked. The caller keys it -- a stored page brings its own
    // dense order, so nothing is renumbered here.
    /*
 * A NEW, EMPTY LAYER ABOVE THE ACTIVE ONE, which is what the window's + does.
 *
 * ABOVE THE ACTIVE ONE rather than on top of everything, because "add a layer"
 * while working on layer 2 of 5 means "one to draw on next to this", and a
 * layer that always lands on top is one the user then has to drag back down.
 * Page-sized and transparent: a layer is a sheet over the picture, and its own
 * raster is only ever its own size when a file arrived at that size
 * (ImportImage).
 *
 * The name is the first "layer N" nobody is using, counting from the stack's
 * size, so adding and removing does not produce two layers with one name.
 */
    ETCS::RID NewLayer()
    {
        if (refuse_read_only("add a layer")) return 0;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        std::string name;
        for (size_t n = stack.size() + 1; ; ++n)
        {
            name = "layer " + std::to_string(n);
            bool taken = false;
            for (auto* l : stack) if (l->name() == name) { taken = true; break; }
            if (!taken) break;
        }
        // Straight above the active layer, counting depth from the bottom as
        // MoveLayerTo does; on top when nothing is active.
        int32_t depth = static_cast<int32_t>(stack.size());
        for (size_t i = 0; i < stack.size(); ++i)
            if (stack[i] == m_active_layer) depth = static_cast<int32_t>(i) + 1;

        /*
         * THE STACK WITH ONE MORE FACE, performed: reconcile makes the layer
         * (page-sized, transparent) because its key names nothing here --
         * exactly as it makes it on every member reading the entry.
         */
        if (m_sel.lifted()) DropSelection();
        recordStructure("new layer");
        PaintOp op = rosterNow();
        for (PaintOp::Face& f : op.roster) if (f.order >= depth) ++f.order;
        const uint64_t key = newKey();
        op.roster.push_back(PaintOp::Face{ depth, 1.0f, true, name, key });
        PerformStack(std::move(op));
        PaintLayer* layer = layerByKey(key);
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "could not spawn a layer under '" << m_name << "'.");
            return 0;
        }
        m_active_layer = layer;
        ETCS_LOG("PaintDocument", "layer '" << name << "' RID:" << layer->getRID()
                 << " added at depth " << depth << ", active");
        return layer->getRID();
    }

    PaintLayer* SpawnLayer(const PaintImage& img, const std::string& name, int32_t order,
                           bool visible, float opacity, bool active)
    {
        Quiet quiet(*this);          // a stored page's stack, not an edit to one
        PaintLayer* layer = this->addTag<PaintLayer>();
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "could not spawn layer '" << name << "' under '" << m_name << "'.");
            return nullptr;
        }
        layer->Create(img.w, img.h);
        layer->RestoreBytes(img.rgba);
        layer->SetName(name);
        layer->SetOrder(order);
        layer->SetVisible(visible);
        layer->SetOpacity(opacity);
        if (active) m_active_layer = layer;
        Touch();
        return layer;
    }

    /*
 * ── drawing the text boxes ───────────────────────────────────────────────
 *
 * PROJECTED LIKE THE PIXELS ARE, which is the whole reason this happens here
 * rather than as a node over the view: a box is at a place in the DOCUMENT, so
 * panning and zooming have to move and scale it exactly as they move the paper
 * under it. The projection is the same one the layers get -- multiply by the
 * zoom, offset by the pan -- applied to the box rather than to a raster.
 *
 * The outline is drawn only while the text tool is held (ShowTextBoxes), because
 * the rest of the time these are words in a picture and a rectangle round them
 * would be a lie about what will print.
 */
    void draw_text_boxes(ETCS::RID target, int32_t ox, int32_t oy, float zoom)
    {
        if (m_text.empty()) return;
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        ETCS::Held<Glyphs_> g;
        if (m_glyphs != 0) g = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs);
        if (!g && m_text_warned == false)
        {
            m_text_warned = true;
            ETCS_LOG("PaintDocument", "there are text boxes but no glyph provider "
                     "bound -- BindGlyphs first, or they cannot be drawn.");
        }

        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        std::vector<std::string> lines;
        for (const PaintTextBox& b : m_text)
        {
            const int32_t vx = ox + static_cast<int32_t>(b.x * z);
            const int32_t vy = oy + static_cast<int32_t>(b.y * z);
            const int32_t vw = std::max(1, static_cast<int32_t>(b.w * z));
            const int32_t vh = std::max(1, static_cast<int32_t>(b.h * z));
            const bool sel = (b.id == m_text_sel);

            if (m_text_show || sel)
            {
                /*
             * A one-pixel frame, as four thin rects -- that is what a surface can
             * draw. Coloured rather than pale: near-white with low alpha is
             * invisible on the paper it is drawn on, and an affordance you cannot
             * see is not one. The open box is stronger and fully opaque, the rest
             * are dimmer, so "which box has the keyboard" is answerable at a
             * glance -- and it carries the corner handle that resizes it.
             */
                const float r0 = sel ? 0.15f : 0.35f;
                const float g0 = sel ? 0.50f : 0.45f;
                const float b0 = sel ? 0.95f : 0.60f;
                const float a  = sel ? 1.00f : 0.55f;
                surface->DrawRect(vx, vy, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy + vh - 1, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
                surface->DrawRect(vx + vw - 1, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
                if (sel)
                    surface->DrawRect(vx + vw - TEXT_HANDLE_PX, vy + vh - TEXT_HANDLE_PX,
                                      static_cast<uint32_t>(TEXT_HANDLE_PX), static_cast<uint32_t>(TEXT_HANDLE_PX),
                                      r0, g0, b0, a);
            }

            if (!g) continue;
            wrap_text(g.get(), b, lines);
            const uint32_t px = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(b.size * z)));
            const int32_t step = line_step(b);
            int32_t caret_x = vx, caret_y = vy;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const int32_t top = static_cast<int32_t>(i) * step;
                // Lines past the box's bottom are not drawn: the box is the page.
                if (top + static_cast<int32_t>(b.size) > b.h) break;
                const int32_t ly = vy + static_cast<int32_t>(top * z);
                if (!lines[i].empty())
                    g->RasterizeText(target, lines[i].c_str(), b.font, px, vx, ly,
                                     b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3]);
                caret_x = vx + static_cast<int32_t>(g->MeasureText(lines[i].c_str(), b.font, px).width);
                caret_y = ly;
            }
            // Where the next character goes, on the open box: typing always
            // lands at the end, so the end is the one place a caret can be.
            if (sel)
                surface->DrawRect(caret_x + 1, caret_y, std::max(1u, px / 12u), px,
                                  b.rgba[0], b.rgba[1], b.rgba[2], 0.85f);
        }
    }

    // The open box's corner handle, in VIEW pixels -- a press in it resizes
    // the box rather than moving it (PaintInput).
    static constexpr int32_t TEXT_HANDLE_PX = 8;

    /*
 * A NEW SELECTION STARTS FROM NOTHING, and never from a lift still in the air:
 * pixels cut for a carry that was then abandoned by a fresh Select* would
 * otherwise vanish with the mask that described them. They land where they
 * hover, which is where the last sample left them.
 *
 * False for a document with no extent, which has nothing to select in.
 */
    bool fresh_selection()
    {
        if (m_width == 0 || m_height == 0) return false;
        if (m_sel.lifted()) DropSelection();
        m_sel.Reset(m_width, m_height);
        return true;
    }

    /*
 * WHAT THE GESTURE JUST DREW, COMBINED WITH WHAT WAS THERE.
 *
 * Called at the end of every Select*: they each build their own region into a
 * mask that fresh_selection wiped, so at this point m_sel holds the GESTURE
 * and m_sel_base holds the selection the gesture started from. Replace is
 * leaving it alone, which is why a bare drag pays nothing for this.
 *
 * The base is snapshotted at the press rather than accumulated, because a drag
 * re-runs the Select* on every motion sample -- accumulating would mean the
 * region grew along the path of the pointer instead of being the rectangle it
 * currently describes.
 */
    void combine_selection()
    {
        if (m_sel_op == PaintSelectOp::Replace) return;
        PaintSelection gesture;
        gesture.mask = m_sel.mask;                // the gesture's own pixels
        gesture.w = m_sel.w; gesture.h = m_sel.h;
        m_sel.mask = m_sel_base;
        if (m_sel_op == PaintSelectOp::Add) m_sel.Union(gesture);
        else                                m_sel.Subtract(gesture);
    }

    /*
 * ── drawing the lifted pixels ────────────────────────────────────────────
 *
 * Through the same projection the layers get, at the layer's own strength, so
 * the carried region looks like the piece of the layer it is. A host-backed
 * view takes it in one resample (paint_composite_raw_scaled); a device-backed
 * one has no bytes to blend into and gets the coarse per-sample DrawRect that
 * PaintLayer::BlitTo falls back to, for the same reason.
 */
    void draw_lift(ETCS::RID target, int32_t ox, int32_t oy, float zoom, const PaintLayer* layer)
    {
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const uint32_t bw = m_sel.width(), bh = m_sel.height();
        const int32_t lx = m_sel.x0 + m_sel.dx, ly = m_sel.y0 + m_sel.dy;
        const int32_t vx = ox + static_cast<int32_t>(std::lround(lx * z));
        const int32_t vy = oy + static_cast<int32_t>(std::lround(ly * z));
        const uint32_t dw = std::max(1u, static_cast<uint32_t>(bw * z + 0.5f));
        const uint32_t dh = std::max(1u, static_cast<uint32_t>(bh * z + 0.5f));
        const float alpha = layer->opacity() * layer->viewStrength();

        if (Pixels_* dpx = etcs_direct_pixels(ETCS::resolve_in_family<Pixels_>("Pixels", target)))
        {
            paint_composite_raw_scaled(*dpx, m_sel.lift.data(), bw, bh, vx, vy, dw, dh, alpha);
            return;
        }
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;
        const uint32_t step = std::max(1u, static_cast<uint32_t>(4.0f / z));
        const uint32_t rw = std::max(1u, static_cast<uint32_t>(step * z + 1.0f));
        for (uint32_t y = 0; y < bh; y += step)
            for (uint32_t x = 0; x < bw; x += step)
            {
                const uint8_t* p = &m_sel.lift[(static_cast<size_t>(y) * bw + x) * 4];
                if (p[3] == 0) continue;
                surface->DrawRect(vx + static_cast<int32_t>(x * z), vy + static_cast<int32_t>(y * z),
                                  rw, rw, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                                  (p[3] / 255.0f) * alpha);
            }
    }

    /*
 * ── drawing the selection ────────────────────────────────────────────────
 *
 * THE OUTLINE OF THE MASK, on the view and never on a layer -- it is drawn
 * again from the mask every render, so it survives a pan, a zoom, a wheel
 * closing over it, and is gone the moment the selection is. That is what makes
 * it a preview rather than a mark, and it is why it lives here in the render
 * path rather than in the input machine, which is not the only thing that
 * re-renders (PaintPalette's zoom steps and PaintColorWheel::Close both do).
 *
 * WALKED AS EDGES, NOT AS PIXELS. A boundary pixel drawn as a z-by-z block is a
 * thick border at any zoom above 1; drawing the one-pixel EDGE of the block on
 * the side that faces out gives a hairline at every zoom, and at zoom 1 the two
 * are the same pixel. Outside the document counts as outside the mask, so a
 * selection that reaches the page edge is outlined along it rather than left
 * open.
 *
 * TWO TONES, ALTERNATING BY POSITION, because the outline has to read on white
 * paper and on black ink and on the dark layer beyond the page: whichever tone
 * vanishes against the ground, the other does not. Alternated by where the
 * segment falls on screen rather than by a counter along the path, so the
 * dashes are the same for a rectangle and a lasso and need no path to follow.
 * Not animated -- an outline that marches needs a clock, and this page has no
 * frame it could tick on.
 *
 * Shifted by the offset while lifted, since the region is wherever its pixels
 * currently hover.
 */
    void draw_selection(Surface_* view, int32_t ox, int32_t oy, float zoom)
    {
        if (m_sel.empty() || !view) return;
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const int32_t sx = m_sel.lifted() ? m_sel.dx : 0;
        const int32_t sy = m_sel.lifted() ? m_sel.dy : 0;
        auto vx_of = [&](int32_t x) { return ox + static_cast<int32_t>(std::lround((x + sx) * z)); };
        auto vy_of = [&](int32_t y) { return oy + static_cast<int32_t>(std::lround((y + sy) * z)); };

        for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
        {
            const int32_t vy = vy_of(y), vh = std::max(1, vy_of(y + 1) - vy);
            for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
            {
                if (!m_sel.at(x, y)) continue;
                const bool l = !m_sel.at(x - 1, y), r = !m_sel.at(x + 1, y);
                const bool t = !m_sel.at(x, y - 1), b = !m_sel.at(x, y + 1);
                if (!l && !r && !t && !b) continue;
                const int32_t vx = vx_of(x), vw = std::max(1, vx_of(x + 1) - vx);
                const bool light = (((vx + vy) >> 2) & 1) != 0;
                const float c = light ? 0.97f : 0.06f;
                if (l) view->DrawRect(vx,          vy,          1u, static_cast<uint32_t>(vh), c, c, c, 1.0f);
                if (r) view->DrawRect(vx + vw - 1, vy,          1u, static_cast<uint32_t>(vh), c, c, c, 1.0f);
                if (t) view->DrawRect(vx,          vy,          static_cast<uint32_t>(vw), 1u, c, c, c, 1.0f);
                if (b) view->DrawRect(vx,          vy + vh - 1, static_cast<uint32_t>(vw), 1u, c, c, c, 1.0f);
            }
        }
    }

    // Apply one brush sample to the active layer (document-side commit).
    // THROUGH activeLayer(), not the member: that is where the selection is
    // bound as the clip, and this is the freehand path -- the one every stroke
    // takes and the one that must not be the exception.
    //
    // AND THE POINT GOES IN THE NOTEBOOK HERE, not in PaintInput. This is the
    // one place a freehand mark reaches the document, which makes it the only
    // place a Dab's path can be recorded without the recording and the marking
    // being two different code paths that have to agree. It also means a
    // scripted stroke (PaintInput::ScriptPointer) is recorded exactly as a
    // device's is, because both arrive here.
    /*
     * ── ONE IMPLEMENTATION OF EVERY CHANGE ───────────────────────────────
     *
     * What a replay does with an entry and what the input does when the change
     * is made here are the same code, called with the same input: the entry.
     * A change made here is BUILT as its entry and handed to Perform, which
     * applies it through ApplyOp -- the function every member's replay runs --
     * and then writes it down. Two implementations of one mark were two
     * chances to disagree, and the record could only ever say what the second
     * one would do.
     *
     * A STROKE IS THE ONE CHANGE MADE A PIECE AT A TIME, because it is seen
     * while it is drawn: its entry is opened at the press (RememberOp), every
     * point goes in through StrokeTo, which lands exactly that point with the
     * same step ApplyOp takes for it (apply_step), and the release seals it.
     * Replaying the sealed entry takes the same steps in the same order.
     */
    void ApplyBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (!m_open_live) RememberOp(PaintOpKind::Dab, brush);
        StrokeTo(x, y);
    }

    void StrokeTo(int32_t x, int32_t y)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (!m_open_live) return;
        PaintLayer* l = layerFor(m_open);
        if (!l) return;
        m_open.addPoint(x, y);
        apply_step(l, m_open, m_open.points() - 1);
    }

    /*
     * A WHOLE CHANGE MADE HERE: a shape, a fill, a cleared layer -- anything
     * complete the moment it is committed. The layer's keyframe first when it
     * is due one (it must hold the picture BEFORE the change), then the change
     * through the replay's own code, then the entry.
     */
    bool Perform(PaintOp op)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        if (refuse_read_only("draw")) return false;
        Touch();
        sealOpenOp();
        if (op.marks() && op.layer)
            if (PaintLayer* l = layerFor(op)) keyframeIfDue(l);
        const bool ok = ApplyOp(op);
        op.author = m_author;
        op.sent   = false;
        setCursor(m_book.Append(std::move(op), m_cursor));
        return ok;
    }

    // An entry for the active layer, of this kind, with this brush and the
    // selection it will be clipped by: what the input fills in and Performs.
    PaintOp OpFor(PaintOpKind kind, const PaintBrushState& brush, uint32_t tolerance = 0)
    {
        PaintOp op;
        op.kind      = kind;
        op.brush     = brush;
        op.tolerance = tolerance;
        op.clip      = clipOf(m_sel);
        if (PaintLayer* l = activeLayer()) { op.layer = keyOf(l); op.order = l->order(); }
        return op;
    }

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    const std::string& name() const { return m_name; }
    // The stack, for a caller that wants it in one call. Built rather than
    // returned by reference: there is no stored vector any more -- see
    // OrderedLayers for why membership is the tree.
    std::vector<PaintLayer*> layers() const
    {
        std::vector<PaintLayer*> out;
        OrderedLayers(out);
        return out;
    }
    /*
     * THE LAYER, CLIPPED BY THIS DOCUMENT'S SELECTION.
     *
     * An accessor that writes, deliberately, and this is the argument for it:
     * the alternative is binding the clip at every site that marks -- five in
     * PaintInput, more in the shape commits -- and one of them being forgotten
     * is a tool that ignores the selection while its neighbours honour it,
     * which is worse than any of them getting it wrong together. Every mark
     * reaches a layer through here, so here is where the invariant holds:
     * a layer you got from a document is clipped by that document.
     *
     * Layers are spawned by scripts as often as by this type
     * (boot_paint_panels.etcs), so binding at creation would miss the ones
     * that matter most. The pointer is a member of this object and outlives
     * every layer under it.
     */
    PaintLayer* activeLayer() const
    {
        if (m_active_layer) m_active_layer->BindClip(&m_sel);
        return m_active_layer;
    }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::string m_name = "Untitled";
    PaintLayer* m_active_layer = nullptr;
    // The text boxes, their id counter, and the two view states the input machine
    // sets while the text tool is held. See PaintTextBox.
    std::vector<PaintTextBox> m_text;
    uint32_t  m_text_seq  = 0;
    uint32_t  m_text_sel  = 0;
    // The open box as it was when it was opened, the key of one placed and not
    // yet recorded, and the style the next new box starts with (SelectTextBox,
    // AddTextBoxColoured).
    PaintTextBox m_text_before;
    std::string  m_text_fresh;
    uint32_t     m_text_font = 0;
    uint32_t     m_text_size = 24;
    bool      m_text_show = false;
    bool      m_text_warned = false;
    ETCS::RID m_glyphs = 0;
    // The one selection, and the pixels it is carrying if any. See PaintSelection.
    uint64_t m_revision = 0;     // climbs on every change -- see Touch
    PaintSelection m_sel;
    // The gesture in progress, and the selection it started from. Both are
    // per-drag and both are cleared by EndSelectionGesture.
    PaintSelectOp m_sel_op = PaintSelectOp::Replace;
    std::vector<uint8_t> m_sel_base;
    PaintSelection m_clip;     // the clipboard: a selection's shape and bytes, kept

    /*
     * ── the notebook, and where in it this document currently stands ─────
     *
     * m_cursor is "the entry this picture is as of", and it is the ONLY
     * position this document keeps -- which is the point of the tree. It is
     * also every new entry's parent, so a change made after an undo forks
     * rather than overwrites, and it is the node redo looks for children of.
     *
     * Zero means "before anything", which an empty notebook and a document
     * wound all the way back both are, and both want the same answer from
     * every walk.
     */
    PaintNotebook m_book;
    PaintOp       m_open;
    bool          m_open_live = false;
    uint64_t      m_cursor    = 0;      // written only through setCursor
    std::atomic<int> m_forked{0};      // see forked()
    std::string   m_author;
    // The record's chain as this page has read it (ImportOps): XXH3 of every
    // line taken in, seeded with the chain before it -- a Record's own
    // arithmetic, so equal heads with different chains means a line this page
    // never took in. A stream (Absorb) keeps only the head, m_chain_seq: it is
    // ordered and whole or it has ended.
    uint64_t      m_chain     = 0;
    uint64_t      m_chain_seq = 0;
    // A page-level change made HERE that the room has not seen: the next
    // ExportOps sends the whole page (see it). Not raised by become_page --
    // that is the room's change arriving, and answering it with this page's
    // own was the ping-pong.
    bool          m_page_changed = false;
    uint64_t      m_key_seq = 0;     // see newKey
    // One change at a time: the input's thread draws while a read lands on a
    // pool worker, and a rewind moves the path both of them write to.
    mutable std::recursive_mutex m_doc_mu;
    std::string   m_stated;             // the Page line of the page last stated -- see write_baseline
    size_t        m_stated_left = 0;    // its lines not yet read back
    std::vector<std::string>           m_outbox;    // holds and releases to send (hold_line)
    std::map<std::string, std::string> m_holders;   // box key -> who holds it (absorb_hold)
    ETCS::RID     m_record = 0;          // the session's record, for the host's checkpoints
    ETCS::RID     m_share  = 0;          // see SetShare
    std::string   m_owner;               // the session's owner, whose releases count for anyone
    std::chrono::steady_clock::time_point m_text_touched = std::chrono::steady_clock::now();
    bool          m_restate = false;    // state the page with the next push -- see Restate
    size_t        m_unreadable = 0;     // lines the last ImportOps could not read
    // See rewind: this page's pending entries while the room's are taken in,
    // and what was in the air.
    bool                 m_rewound = false;
    std::vector<PaintOp> m_stash;
    bool                 m_held_stroke = false;
    PaintOp              m_held_open;
    bool                 m_held_box = false;
    PaintTextBox         m_held_text, m_held_before;
    std::string          m_held_fresh;
    int           m_quiet = 0;   // see Quiet

    void sealOpenOp()
    {
        if (!m_open_live) return;
        m_open_live = false;
        // An entry that marked nothing is not history. A press that never
        // moved, an anchored gesture abandoned before it had two corners,
        // a preview that committed nothing -- all of them open an entry and
        // none of them changed the picture.
        if (m_open.pts.empty()) { m_open = PaintOp{}; return; }
        // The cursor is the parent, which is what makes a change after an undo
        // a BRANCH rather than an overwrite.
        setCursor(m_book.Append(std::move(m_open), m_cursor));
        m_open = PaintOp{};
    }

    /*
     * KEYFRAME EVERY LAYER THAT IS ABOUT TO BE DISTURBED, then write down what
     * the stack will look like. Called BEFORE the structural change, so the
     * bytes needed to bring a layer back are already on the chain when the
     * roster that no longer mentions it arrives.
     *
     * Every layer, not only the ones this particular act touches: a merge takes
     * two and a delete takes one, but a reorder moves several and the cost of
     * being exact about which is a rule that will be wrong the first time
     * somebody adds a fourth structural verb.
     */
    void recordStructure(const char* why)
    {
        sealOpenOp();
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack) appendSnapshot(l);
        // AND THE ROSTER AS IT STANDS NOW, which is the entry an undo lands on.
        // Without it, undoing past a NEW layer finds no roster at or before the
        // target and reconciles to nothing -- the layer stays. The pair is
        // "here is the stack before" and, after the change, "here is the stack
        // after"; a walk backwards over the second arrives at the first.
        keyframeRoster();
        ETCS_LOG("PaintDocument", why << ": keyframed " << stack.size() << " layer(s) and the roster.");
    }

    // And the roster AFTER it, which is the entry undo actually walks over.
    bool sharing() const { return !m_author.empty(); }

    // One Text entry on the path -- see PaintOpKind::Text.
    void record_text(const PaintTextBox& b, bool removed, bool keyframe = false)
    {
        sealOpenOp();
        PaintOp op;
        op.kind     = PaintOpKind::Text;
        op.author   = m_author;
        op.box      = b;
        op.box.id   = 0;               // this document's number, not the room's
        op.removed  = removed;
        op.keyframe = keyframe;
        setCursor(m_book.Append(std::move(op), m_cursor));
    }

    // The edit on box `id` is over: recorded if anything about it changed,
    // dropped if it was placed and never written in, released in a session.
    void end_text_edit(uint32_t id)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b) return;
        const bool fresh = (b->key == m_text_fresh);
        m_text_fresh.clear();
        const std::string key = b->key;
        if (fresh && b->text.empty())
        {
            for (auto it = m_text.begin(); it != m_text.end(); ++it)
                if (it->id == id) { m_text.erase(it); break; }
            Touch();
        }
        else if (fresh || !b->same_as(m_text_before))
        {
            record_text(*b, false);
            ETCS_LOG("PaintDocument", "text box " << id << " edited: \"" << b->text << "\"");
        }
        if (sharing()) hold_line(PaintOpKind::Free, key, m_author);
    }
    // A hold or release of a box, into the outbox for Emit.
    void hold_line(PaintOpKind kind, const std::string& key, const std::string& whose)
    {
        PaintOp op;
        op.kind    = kind;
        op.seq     = m_cursor;
        op.author  = m_author;
        op.box.key = key;
        op.target  = whose;
        m_outbox.push_back(paint_op_encode(op));
    }

    // A box as an entry says it is, by key: made, changed or gone.
    void apply_text_state(const PaintTextBox& s, bool removed)
    {
        for (auto it = m_text.begin(); it != m_text.end(); ++it)
            if (it->key == s.key)
            {
                if (removed)
                {
                    if (m_text_sel == it->id) m_text_sel = 0;
                    m_text.erase(it);
                }
                else { const uint32_t id = it->id; *it = s; it->id = id; }
                Touch();
                return;
            }
        if (removed) return;
        PaintTextBox b = s;
        b.id = ++m_text_seq;
        m_text.push_back(b);
        Touch();
    }

    /*
     * THE BOXES AS OF A POINT IN HISTORY: every Text entry on the path, in
     * order, each the last word on its key. The boxes that stay keep their
     * numbers, so nothing holding one is surprised; the open box closes if its
     * key is gone.
     */
    bool rebuild_text(const std::vector<const PaintOp*>& chain)
    {
        std::vector<PaintTextBox> next;
        for (const PaintOp* o : chain)
        {
            if (o->kind != PaintOpKind::Text) continue;
            auto it = std::find_if(next.begin(), next.end(),
                                   [&](const PaintTextBox& t) { return t.key == o->box.key; });
            if (o->removed) { if (it != next.end()) next.erase(it); }
            else if (it != next.end()) *it = o->box;
            else next.push_back(o->box);
        }
        bool changed = next.size() != m_text.size();
        for (PaintTextBox& b : next)
        {
            const PaintTextBox* was = nullptr;
            for (const auto& t : m_text) if (t.key == b.key) was = &t;
            b.id = was ? was->id : ++m_text_seq;
            if (!was || !was->same_as(b)) changed = true;
        }
        m_text = std::move(next);
        if (m_text_sel && !FindTextBox(m_text_sel)) m_text_sel = 0;
        return changed;
    }

    // True, and said once, when this document is view only (SetReadOnly).
    bool refuse_read_only(const char* what) const
    {
        if (!readOnly()) return false;
        ETCS_LOG("PaintDocument", what << ": view only -- the host has not given this page drawing.");
        return true;
    }

    // The Page entry and the keyframes after it -- see ExportBaseline.
    size_t write_baseline(std::ostream& o)
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintOp page;
        page.kind   = PaintOpKind::Page;
        page.seq    = m_cursor;
        page.author = m_author;
        page.w = m_width; page.h = m_height;
        for (PaintLayer* l : stack)
            page.roster.push_back(PaintOp::Face{ l->order(), l->opacity(), l->visible(), l->name(), l->key() });
        const std::string page_line = paint_op_encode(page);
        o << page_line << "\n";
        size_t n = 1;
        for (PaintLayer* l : stack)
        {
            PaintOp snap;
            snap.kind   = PaintOpKind::Snapshot;
            snap.seq    = m_cursor;
            snap.layer  = keyOf(l);
            snap.order  = l->order();
            snap.author = m_author;
            snap.w      = l->PixelWidth();
            snap.h      = l->PixelHeight();
            if (!l->SnapshotBytes(snap.bytes)) continue;
            o << paint_op_encode(snap) << "\n";
            ++n;
        }
        // The boxes, as statements rather than edits: keyframes, which undo on
        // the other side steps over.
        for (const PaintTextBox& b : m_text)
        {
            PaintOp t;
            t.kind = PaintOpKind::Text;
            t.seq = m_cursor;
            t.author = m_author;
            t.box = b;
            t.box.id = 0;
            t.keyframe = true;
            o << paint_op_encode(t) << "\n";
            ++n;
        }
        /*
         * STATED, AND REMEMBERED AS STATED: the Page line and how many lines
         * came with it, so this page's read-back passes over exactly those
         * (ImportOps). Taking our own Page line would replace this document
         * with a copy of itself and drop its history; passing over EVERY own
         * line of that read (the `keep` this replaced) also passed over the
         * strokes pushed after it, which then never confirmed.
         */
        m_stated = paint_line_rest(page_line);
        m_stated_left = n;
        return n;
    }

    // Be the page a Page entry describes -- see AcceptOp.
    void become_page(const PaintOp& page)
    {
        if (m_sel.lifted()) DropSelection();
        renew(page.w, page.h);            // clears layers, text and history
        reconcileLayers(&page);
        for (PaintLayer* l : layers())
            if (l->PixelWidth() != page.w || l->PixelHeight() != page.h) l->Allocate(page.w, page.h);
        // AND THE STACK AS THE FIRST ENTRY of the history that starts here, a
        // keyframe the record already holds: a replay that goes back to this
        // point (a rewind, an undo) has the stack to reconcile to, rather than
        // leaving whatever the page had rearranged since.
        PaintOp stack = page;
        stack.kind      = PaintOpKind::Layers;
        stack.keyframe  = true;
        stack.sent      = true;
        stack.confirmed = true;
        stack.w = stack.h = 0;
        setCursor(m_book.Append(std::move(stack), m_cursor));
        ETCS_LOG("PaintDocument", "following the session's page: " << page.w << "x" << page.h
                 << ", " << page.roster.size() << " layer(s).");
    }

    /*
     * THE STACK AS IT STANDS, AS A KEYFRAME: what an undo of the stack change
     * about to be performed lands on. A keyframe is walked over like a pixel
     * keyframe, so the change it precedes is one step. The raster a stack
     * change carries (a merge's survivor, an import) is on the change's own
     * entry (PerformStack) -- a separate keyframe before it is what an undo
     * would land on, and one after it is never reached by a redo.
     */
    void keyframeRoster()
    {
        PaintOp op = rosterNow();
        op.keyframe = true;
        op.author   = m_author;
        setCursor(m_book.Append(std::move(op), m_cursor));
        Touch();
    }

    /*
     * A KEY FOR EVERY LAYER THAT HAS NONE, before a roster names them
     * (PaintLayer::key). Hashed from this page's author and the layer's RID
     * rather than drawn: the same layer keyed twice gets the same key, and
     * two members cannot key different layers alike short of sharing a name
     * and a RID. A layer made from a roster takes the roster's key instead
     * (reconcileLayers), which is how one layer is one layer everywhere.
     */
    void ensureKeys()
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack) keyOf(l);
    }

    // A layer's key, given now if it has none -- every entry that names a
    // layer names it by this (PaintOp::layer).
    uint64_t keyOf(PaintLayer* l) const
    {
        if (!l) return 0;
        if (l->key()) return l->key();
        const std::string seed = m_author + ':' + std::to_string(l->getRID());
        uint64_t k = XXH3_64bits(seed.data(), seed.size());
        if (!k) k = 1;
        l->SetKey(k);
        return k;
    }

    /*
     * THE SEAM A LAYER'S FACE CHANGES THROUGH (PaintLayer::face_change_begin/
     * end): the roster before as a keyframe, the roster after as the entry.
     * Quiet while a structural verb is already writing its own pair -- a
     * restack renumbers every layer and is one change, not five -- and
     * while a roster from the record is being applied, which is the room's
     * change and not an edit here.
     */
    struct Quiet
    {
        PaintDocument& d;
        explicit Quiet(PaintDocument& doc) : d(doc) { ++d.m_quiet; }
        ~Quiet() { --d.m_quiet; }
    };
public:
    // A layer's face, changed as an entry (PaintLayer::face_via_document).
    bool FaceChange(PaintLayer* layer, PaintLayer::FaceField field, int32_t i, float f, const std::string& s)
    {
        if (m_quiet) return false;
        PaintOp op = rosterNow();
        const uint64_t key = keyOf(layer);
        PaintOp::Face* face = nullptr;
        for (PaintOp::Face& fc : op.roster) if (fc.key == key) { face = &fc; break; }
        if (!face) return false;                          // not in the stack: nothing to record
        if (refuse_read_only("change a layer")) return true;
        switch (field)
        {
        case PaintLayer::FaceOrder:   face->order   = i; break;
        case PaintLayer::FaceName:    face->name    = s; break;
        case PaintLayer::FaceVisible: face->visible = (i != 0); break;
        case PaintLayer::FaceOpacity: face->opacity = f; break;
        }
        sealOpenOp();
        keyframeRoster();                                 // the stack before: what undo lands on
        PerformStack(std::move(op));
        return true;
    }
private:

    // The stack as it stands, as a roster entry (keys given where missing).
    PaintOp rosterNow()
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintOp op;
        op.kind = PaintOpKind::Layers;
        for (PaintLayer* l : stack)
            op.roster.push_back(PaintOp::Face{ l->order(), l->opacity(), l->visible(), l->name(), keyOf(l) });
        return op;
    }

    /*
     * A STACK CHANGE, APPLIED: the roster by reconcile, then the raster the
     * entry carries (a merge's survivor, an import) -- what AcceptOp does with
     * a `layers` entry from the room, and what every stack change made here
     * does with its own (PerformStack). One implementation: a new layer, a
     * removal, a merge, an import, a restack and a face change all reach the
     * stack the way their replay does.
     */
    bool apply_stack(const PaintOp& op)
    {
        reconcileLayers(&op);
        return ApplyOp(op);
    }

    // A stack change made here: applied as above, then written down. The
    // caller has written the stack BEFORE it (recordStructure or a keyframe
    // roster) when there is anything undo must be able to land on.
    bool PerformStack(PaintOp op)
    {
        std::lock_guard<std::recursive_mutex> hold(m_doc_mu);
        Touch();
        sealOpenOp();
        op.kind     = PaintOpKind::Layers;
        op.keyframe = false;
        const bool ok = apply_stack(op);
        op.author = m_author;
        op.sent   = false;
        setCursor(m_book.Append(std::move(op), m_cursor));
        return ok;
    }

    // A key for a layer about to be made from a roster: unique to this author
    // and to this point in the record, so a reload that restarts the counter
    // cannot make a key an earlier layer of the same author already has.
    uint64_t newKey()
    {
        const std::string seed = m_author + ":new:" + std::to_string(++m_key_seq) + ":"
                               + std::to_string(m_chain) + ":" + std::to_string(m_book.head()) + ":"
                               + std::to_string(m_revision);
        uint64_t k = XXH3_64bits(seed.data(), seed.size());
        return k ? k : 1;
    }

    void appendSnapshot(PaintLayer* layer)
    {
        if (!layer) return;
        PaintOp snap;
        snap.kind   = PaintOpKind::Snapshot;
        snap.layer  = keyOf(layer);
        snap.order  = layer->order();
        snap.author = m_author;
        snap.w      = layer->PixelWidth();
        snap.h      = layer->PixelHeight();
        snap.sent   = true;               // a keyframe is this page's own and never travels
        snap.confirmed = true;
        if (!layer->SnapshotBytes(snap.bytes)) return;
        setCursor(m_book.Append(std::move(snap), m_cursor));
    }

    PaintLayer* layerByKey(uint64_t key) const
    {
        if (key == 0) return nullptr;
        std::vector<PaintLayer*> layers;
        OrderedLayers(layers);
        for (PaintLayer* l : layers) if (l->key() == key) return l;
        return nullptr;
    }

    /*
     * THE KEY, AND ONLY THE KEY, for any entry that has one. A key that
     * matches nothing names a layer that is not in this picture (merged away,
     * removed), and putting its entry on whatever sits at its old position is
     * destructive: an undone merge used to restore the dead layer's empty
     * keyframe onto the survivor, which looked exactly like undo erasing the
     * picture. The order is consulted only for a line from before keys, which
     * names no key at all.
     */
    PaintLayer* layerFor(const PaintOp& op) const
    {
        if (op.layer != 0) return layerByKey(op.layer);
        std::vector<PaintLayer*> layers;
        OrderedLayers(layers);
        for (PaintLayer* l : layers) if (l->order() == op.order) return l;
        return nullptr;
    }

    /*
     * BE THE PICTURE AS OF seq. For every layer the notebook has touched:
     * restore its newest snapshot at or before seq, then replay that layer's
     * marks from there forward.
     *
     * PER LAYER rather than over the whole log, because a snapshot is a
     * layer's and replaying another layer's marks onto it would be wrong in
     * the one case that matters -- two layers painted alternately, which is
     * ordinary work rather than a corner.
     *
     * A layer with no snapshot at or before seq is left alone and said so:
     * its recorded past begins later than the point being asked for, so
     * there is nothing this function could restore it to that would be more
     * correct than what is already on it.
     */
    /*
     * BE THE STACK THE ROSTER DESCRIBES, before a single raster is restored.
     *
     * ORDER IS THE IDENTITY. A layer brought back by an undo is a new entity
     * with a new RID, because a detached one cannot be re-registered as a typed
     * child -- and that is the right answer rather than a compromise: a RID is a
     * causal position in the runtime, so it does not survive moving to a
     * different position in causal history. Position is what a script names and
     * what an entry's `order` field already carries, which is why the raster
     * phase after this resolves by order without anything having to be rewritten.
     */
    void reconcileLayers(const PaintOp* roster)
    {
        if (!roster) return;
        Quiet quiet(*this);          // the room's stack arriving, not an edit here

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        const std::vector<PaintOp::Face>& faces = roster->roster;

        /*
         * BY KEY FIRST, THEN BY ORDER. A key is the layer's name across the
         * whole session (PaintLayer::key), so a roster that moved a layer
         * finds that layer here and moves it, pixels and all; order alone
         * found whatever sat at the position and renamed it, and a restack
         * arrived as every layer changing its face over pixels that stayed.
         * Order is what a roster from before keys, or a layer that never
         * had one, is matched by.
         */
        std::vector<PaintLayer*> at(faces.size(), nullptr);
        std::vector<bool> used(stack.size(), false);
        for (size_t i = 0; i < faces.size(); ++i)
        {
            if (!faces[i].key) continue;
            for (size_t j = 0; j < stack.size(); ++j)
                if (!used[j] && stack[j]->key() == faces[i].key) { at[i] = stack[j]; used[j] = true; break; }
        }
        for (size_t i = 0; i < faces.size(); ++i)
        {
            if (at[i]) continue;
            for (size_t j = 0; j < stack.size(); ++j)
                if (!used[j] && (stack[j]->key() == 0 || faces[i].key == 0) && stack[j]->order() == faces[i].order)
                { at[i] = stack[j]; used[j] = true; break; }
        }

        // Anything the roster does not mention is not in this picture.
        // Detached rather than deleted, exactly as RemoveLayer does.
        for (size_t j = 0; j < stack.size(); ++j)
        {
            if (used[j]) continue;
            if (m_active_layer == stack[j]) m_active_layer = nullptr;
            stack[j]->SetDim(1.0f);          // it is nobody's hover target now
            stack[j]->SetPeek(0.0f);
            stack[j]->detachFromParent();
        }

        // And anything the roster names that is not here comes back -- empty,
        // because the keyframe that fills it is the next phase's job.
        for (size_t i = 0; i < faces.size(); ++i)
        {
            const PaintOp::Face& f = faces[i];
            PaintLayer* l = at[i];
            if (!l)
            {
                l = this->addTag<PaintLayer>();
                if (!l) continue;
                l->Create(m_width, m_height);
                l->Clear(0.0f, 0.0f, 0.0f, 0.0f);
            }
            if (f.key) l->SetKey(f.key);
            l->SetOrder(f.order);
            l->SetName(f.name);
            l->SetOpacity(f.opacity);
            l->SetVisible(f.visible);
        }

        OrderedLayers(stack);
        if (!m_active_layer && !stack.empty()) m_active_layer = stack.back();
    }

    bool replayTo(uint64_t seq, const char* what)
    {
        if (m_sel.lifted()) DropSelection();

        // THE CANONICAL PATH, and only it. Sequence order stopped being the
        // answer when a second branch became possible: entries numbered
        // between a keyframe and the target may belong to a sibling, and
        // replaying those paints a picture nobody ever made.
        // AS THE RECORD READS IT: the canonical path less what retractions on
        // it took back (PaintNotebook::EffectivePath). A local document with no
        // retractions gets the canonical path unchanged.
        std::vector<const PaintOp*> chain;
        m_book.EffectivePath(seq, chain);

        // STRUCTURE FIRST. The rasters below are restored onto the layers this
        // puts back; doing it the other way round restores pixels onto planes
        // that are about to be replaced.
        const PaintOp* roster = nullptr;
        for (const PaintOp* o : chain) if (o->structural()) roster = o;
        reconcileLayers(roster);

        /*
         * BY THE LAYER AN ENTRY RESOLVES TO (layerFor), resolved once per
         * entry up front and grouped by the answer: a layer's keyframe and the
         * marks after it are one group whoever made them. Grouping by what an
         * entry NAMED once put a foreign entry in a group with no keyframe, and
         * every undo in a room took the others' strokes off the layer with it.
         */
        std::vector<ETCS::RID> touched;
        std::vector<PaintLayer*> lands(chain.size(), nullptr);
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            // A structural entry names no layer UNLESS it carries one's bytes,
            // and a box never does (rebuild_text, below).
            if (o->structural() && o->bytes.empty()) continue;
            if (o->kind == PaintOpKind::Text) continue;
            lands[i] = layerFor(*o);
            if (!lands[i]) continue;
            const ETCS::RID rid = lands[i]->getRID();
            bool seen = false;
            for (ETCS::RID r : touched) if (r == rid) { seen = true; break; }
            if (!seen) touched.push_back(rid);
        }

        size_t replayed = 0, restored = 0;
        for (ETCS::RID rid : touched)
        {
            const PaintOp* base = nullptr;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                const bool keyframes_it =
                    (o->kind == PaintOpKind::Snapshot || (o->structural() && !o->bytes.empty()))
                    && lands[i] && lands[i]->getRID() == rid;
                if (keyframes_it) base = o;
            }
            if (!base)
            {
                ETCS_LOG("PaintDocument", what << ": layer " << rid
                         << " has no keyframe on this path -- left as it is.");
                continue;
            }
            if (!ApplyOp(*base)) continue;
            ++restored;
            bool past = false;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                if (!past) { if (o == base) past = true; continue; }
                if (!lands[i] || lands[i]->getRID() != rid || !o->marks()) continue;
                if (ApplyOp(*o)) ++replayed;
            }
        }

        // THE TEXT, from the same path: every box as its last entry left it.
        const bool text = rebuild_text(chain);

        Touch();
        ETCS_LOG("PaintDocument", what << " to " << seq << ": " << restored
                 << " layer(s) restored, " << replayed << " op(s) replayed along a "
                 << chain.size() << "-entry path" << (text ? ", text boxes changed" : "") << ".");
        return restored > 0 || text;
    }

    // What the paper is cleared to -- the page's convention, stated once here
    // for the two verbs that have to make new paper (Resize, New) rather than
    // read back from a layer that may have been painted on since.
    static constexpr float PAPER_CLEAR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // The same ceiling a file is held to: a page bigger than this is an
    // allocation, not a picture (PAINT_IMAGE_MAX_SIDE). And then whether the
    // allocation fits: every layer at the new size, the mask, and the one old
    // layer Rebase holds while it copies -- asked once here, since a refusal
    // after the first layer has moved would leave the stack two sizes at once.
    bool extent_ok(uint32_t w, uint32_t h, const char* what) const
    {
        if (w == 0 || h == 0 || w > PAINT_IMAGE_MAX_SIDE || h > PAINT_IMAGE_MAX_SIDE)
        {
            ETCS_LOG("PaintDocument", what << " " << w << "x" << h << " refused -- 1.."
                     << PAINT_IMAGE_MAX_SIDE << " on a side.");
            return false;
        }
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        const size_t page = static_cast<size_t>(w) * h;
        const size_t need = page * 4 * (stack.size() + 1) + page;
        std::string why;
        if (paint_heap_can_take(need, why)) return true;
        ETCS_LOG("PaintDocument", what << " " << w << "x" << h << " refused -- " << why
                 << ". A smaller page, or fewer layers, fits.");
        return false;
    }

    // One axis of the anchor: where the old extent's origin lands in the new
    // one so that the near edge (0), the middle (1) or the far edge (2) stays put.
    static int32_t anchor_shift(int a, uint32_t before, uint32_t after)
    {
        const int32_t d = static_cast<int32_t>(after) - static_cast<int32_t>(before);
        return (a <= 0) ? 0 : (a == 1) ? d / 2 : d;
    }

    /*
 * WHICH LAYER IS THE PAPER: the bottom of the stack, when nothing shows through
 * it. The bottom because that is where the page puts it and an import lands on
 * top (ImportImage), so it stays the bottom; opaque because a paper is a ground
 * -- the thing every other layer is seen against -- and a bottom layer with
 * holes in it is not that, it is ink over the dark layer beyond the page, and
 * treating it as paper would fill its holes in. Nothing else is remembered
 * about which layer was spawned as `paper`, so this is a question asked of the
 * pixels each time rather than a flag that could be stale.
 */
    static PaintLayer* ground_layer(const std::vector<PaintLayer*>& stack)
    {
        if (stack.empty() || !stack.front()->Opaque()) return nullptr;
        return stack.front();
    }

};

// Out of line because a layer's parent is a PaintDocument, which is declared
// after PaintLayer -- see the note on the declaration.
inline void PaintLayer::touch_document()
{
    ETCS::Entity* parent = this->getParent();
    // By TAG, not by dynamic_cast: getTrueType() hands back void* (the leaf is
    // reached through the wire, not through C++ inheritance), and a layer may
    // sit under something that is not a document in a test.
    if (!parent || !parent->hasTag(ETCS::Buffer("PaintDocument"))) return;
    static_cast<PaintDocument*>(parent->getTrueType())->Touch();
}

inline bool PaintLayer::face_via_document(FaceField field, int32_t i, float f, const std::string& s)
{
    ETCS::Entity* parent = this->getParent();
    if (!parent || !parent->hasTag(ETCS::Buffer("PaintDocument"))) return false;   // nobody's yet: nothing to record
    return static_cast<PaintDocument*>(parent->getTrueType())->FaceChange(this, field, i, f, s);
}

#endif // PAINTPROVIDER_PAINTDOCUMENT_H__
