#ifndef PAINTPROVIDER_PAINTOP_H__
#define PAINTPROVIDER_PAINTOP_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintTextBox.h"   // in order: everything above this in the module is visible here

/*
 * ONE ENTRY. The brush is carried by VALUE rather than read from the tool at
 * replay time, and that is the whole difference between a log and a rumour: the
 * tool has moved on by the time anything replays this, and on a viewer's machine
 * it was never the same tool.
 *
 * `layer` is a RID and is resolved at replay, not held: a layer can be
 * reordered, renamed or deleted between the entry and its use, and only the last
 * of those is a problem -- which is the same rule the snapshot store already
 * used and the reason it took the RID rather than an index.
 */
struct PaintOp
{
    uint64_t    seq   = 0;
    /*
     * THE ENTRY THIS ONE WAS MADE AFTER, which is what makes the notebook a
     * TREE rather than a list with a cursor on it.
     *
     * A session that never undoes has parent == the previous entry for every
     * one of them, so the tree is a line and nothing about it is visible.
     * Undo moves the cursor to a parent; a change made while the cursor is
     * behind the head becomes a SECOND CHILD instead of erasing the future,
     * and redo picks the most recent child. That is the whole of branching,
     * and it replaced a separate `m_redo_from` high-water mark -- "is there
     * anything to redo" is now "does the cursor have children", which is the
     * same question asked of the structure instead of of a second variable
     * that had to be kept in step with it.
     *
     * LOCAL, AND NEVER ON THE WIRE. A node renumbers every entry it accepts,
     * because two writers numbering their own would collide -- so a parent in
     * this runtime's numbering means nothing after renumbering, and carrying
     * it would make the node understand a field it otherwise copies blind.
     * Branches are a property of YOUR editing history; what a session replays
     * is the canonical path and only that (PaintDocument::ExportOps).
     */
    uint64_t    parent = 0;
    PaintOpKind kind  = PaintOpKind::Snapshot;
    /*
     * WHICH LAYER: its KEY (PaintLayer::key), the one name for it that is the
     * same on every member and survives an undo that brings it back as a new
     * entity. A RID is this runtime's alone -- a viewer's layers were spawned
     * separately -- and an order is a position a restack changes, so neither
     * can say which layer a replay should touch. The order is still carried:
     * it is what a line from before keys resolves by (layerFor).
     */
    uint64_t    layer = 0;
    int32_t     order = 0;
    // Who made it. Empty is "this page", which is what a session with nobody
    // else in it writes and what a local-only document keeps writing forever.
    std::string author;

    PaintBrushState brush;
    uint32_t        tolerance = 0;          // Fill only

    // x,y pairs. A Dab's path; a Line/Rect/Ellipse's two corners; a Poly's ring;
    // a Fill's one seed point.
    std::vector<int32_t> pts;

    // Snapshot only: the layer's bytes and the extent they are for. Checked at
    // restore rather than trusted -- a snapshot taken before a resize must not
    // be written over a buffer of another size, which is the check RestoreBytes
    // has always made and the reason a stale entry is dropped rather than fatal.
    std::vector<uint8_t> bytes;
    uint32_t w = 0, h = 0;

    /*
     * WHERE THE MARK WAS ALLOWED TO LAND: the selection that clipped it, as a
     * mask over its bounding box (PaintLayer::BindClip), carried by every
     * marking entry made under one. A selection is the page's own -- each
     * member has its own, and none of them is in the record -- so a replayed
     * stroke used to land unclipped everywhere but where it was made: on the
     * other members, and here under undo. Empty is "anywhere".
     */
    struct Clip
    {
        int32_t  x0 = 0, y0 = 0;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> mask;   // w*h, one byte each, nonzero inside
        bool empty() const { return mask.empty(); }
    };
    Clip clip;

    // Layers only: what the stack looks like after this entry. ORDER IS THE
    // IDENTITY here, not the RID -- a layer brought back by an undo is a new
    // entity at the same position, and position is what a script names and what
    // a viewer's own stack can be matched against.
    struct Face
    {
        int32_t     order   = 0;
        float       opacity = 1.0f;
        bool        visible = true;
        std::string name;
        uint64_t    key     = 0;    // PaintLayer::key; zero on a line that predates it
    };
    std::vector<Face> roster;

    /*
     * A ROSTER THAT MERELY STATES THE STACK, rather than changing it: the one
     * taken BEFORE a structural change, so an undo has somewhere to land. It is
     * a keyframe of structure and is walked over exactly as a pixel keyframe is.
     *
     * Without this flag a merge cost TWO presses of ctrl+z, and the second one
     * did nothing anybody could see -- which reads as a broken key, and is the
     * same failure stepping over pixel keyframes was added to avoid.
     */
    bool keyframe = false;

    // Text only: the box after the edit, or that it went. The key is its name
    // everywhere (PaintTextBox::key); the id means nothing past this document.
    PaintTextBox box;
    bool         removed = false;

    // Undo/Redo only: whose entry, and which of theirs (PaintOpKind::Undo).
    std::string target;
    uint32_t    ordinal = 0;

    /*
     * IN THE RECORD. Set when the entry arrived from the session (AcceptOp)
     * or went out to it (ExportOps, ExportBaseline); a keyframe this page
     * took for itself is born with it, since keyframes never travel. What a
     * push sends is every entry on the path without it -- a MARK, not a
     * number: "everything after sequence N" broke the moment a Page entry
     * emptied the notebook under N, and the answer to that was to push the
     * whole page again, which emptied everybody else's, which they answered
     * the same way. Local, never on the wire.
     */
    bool sent = false;

    /*
     * AND BACK: the record holds it, in the place the node gave it. A change
     * made here is drawn at once and is only PENDING until its line comes
     * back through a read -- set then, or on arrival for anything from the
     * room. What a pending entry becomes is the record's call: it is put
     * after whatever the room ordered before it (PaintDocument::rewind), and
     * taken away if the node refused it. `wire` is the line as it went out,
     * past its sequence and author, which is what its read-back is matched on.
     */
    bool        confirmed = false;
    std::string wire;

    // "Did this change the picture." A structural entry that changed the stack
    // did -- undoing it puts a layer back -- so it steps like a mark. A
    // retraction is not itself a mark: it names one, and it is what the
    // ordinals in the record count past.
    bool marks() const { return kind != PaintOpKind::Snapshot && !keyframe && !retraction(); }
    bool retraction() const { return kind == PaintOpKind::Undo || kind == PaintOpKind::Redo; }
    bool structural() const { return kind == PaintOpKind::Layers; }
    void addPoint(int32_t x, int32_t y) { pts.push_back(x); pts.push_back(y); }
    size_t points() const { return pts.size() / 2; }
};

#endif // PAINTPROVIDER_PAINTOP_H__
