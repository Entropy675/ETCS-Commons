#ifndef PAINTPROVIDER_PAINTTEXTBOX_H__
#define PAINTPROVIDER_PAINTTEXTBOX_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintFonts.h"   // in order: everything above this in the module is visible here

/*
 * ── A TEXT BOX ───────────────────────────────────────────────────────────────
 *
 * TEXT THAT IS STILL TEXT. The glyph tool used to prompt for a string, rasterise
 * it into the active layer and forget it -- after which the words were pixels,
 * as editable as a brush stroke and no more. Correcting a typo meant undoing and
 * retyping the lot.
 *
 * So a box keeps its string, in DOCUMENT coordinates, and is drawn on every
 * render from what it holds. It is content rather than decoration: it pans and
 * zooms with the picture, it survives switching tools and layers, and it can be
 * picked up again later and changed.
 *
 * NOT A LAYER, and not a node in the sheet's 2D tree either. A layer is a raster
 * and this is not; a sheet node would float above the picture and never pan with
 * it. What it is, is a second kind of thing the document contains, which is why
 * the document holds them.
 *
 * THE BOX IS THE COLUMN, THE SIZE IS THE TYPE. Text is set in the box's font at
 * its size and wraps where the box's width runs out -- at a space when there is
 * one, inside a word that is wider than the whole box -- and Enter starts a new
 * line. Lines past the bottom of the box are not drawn; drag the corner to make
 * room (PaintInput, the resize handle). Font, size and colour are the box's own,
 * set from the bar that opens over it while it is selected (PaintTextBar).
 */
struct PaintTextBox
{
    int32_t     x = 0, y = 0;       // document space, top-left
    int32_t     w = 1, h = 1;
    std::string text;
    uint32_t    id = 0;             // stable across edits, unlike an index
    /*
     * ITS NAME IN A SHARED SESSION. The id is this document's own counter and
     * means nothing on another page, so a box also carries a key that does:
     * who made it and their id for it ("alice.3"; "-.3" for one made before
     * the page was shared). Every page in the room names the box by it.
     */
    std::string key;
    // The colour it was placed with. On the box rather than read from the tool at
    // draw time, because the tool's colour moves on and this text should not: two
    // captions placed with different colours stay different.
    float       rgba[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    uint32_t    font = 0;           // a Glyphs font handle -- see PaintFonts
    uint32_t    size = 24;          // the line's height, in document pixels

    bool same_as(const PaintTextBox& o) const
    {
        return x == o.x && y == o.y && w == o.w && h == o.h && text == o.text
            && font == o.font && size == o.size
            && rgba[0] == o.rgba[0] && rgba[1] == o.rgba[1]
            && rgba[2] == o.rgba[2] && rgba[3] == o.rgba[3];
    }
};

/*
 * ── THE NOTEBOOK ─────────────────────────────────────────────────────────────
 *
 * WHAT HAPPENED, IN ORDER, RE-EXECUTABLE.
 *
 * The three-deep snapshot store this replaces said of itself, from the day it
 * was written, that it was a placeholder: "the real history comes from the
 * persistence tag: the input event stream is itself the record of what happened,
 * and undo will be a replay of it -- unbounded, and kept across sessions once
 * saving is in." That is this, and the seam it named -- Remember(), called by
 * every committed change before it lands -- is still the seam. No call site
 * moved.
 *
 * ONE OBJECT, FOUR JOBS, which is the argument for building it now rather than
 * building a fourth thing beside it:
 *
 *   undo/redo   walk the log
 *   sharing     a viewer replays it; a late one replays from a snapshot
 *   saving      a document is its notebook
 *   autosave    the head sequence is the only dirty check anyone needs
 *
 * AN ENTRY HAS A LIFETIME, unlike the snapshot it replaces. Remember() fires at
 * BeginStroke -- deliberately, so an anchored preview that commits nothing does
 * not spend a snapshot -- but a freehand stroke's content accrues through motion
 * afterwards and is complete only at release. So an entry is opened at the seam,
 * appended to while the stroke runs, and sealed at the release. An entry left
 * open by a lost release is sealed by the next seam rather than discarded: the
 * ink is on the layer either way, and a notebook that disagrees with the pixels
 * is worse than a slightly ragged stroke.
 *
 * WHY POINTS AND NOT SAMPLES. A Dab records every point ApplyBrush was actually
 * called with, interpolation included, rather than the raw pointer samples plus
 * a spacing rule. Replay is then the same calls in the same order and cannot
 * drift: re-deriving the spacing at replay time would make the viewer's copy a
 * function of PaintInput::apply_segment's arithmetic, which is exactly the kind
 * of agreement that holds until one side is edited. It costs more points than a
 * sample list -- a one-pixel nib over a thousand pixels is a thousand of them --
 * and that is the price of the guarantee.
 *
 * WHAT IS NOT DESCRIBED gets a Snapshot, and that is not an admission of
 * defeat: a paste carries arbitrary pixels, a lift is a hole plus a floating
 * buffer, and there is no compact descriptor for either that is not just the
 * bytes. Remember() with no describing call in front of it MEANS Snapshot, so a
 * change nobody taught the notebook about is recorded correctly rather than
 * silently missed -- the failure mode of the alternative.
 */
// How much a smudge step carries. Fixed rather than a tool setting, and one
// number for the pointer and for replay (PaintOpKind::Smudge), or the two would
// carry different pictures.
static constexpr float PaintInput_SMUDGE_STRENGTH = 0.6f;

enum class PaintOpKind : uint8_t
{
    Snapshot,   // whole-layer bytes; the only entry that restores without replay
    Dab,        // freehand: brush + every point it was stamped at
    Line, Rect, Ellipse,
    Poly,       // a shape mode's vertex ring, stroked closed
    Fill,
    Smudge,     // carried pixels: the path, replayed pair by pair (PaintLayer::SmudgeDab)
    Clear,      // the whole layer to the brush colour (ClearLayer)
    /*
     * A RECTANGLE OF PIXELS, STATED: what a region looks like after a change
     * that no brush describes -- a carried selection landing (and the hole it
     * left), a paste, a cut, the animation window's put. Replaces what is
     * there, like a snapshot of one rectangle rather than the layer, and
     * unlike a snapshot it IS a step: undo walks over it and a session
     * carries it. Before this those changes were a whole-layer keyframe
     * locally and nothing at all to the room.
     */
    Patch,
    /*
     * THE LAYER SET ITSELF, and adding it is what makes a merge undoable.
     *
     * Everything above is a change to PIXELS. A merge, a delete and a new layer
     * are changes to STRUCTURE, and a notebook that records only the first kind
     * can put the paint back and not the plane it was on -- which is why a merge
     * used to be a one-way door with a keyframe in front of it.
     *
     * This carries the roster AFTER the change and no rasters: the layers that
     * are about to be disturbed get ordinary keyframes immediately before it, so
     * the bytes to bring one back are already on the chain. Metadata only, so a
     * structural entry costs nothing next to the snapshots around it.
     */
    Layers,
    /*
     * THE WHOLE PAGE, stated rather than changed: its extent and its stack. It
     * begins a baseline (PaintDocument::ExportBaseline) -- one of these, then a
     * keyframe per layer -- and a document that takes one in BECOMES that page
     * (AcceptOp): same size, same layers, nothing else. Never kept in a
     * notebook; it is what a notebook starts from.
     */
    Page,
    /*
     * ONE TEXT BOX, AS IT STANDS AFTER AN EDIT -- or its removal. A box is a
     * string and a place, not pixels, so the entry carries the whole box and
     * replay folds these along the path (PaintDocument::replayTo): the boxes a
     * point in history has are the last word each entry said about its key.
     * One entry per edit, written when the edit ends (SelectTextBox), so undo
     * takes back a whole edit -- the typing, the font, the move -- in one step.
     */
    Text,
    /*
     * AN UNDO IS AN EDGE, NOT A WALK. In a shared session every member's
     * picture is a function of one record, so taking a stroke back has to be
     * something the record SAYS, replayed by everyone the same way -- not a
     * cursor moving in one page's own tree, which the room could only learn
     * about by being re-baselined with that page's whole picture (which is
     * what it used to do, and what wiped everyone else's unsent strokes).
     *
     * Names its target by (author, ordinal): the k-th marking entry that
     * author made, counted along the record. Never by sequence number -- a
     * page numbers its own entries locally and the node renumbers them on the
     * way in, so a sequence means nothing past the page that wrote it. Order
     * is the identity, and the order of one author's entries is the same on
     * every member (PaintNotebook's own rule, one level up).
     *
     * Redo is the same edge in reverse. Both are recorded, so a retraction and
     * its retraction are history like everything else, and "undo, then redo"
     * and "never touched" are different records of the same picture.
     */
    Undo,
    Redo,
    /*
     * ONE HAND ON A TEXT BOX, as record lines: a member's claim on a box's
     * key, and its release. Never in a notebook and never applied to a
     * picture -- every member derives who holds what from the same order
     * (PaintDocument::absorb_hold): the first claim in the record holds, a
     * later one from somebody else is void, and its maker sees its own come
     * back void (TextDenied). A release by the holder, or by the owner for a
     * member who left, frees the key. The record decides, and nothing else
     * keeps a table of claims.
     */
    Hold,
    Free,
};

static inline const char* paint_op_name(PaintOpKind k)
{
    switch (k)
    {
    case PaintOpKind::Snapshot: return "snap";
    case PaintOpKind::Dab:      return "dab";
    case PaintOpKind::Line:     return "line";
    case PaintOpKind::Rect:     return "rect";
    case PaintOpKind::Ellipse:  return "ellipse";
    case PaintOpKind::Poly:     return "poly";
    case PaintOpKind::Fill:     return "fill";
    case PaintOpKind::Smudge:   return "smudge";
    case PaintOpKind::Clear:    return "clear";
    case PaintOpKind::Patch:    return "patch";
    case PaintOpKind::Layers:   return "layers";
    case PaintOpKind::Page:     return "page";
    case PaintOpKind::Text:     return "text";
    case PaintOpKind::Undo:     return "undo";
    case PaintOpKind::Redo:     return "redo";
    case PaintOpKind::Hold:     return "hold";
    case PaintOpKind::Free:     return "free";
    }
    return "snap";
}

static inline PaintOpKind paint_op_from(const std::string& s)
{
    if (s == "dab")     return PaintOpKind::Dab;
    if (s == "line")    return PaintOpKind::Line;
    if (s == "rect")    return PaintOpKind::Rect;
    if (s == "ellipse") return PaintOpKind::Ellipse;
    if (s == "poly")    return PaintOpKind::Poly;
    if (s == "fill")    return PaintOpKind::Fill;
    if (s == "smudge")  return PaintOpKind::Smudge;
    if (s == "clear")   return PaintOpKind::Clear;
    if (s == "patch")   return PaintOpKind::Patch;
    if (s == "layers")  return PaintOpKind::Layers;
    if (s == "page")    return PaintOpKind::Page;
    if (s == "text")    return PaintOpKind::Text;
    if (s == "undo")    return PaintOpKind::Undo;
    if (s == "redo")    return PaintOpKind::Redo;
    if (s == "hold")    return PaintOpKind::Hold;
    if (s == "free")    return PaintOpKind::Free;
    return PaintOpKind::Snapshot;
}

/*
 * ── base64, because a snapshot has to travel as a line ───────────────────────
 *
 * The notebook's wire form is one entry per line, so a viewer can parse what it
 * has without waiting for the rest -- and a line cannot hold a NUL, which a PNG
 * begins with. Sixty-four characters is the price of that, and it is the same
 * price every other line-oriented transport pays.
 *
 * NOT A GENERAL CODEC. No wrapping, no whitespace tolerance beyond what a
 * splitter already removed: this encodes what this decodes and nothing else is
 * ever handed to it.
 */
static inline const char* paint_b64_alphabet()
{
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

static inline std::string paint_b64_encode(const uint8_t* p, size_t n)
{
    const char* A = paint_b64_alphabet();
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3)
    {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63];
        out += A[(v >>  6) & 63]; out += A[v & 63];
    }
    if (i + 1 == n)
    {
        const uint32_t v = uint32_t(p[i]) << 16;
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += "==";
    }
    else if (i + 2 == n)
    {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += A[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static inline bool paint_b64_decode(const std::string& s, std::vector<uint8_t>& out)
{
    int8_t rev[256];
    std::memset(rev, -1, sizeof(rev));
    const char* A = paint_b64_alphabet();
    for (int k = 0; k < 64; ++k) rev[static_cast<uint8_t>(A[k])] = static_cast<int8_t>(k);

    out.clear();
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s)
    {
        if (ch == '=') break;
        const int8_t v = rev[static_cast<uint8_t>(ch)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

#endif // PAINTPROVIDER_PAINTTEXTBOX_H__
