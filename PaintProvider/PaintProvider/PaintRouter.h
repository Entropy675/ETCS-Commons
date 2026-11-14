#ifndef PAINTPROVIDER_PAINTROUTER_H__
#define PAINTPROVIDER_PAINTROUTER_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintInput.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintRouter ─────────────────────────────────────────────────────────
 *
 * WHO GETS THE EVENT WHEN TWO PANES BOTH WANT IT.
 *
 * A session with more than one pane -- a sheet and a tool strip, a strip and a
 * layer window -- has two things that both answer "the pointer is over me", and
 * two PaintInputs with no arbiter between them is not a division of labour, it
 * is both of them acting. That was observable: a press on the strip picked a
 * colour AND painted a dab at the strip's coordinates, because the only thing
 * standing between them was PaintInput's own "is this my canvas" guard, which
 * one pane cannot state on another's behalf.
 *
 * So the arbitration is here, and it is the ordering the panes already have.
 * Panes are walked by their root's Drawable_::Order(), HIGHEST FIRST -- the
 * reverse of the composite, which is what "topmost" means and the same rule
 * Drawable2D_::PickAt uses inside one tree. One relation decides what is drawn
 * on top, what a click inside a tree hits, and which pane owns a click: they
 * cannot disagree, because there is nothing to keep in step.
 *
 * THE BUDGET IS THE PASS-THROUGH. An event carries a number of panes it may be
 * consumed by. Each pane whose bounds contain the point takes one and delivery
 * stops at zero, so a budget of 1 is "the topmost pane that contains it, and
 * nobody below" -- the strip eats its own click -- and 2 is "the top two", which
 * is how a pane becomes an overlay that watches without blocking. Containment,
 * not interest: a pane that is under the point has had its turn whether or not
 * it did anything with it, because "I was not interested" is a claim only the
 * pane can make and making delivery depend on it means one pane's indifference
 * silently re-enables another's.
 *
 * NOT ORDERABLE ITSELF. The router stands in no order; it reads one. Its panes'
 * roots are ordinary Drawable2Ds in their own trees, so a script arranges the
 * priority with the same SetOrder it already uses for a swatch.
 */


class PaintRouter : public DeletableBase<PaintRouter>
{
public:
    WIRE_TYPE_IDENTITY(PaintRouter);

    PaintRouter() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(uint32_t passes = 1)
    {
        m_panes.clear();
        m_budget = (passes == 0) ? 1u : passes;
        this->addTag("active");
        return true;
    }

    /*
 * A pane is a root to hit-test and an input to hand the hit to. Both by RID and
 * resolved per event rather than held: a pane can be deleted while the pointer
 * is moving over it, and a stale PaintInput* is the one failure this class
 * exists to be trusted through.
 */
    void AddPane(ETCS::RID root, ETCS::RID input)
    {
        if (root == 0 || input == 0) return;
        for (const Pane& p : m_panes)
            if (p.root == root && p.input == input) return;
        m_panes.push_back(Pane{ root, input });
    }

    void RemovePane(ETCS::RID root)
    {
        m_panes.erase(std::remove_if(m_panes.begin(), m_panes.end(),
                          [root](const Pane& p) { return p.root == root; }),
                      m_panes.end());
    }

    // How many panes an event may be consumed by. 0 is nonsense rather than
    // "drop everything", so it reads as 1 -- a router that delivers nothing is a
    // configuration mistake that looks exactly like a broken pointer.
    void SetPassBudget(uint32_t passes) { m_budget = (passes == 0) ? 1u : passes; }
    uint32_t passBudget() const { return m_budget; }

    /*
 * THE WALK. Highest Order() first, one unit of budget per pane that contains the
 * point, stop at zero.
 *
 * The order is read fresh per event from the roots themselves, so a script that
 * restacks a pane mid-session needs to tell this object nothing -- the same
 * reason Drawable_::Order() is a question rather than a cached field. A pane
 * whose root has gone is skipped and left in the list: the RID may be re-bound,
 * and quietly dropping panes would make a transient resolve failure permanent.
 *
 * Held for the walk, not merely resolved -- PickAt descends somebody else's tree
 * and the answer has to stay true for the whole descent (core/Entity.h).
 */
    void Route(const InputEvent& ev)
    {
        if (m_panes.empty()) return;

        /*
     * THE POINTER HELD ON ONE PANE. While an input is carrying a window by its
     * title (PaintInput::wantsCapture) every event goes to that pane, whether
     * or not the pane contains the point: the window moves one motion behind
     * the pointer, and a flick faster than its own title is tall would
     * otherwise leave it behind, stranded with the button still down. The
     * release is the pane's too, and ends the hold.
     */
        if (m_capture != 0)
        {
            const bool positional_c = (ev.action != INPUT_SCROLL);
            if (positional_c) { m_x = ev.x; m_y = ev.y; }
            PaintInput* held = nullptr;
            for (const Pane& pane : m_panes)
                if (pane.root == m_capture)
                    if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input))
                        held = static_cast<PaintInput*>(raw->getTrueType());
            if (held)
            {
                if (positional_c) held->NoteRoutedCursor(ev.x, ev.y);
                held->RouteEvent(ev);
            }
            if (!held || ev.action == INPUT_BUTTON_UP || ev.action == INPUT_UP || !held->wantsCapture())
                m_capture = 0;
            return;
        }

        /*
     * A SCROLL CARRIES A DELTA, NOT A POSITION, which makes it the one event
     * here that cannot say where it happened. Everything below -- containment,
     * the pick, the translation -- is a question about a POINT, so the point
     * used is the last one this router saw. That is not a fallback: a wheel
     * notch happens wherever the pointer already is, and asking it to carry a
     * position would mean inventing one.
     *
     * Kept as the router's own rather than read back from a pane, because the
     * router is what has seen every position regardless of which pane consumed
     * it.
     */
        const bool positional = (ev.action != INPUT_SCROLL);
        if (positional) { m_x = ev.x; m_y = ev.y; }
        const int32_t at_x = positional ? ev.x : m_x;
        const int32_t at_y = positional ? ev.y : m_y;

        // (Rank, index): Drawable_::Rank, so a hidden pane loses to every shown
        // one, and the tie-break is the order panes were added.
        std::vector<std::pair<std::pair<bool, int32_t>, size_t>> ranked;
        ranked.reserve(m_panes.size());
        for (size_t i = 0; i < m_panes.size(); ++i)
        {
            ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", m_panes[i].root);
            ranked.emplace_back(root ? Drawable_::Rank(root.get()) : std::pair<bool, int32_t>{ true, 0 }, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });

        /*
     * LEAVING IS AN EDGE, AND ONLY THE ROUTER CAN SEE IT.
     *
     * A pane is offered an event only while it contains the point, so from
     * inside a pane the pointer never leaves -- it simply stops arriving, which
     * is indistinguishable from the pointer standing still. Anything that holds
     * state for the duration of a hover would keep it forever.
     *
     * So the transition is reported from here, where both halves are known: a
     * pane that contained the LAST position and does not contain this one is
     * told once, on the motion that crossed the boundary. Motion only -- a
     * press does not move the pointer, so it cannot cross anything.
     */
        if (ev.action == INPUT_MOTION)
        {
            for (Pane& pane : m_panes)
            {
                bool inside = false;
                {
                    ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
                    inside = root && paint_pane_contains(pane.root, at_x, at_y);
                }
                if (pane.inside && !inside)
                {
                    if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input))
                        static_cast<PaintInput*>(raw->getTrueType())->RouteLeave();
                }
                pane.inside = inside;
            }
        }

        uint32_t left = m_budget;
        std::vector<size_t> delivered;
        for (const auto& r : ranked)
        {
            if (left == 0) break;
            const Pane& pane = m_panes[r.second];

            // Held for the containment test only, not for the pane's handling
            // of the event -- PaintInput::RouteEvent says why a hold must not
            // outlive the walk it was taken for.
            {
                ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
                if (!root) continue;
                if (!paint_pane_contains(m_panes[r.second].root, at_x, at_y)) continue;
            }

            ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input);
            if (!raw) continue;
            auto* input = static_cast<PaintInput*>(raw->getTrueType());
            if (!input) continue;

            // The pane's own root is what it routes against, so the event goes
            // over in the frame the router received it and PaintInput::RouteEvent
            // does the descent and the translation -- one place that knows how to
            // turn a window coordinate into a node's own, not two.
            // Not for a scroll: its x/y are a delta, and writing that in as the
            // routed cursor would move the point the next press routes against.
            if (positional) input->NoteRoutedCursor(ev.x, ev.y);
            input->RouteEvent(ev);
            --left;
            if (input->wantsCapture()) m_capture = pane.root;
            delivered.push_back(r.second);
        }

        // Everyone the press did not reach hears that it happened (PaintInput::
        // PressedElsewhere) -- the way a name field there learns to close.
        if (ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_DOWN)
            for (size_t i = 0; i < m_panes.size(); ++i)
            {
                if (std::find(delivered.begin(), delivered.end(), i) != delivered.end()) continue;
                if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", m_panes[i].input))
                    static_cast<PaintInput*>(raw->getTrueType())->PressedElsewhere();
            }
    }

    // A pointer without a device, for the page layer and for tests: the same
    // three verbs PaintInput exposes, arriving at the same walk. See
    // PaintInput::ScriptPointer for why a press carries the last position.
    void ScriptPointer(int32_t x, int32_t y)
    {
        m_x = x; m_y = y;
        InputEvent ev{};
        ev.action = INPUT_MOTION;
        ev.x = static_cast<int16_t>(x);
        ev.y = static_cast<int16_t>(y);
        Route(ev);
    }

    /*
 * WHICH BUTTON, because the right one does not mean what the left one means
 * (PaintInput's right-button branch: clear and pan, never draw). Press/Release
 * keep meaning the left button, which is what every existing caller wants and
 * what a bare "press" means in English.
 */
    void ScriptPress()   { ScriptPressButton(PAINT_BUTTON_LEFT); }
    void ScriptRelease() { ScriptReleaseButton(PAINT_BUTTON_LEFT); }

    void ScriptPressButton(uint16_t button)
    {
        InputEvent ev{};
        ev.key    = button;
        ev.action = INPUT_BUTTON_DOWN;
        ev.x = static_cast<int16_t>(m_x);
        ev.y = static_cast<int16_t>(m_y);
        Route(ev);
    }

    /*
 * ── A KEY IS NOT A CLICK ─────────────────────────────────────────────────
 *
 * This router used to turn every key event into ScriptPress/ScriptRelease, so
 * any keystroke drew a dab at wherever the pointer happened to be -- exactly the
 * mistake ontology/InputSource.h warns about in the comment that introduced
 * separate button events ("a paint program ended up drawing on any keystroke
 * instead of on a click"). The key ring was wired to the pointer's meaning.
 *
 * OFFERED TO EVERY PANE, AND THE ONE WITH THE FOCUS TAKES IT. A key carries no
 * position, so containment cannot choose a pane the way it does for a pointer,
 * and the thing that should receive it is whatever is being typed into. Rather
 * than keep a focus pointer here -- a second place for it to be wrong -- the
 * question is asked of each pane in the same top-first order, and the first that
 * says it consumed the key ends the walk. A pane with nothing selected consumes
 * nothing, so with no text box open every key still reaches nobody, which is the
 * behaviour anything not-yet-written depends on.
 */
    void RouteKey(uint16_t key, bool down)
    {
        // A modifier's edges are recorded and go no further -- see
        // PaintModifierKeys. Both edges, which is why this is above the
        // release early-out: ctrl coming UP is the half a chord depends on.
        if (paint_modifiers().Note(key, down)) return;
        if (!down) return;          // nothing else here acts on release yet
        std::vector<std::pair<std::pair<bool, int32_t>, size_t>> ranked;
        ranked.reserve(m_panes.size());
        for (size_t i = 0; i < m_panes.size(); ++i)
        {
            ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", m_panes[i].root);
            // A hidden pane is not offered the key, as paint_pane_contains keeps
            // it from the pointer. Its order still ranks it (the parking that
            // used to sink it is gone), so without this a closed window at order
            // 30 answers ctrl+PageDown ahead of the canvas and swallows it.
            if (root && root->Hidden()) continue;
            ranked.emplace_back(root ? Drawable_::Rank(root.get()) : std::pair<bool, int32_t>{ true, 0 }, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });

        for (const auto& r : ranked)
        {
            ETCS::Entity* raw = paint_resolve_tag("PaintInput", m_panes[r.second].input);
            if (!raw) continue;
            auto* in = static_cast<PaintInput*>(raw->getTrueType());
            if (in && in->KeyDown(key)) return;
        }
    }

    // Any pane wanting keys (PaintInput::editingText).
    bool Editing() const
    {
        for (const Pane& p : m_panes)
        {
            ETCS::Entity* raw = paint_resolve_tag("PaintInput", p.input);
            if (!raw) continue;
            auto* in = static_cast<PaintInput*>(raw->getTrueType());
            if (in && in->editingText()) return true;
        }
        return false;
    }

    /*
     * TYPE A STRING, as key presses. A soft keyboard hands the page characters,
     * not keys (its key events carry no code), so the page hands them here and
     * this turns each one back into the key that would have typed it -- the
     * US layout's, the same table the key path reads
     * (paint_key_to_char_shifted) -- with shift held around the ones that
     * need it. Enter and backspace are keys already (Key 257, Key 259).
     */
    void Type(const std::string& text)
    {
        static const char* from = "!@#$%^&*()_+{}|:\"<>?~";
        static const char* to   = "1234567890-=[]\\;',./`";
        for (char c : text)
        {
            uint16_t key = 0;
            bool shift = false;
            if (c >= 'a' && c <= 'z')      key = static_cast<uint16_t>(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') { key = static_cast<uint16_t>(c); shift = true; }
            else if (c >= 32 && c <= 126)
            {
                key = static_cast<uint16_t>(c);
                for (size_t i = 0; from[i]; ++i)
                    if (from[i] == c) { key = static_cast<uint16_t>(to[i]); shift = true; break; }
            }
            if (!key) continue;
            if (shift) paint_modifiers().Note(PaintModifierKeys::KEY_LEFT_SHIFT, true);
            RouteKey(key, true);
            if (shift) paint_modifiers().Note(PaintModifierKeys::KEY_LEFT_SHIFT, false);
        }
    }

    void ScriptReleaseButton(uint16_t button)
    {
        InputEvent ev{};
        ev.key    = button;
        ev.action = INPUT_BUTTON_UP;
        ev.x = static_cast<int16_t>(m_x);
        ev.y = static_cast<int16_t>(m_y);
        Route(ev);
    }

    size_t paneCount() const { return m_panes.size(); }

    // The panes in the order events will be offered to them, so a configuration
    // mistake reads as a wrong order rather than as a pane that never responds.
    void Report() const
    {
        ETCS_LOG("PaintRouter", "budget " << m_budget << ", " << m_panes.size() << " pane(s), top first:");
        std::vector<std::pair<std::pair<bool, int32_t>, size_t>> ranked;
        for (size_t i = 0; i < m_panes.size(); ++i)
        {
            ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", m_panes[i].root);
            ranked.emplace_back(root ? Drawable_::Rank(root.get()) : std::pair<bool, int32_t>{ true, 0 }, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& r : ranked)
            ETCS_LOG("PaintRouter", "  order=" << r.first.second
                     << (r.first.first ? "" : " (hidden)") << " root RID:" << m_panes[r.second].root
                     << " -> input RID:" << m_panes[r.second].input);
    }

private:
    // `inside` is the last answer to "did this pane contain the pointer", kept so
    // the crossing can be spotted. See Route.
    struct Pane { ETCS::RID root = 0; ETCS::RID input = 0; bool inside = false; };
    std::vector<Pane> m_panes;
    uint32_t m_budget = 1;
    int32_t  m_x = 0;
    int32_t  m_y = 0;
    ETCS::RID m_capture = 0;   // the pane holding the pointer -- see Route
};

#endif // PAINTPROVIDER_PAINTROUTER_H__
