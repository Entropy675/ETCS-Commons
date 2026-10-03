#ifndef PAINTPROVIDER_PAINTNOTEBOOK_H__
#define PAINTPROVIDER_PAINTNOTEBOOK_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintOp.h"   // in order: everything above this in the module is visible here

/*
 * THE LOG ITSELF -- append-only, sequence-numbered from 1 so that 0 can mean
 * "before anything", which is what a viewer asking for everything sends.
 *
 * SEQUENCES ARE ASSIGNED HERE and nowhere else. When this notebook is the one a
 * node holds, that makes the node the ordering domain for the session, which is
 * the same answer ChessNode reached for the same reason and by the same
 * argument: the sync unit and the ordering domain have to be one object or a
 * viewer following two documents needs two channels.
 *
 * THE COST OF UNDO IS THE SNAPSHOT INTERVAL. Replay-only undo is O(history) per
 * step, which is precisely why the store this replaces was snapshots; entries
 * with a whole-layer snapshot dropped in every SNAPSHOT_EVERY marks bounds it at
 * that interval instead. The same structure is what a late joiner wants --
 * nearest snapshot plus the tail -- so the interval is one knob for both and
 * neither reader has to know the other exists.
 */
class PaintNotebook
{
public:
    // Marks between snapshots, per layer. Sixteen is a compromise with two
    // readers: undo replays at most this many ops, and a viewer joining late
    // downloads at most this many on top of one snapshot.
    static constexpr size_t SNAPSHOT_EVERY = 16;

    uint64_t head() const { return m_ops.empty() ? 0 : m_ops.back().seq; }
    size_t   size() const { return m_ops.size(); }
    bool     empty() const { return m_ops.empty(); }

    // `parent` is where the document stood when this was made. Passed in
    // rather than read from a member, because the notebook does not own the
    // cursor -- the document does, and a store that kept its own copy of
    // somebody else's position is a second thing to keep in step.
    uint64_t Append(PaintOp op, uint64_t parent)
    {
        op.seq    = m_next++;
        op.parent = parent;
        return push(std::move(op));
    }

    /*
     * ONE NUMBERING, THIS PAGE'S. An entry that arrives from a session is
     * numbered here like one made here; the node's own sequence for it is
     * read off the line by ImportOps for the record chain and goes no
     * further. Keeping the node's number in the notebook was the bug it
     * looked like: a host with thirteen entries of its own took in the
     * room's entry 4, at(4) then found the host's OLD entry 4, and the tree
     * -- which links by sequence -- walked a path that was never made.
     */

    // Is this layer due for a keyframe? Asked by the document before it opens a
    // marking entry, so the snapshot lands BEFORE the mark it protects rather
    // than after it -- a snapshot taken after the change cannot undo it.
    bool snapshotDue(ETCS::RID layer) const
    {
        auto it = m_since_snap.find(layer);
        if (it == m_since_snap.end()) return true;      // never seen: seed one
        return it->second >= SNAPSHOT_EVERY;
    }

    const PaintOp* at(uint64_t seq) const
    {
        for (const PaintOp& o : m_ops) if (o.seq == seq) return &o;
        return nullptr;
    }

    const std::vector<PaintOp>& ops() const { return m_ops; }

    /*
     * ── the tree ────────────────────────────────────────────────────────
     *
     * THE CANONICAL PATH IS THE ONLY THING THAT GETS REPLAYED, and every one
     * of these exists to say what that path is. Sequence order stopped being
     * the answer the moment a second branch could exist: the entries between
     * a snapshot and a target may belong to a sibling, and replaying those
     * paints a picture that was never made.
     */

    // Root-first: the chain of entries from the beginning to `seq`. Empty if
    // `seq` names nothing, which is what a cursor of 0 means -- before
    // anything, and correct rather than an error.
    void ChainTo(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        out.clear();
        uint64_t walk = seq;
        // Bounded by the log's own size: a cycle cannot form from an append-only
        // store whose parents are always older, but this walks user-visible
        // state and a bound costs nothing next to trusting that.
        for (size_t guard = 0; walk != 0 && guard <= m_ops.size(); ++guard)
        {
            const PaintOp* o = at(walk);
            if (!o) break;
            out.push_back(o);
            walk = o->parent;
        }
        std::reverse(out.begin(), out.end());
    }

    // The children of `seq`, newest first -- which is the order redo wants and
    // the whole of the "branches decided by most recent" rule. Sequences are
    // unique and assigned in the order things happened, so there is no tie to
    // break.
    void ChildrenOf(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        out.clear();
        for (const PaintOp& o : m_ops) if (o.parent == seq) out.push_back(&o);
        std::sort(out.begin(), out.end(),
                  [](const PaintOp* a, const PaintOp* b) { return a->seq > b->seq; });
    }

    // The newest snapshot of this layer ON THIS CHAIN. Not "at or before seq":
    // an entry with a lower sequence can belong to a branch the target is not
    // on, and restoring from it would be restoring somebody else's past.
    static const PaintOp* SnapshotOnChain(const std::vector<const PaintOp*>& chain,
                                          ETCS::RID layer)
    {
        const PaintOp* best = nullptr;
        for (const PaintOp* o : chain)
        {
            // A structural entry that carries bytes IS a keyframe for the layer
            // it carries them for -- that is how a merge's result reaches the
            // survivor on the way forward (PaintDocument::PerformStack).
            const bool keyframes_it =
                (o->kind == PaintOpKind::Snapshot || (o->structural() && !o->bytes.empty()))
                && o->layer == layer;
            if (keyframes_it) best = o;
        }
        return best;
    }

    /*
     * ── the path as the record reads it ─────────────────────────────────
     *
     * A retraction (PaintOpKind::Undo) names an entry by (author, ordinal):
     * the ordinal is that author's count of MARKING entries along the path,
     * from one. Counted here, in one place, because the writer that makes the
     * edge and every member that applies it have to count the same way, and
     * they do not share a sequence numbering -- only this order.
     *
     * The effective path is the canonical path with three things taken out:
     * the retraction entries themselves (they say, they do not draw); every
     * entry a retraction names that no later redo put back; and, for each
     * layer, every KEYFRAME taken after the oldest retracted mark on it -- a
     * snapshot holds the pixels of everything before it, retracted or not, so
     * the layer has to be rebuilt from the last keyframe that predates the
     * retraction. replayTo over the result is the same walk it always was.
     */
    struct Ordinal { std::string author; uint32_t ordinal = 0; };

    static void Ordinals(const std::vector<const PaintOp*>& chain,
                         std::vector<Ordinal>& out)
    {
        out.assign(chain.size(), Ordinal{});
        std::unordered_map<std::string, uint32_t> count;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (!o->marks()) continue;
            out[i].author  = o->author;
            out[i].ordinal = ++count[o->author];
        }
    }

    // The entry an (author, ordinal) names on this path, or null.
    static const PaintOp* ByOrdinal(const std::vector<const PaintOp*>& chain,
                                    const std::string& author, uint32_t ordinal)
    {
        uint32_t seen = 0;
        for (const PaintOp* o : chain)
            if (o->marks() && o->author == author && ++seen == ordinal) return o;
        return nullptr;
    }

    // This author's newest marking entry on the path that stands (`retracted`
    // false) or that is retracted (`retracted` true): what an undo and a redo
    // respectively name. Answers its ordinal; zero when there is none.
    static uint32_t Newest(const std::vector<const PaintOp*>& chain,
                           const std::string& author, bool retracted)
    {
        std::vector<bool> gone;
        RetractedMask(chain, gone);
        uint32_t seen = 0, answer = 0;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (!o->marks() || o->author != author) continue;
            ++seen;
            if (gone[i] == retracted) answer = seen;
        }
        return answer;
    }

    // Which entries of `chain` the retractions on it have taken back, as a
    // mask aligned with it. A redo after an undo of the same entry puts it
    // back; the last word along the path wins.
    static void RetractedMask(const std::vector<const PaintOp*>& chain, std::vector<bool>& gone)
    {
        gone.assign(chain.size(), false);
        for (const PaintOp* r : chain)
        {
            if (!r->retraction()) continue;
            uint32_t seen = 0;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                if (o == r) break;                       // only what came before it
                if (!o->marks() || o->author != r->target) continue;
                if (++seen == r->ordinal) { gone[i] = (r->kind == PaintOpKind::Undo); break; }
            }
        }
    }

    void EffectivePath(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        std::vector<const PaintOp*> chain;
        ChainTo(seq, chain);
        std::vector<bool> gone;
        RetractedMask(chain, gone);

        // The oldest retracted mark per layer: keyframes of that layer from
        // there on are of a picture that no longer stands.
        std::unordered_map<ETCS::RID, uint64_t> first_gone;
        for (size_t i = 0; i < chain.size(); ++i)
            if (gone[i] && !first_gone.count(chain[i]->layer))
                first_gone[chain[i]->layer] = chain[i]->seq;

        out.clear();
        out.reserve(chain.size());
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (o->retraction() || gone[i]) continue;
            const bool keyframes = o->kind == PaintOpKind::Snapshot
                                || (o->structural() && !o->bytes.empty());
            if (keyframes)
            {
                auto it = first_gone.find(o->layer);
                if (it != first_gone.end() && o->seq > it->second) continue;
            }
            out.push_back(o);
        }
    }

    // Drop everything before the newest snapshot of every layer. What keeps a
    // long session bounded, and the reason snapshots exist at all rather than
    // being only an undo optimisation: without a keyframe there is nothing a
    // prefix can be discarded in favour of.
    /*
     * COMPACT ALONG ONE PATH, AND REFUSE IF THERE ARE OTHERS.
     *
     * Dropping by sequence number was right for a list and is wrong for a
     * tree: an entry with a low sequence can be the only thing holding a
     * branch's ancestry, and cutting it orphans every entry above it -- an
     * undo that walks into the gap then finds a parent that is not there.
     *
     * So this keeps the given path's prefix back to its own oldest still-
     * needed keyframe, and only when that path is the whole tree. A document
     * with live branches keeps everything, which is the correct answer at the
     * sizes anybody has actually reached; a session long enough for that to
     * hurt wants a policy for WHICH branches to forget, and inventing one
     * before anyone has hit the problem would be inventing the wrong one.
     */
    size_t Compact(const std::vector<const PaintOp*>& path)
    {
        if (path.empty()) return 0;
        if (path.size() != m_ops.size())
        {
            ETCS_LOG("PaintNotebook", "compact declined: " << (m_ops.size() - path.size())
                     << " entr(ies) sit off this path -- branches are kept whole.");
            return 0;
        }

        uint64_t cut = path.back()->seq;
        std::unordered_map<ETCS::RID, uint64_t> newest;
        for (const PaintOp* o : path)
            if (o->kind == PaintOpKind::Snapshot) newest[o->layer] = o->seq;
        if (newest.empty()) return 0;
        for (const auto& [rid, seq] : newest) { (void)rid; cut = std::min(cut, seq); }

        const size_t before = m_ops.size();
        std::vector<PaintOp> kept;
        kept.reserve(m_ops.size());
        for (PaintOp& o : m_ops) if (o.seq >= cut) kept.push_back(std::move(o));
        m_ops.swap(kept);
        // The oldest survivor is a root now; nothing above it may point past it.
        if (!m_ops.empty()) m_ops.front().parent = 0;
        return before - m_ops.size();
    }

    // The entries go; the numbering does not. A sequence names an entry for
    // as long as anything might still hold it (a parent link, a page's idea
    // of what it last sent), and starting again at one is how a stale name
    // comes to fit a new entry.
    void Clear() { m_ops.clear(); m_since_snap.clear(); }

    // Drop the tail from `seq` on. Undo does not use this -- an undo that
    // erased its own future could not be redone -- but a document reloaded from
    // a shorter notebook does.
    void Truncate(uint64_t seq)
    {
        while (!m_ops.empty() && m_ops.back().seq >= seq) m_ops.pop_back();
        m_since_snap.clear();
        for (const PaintOp& o : m_ops)
        {
            if (o.marks()) ++m_since_snap[o.layer];
            else            m_since_snap[o.layer] = 0;
        }
    }

private:
    uint64_t push(PaintOp op)
    {
        if (op.marks()) ++m_since_snap[op.layer];
        else            m_since_snap[op.layer] = 0;
        m_ops.push_back(std::move(op));
        return m_ops.back().seq;
    }

    std::vector<PaintOp> m_ops;
    std::unordered_map<ETCS::RID, size_t> m_since_snap;
    uint64_t m_next = 1;
};

/*
 * ── the notebook on the wire ─────────────────────────────────────────────────
 *
 * ONE ENTRY PER LINE, fields separated by single spaces, numbers in decimal:
 *
 *   <seq> <kind> <author> <layer> <order> <r> <g> <b> <a> <size> <hard> <tip>
 *         <blend> <tol> <npts> <x,y> <x,y> ... [clip <x0> <y0> <w> <h> <b64 png>]
 *   <seq> snap <author> <layer> <order> <w> <h> <base64 png>
 *   <seq> patch <author> <layer> <order> <x> <y> <w> <h> <base64 png>
 *   <seq> layers <author> 0 0 <keyframe> <n> <order,opacity,visible,name,key>...
 *         [<w> <h> <base64 png>]          -- the raster half, when it has one
 *   <seq> page <author> 0 0 <w> <h> <n> <faces as above>
 *   <seq> text <author> ... ; <seq> undo|redo <author> 0 0 <target> <ordinal>
 *
 * LINES, not a binary frame, for a reason that outlives the convenience: the
 * thing carrying these is an HTTP body and the thing relaying them is a node
 * that must renumber and re-attribute every entry without understanding any of
 * them. A node parses the first three fields and copies the rest through --
 * which is what lets the SAME node relay an entry kind that was added to
 * PaintProvider after the node was built.
 *
 * WHY THE NODE REWRITES AUTHOR. A writer that could name itself could name
 * somebody else. The token the push arrived with is the only trustworthy
 * statement of who is pushing, so the node puts that name in and drops whatever
 * the line claimed. Same argument as ChessGame refusing a move from a seat's
 * non-holder: the client's own account of who it is has no standing.
 *
 * A snapshot travels as a PNG rather than raw RGBA -- the same encoder an
 * export already uses -- because a 1024x768 layer is 3 MB raw, and the base64
 * of that is 4 MB, which is most of an HttpServer send buffer for one entry.
 */
// A name inside a space- and comma-delimited line: the two delimiters and
// '%' itself as %XX, nothing else touched, so a plain name is itself on the
// wire and a decoder that never heard of this reads it unchanged.
static inline std::string paint_wire_escape(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c == ' ' || c == ',' || c == '%')
        {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        }
        else out += static_cast<char>(c);
    }
    return out;
}

static inline std::string paint_wire_unescape(const std::string& s)
{
    auto nib = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && nib(s[i + 1]) >= 0 && nib(s[i + 2]) >= 0)
        {
            out += static_cast<char>((nib(s[i + 1]) << 4) | nib(s[i + 2]));
            i += 2;
        }
        else out += s[i];
    }
    return out;
}

// An entry's raster as a PNG in base64, "-" when it has none. Straight from
// the entry's bytes: they are RGBA at (w,h), packed, which is the layout the
// encoder wants and the layout SnapshotBytes produced.
static inline std::string paint_raster_b64(const PaintOp& op)
{
    std::vector<uint8_t> png;
    if (op.w && op.h && op.bytes.size() == size_t(op.w) * op.h * 4)
    {
        stbi_write_png_to_func(
            [](void* ctx, void* data, int len)
            {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                const uint8_t* b = static_cast<const uint8_t*>(data);
                v->insert(v->end(), b, b + len);
            },
            &png, static_cast<int>(op.w), static_cast<int>(op.h), 4,
            op.bytes.data(), static_cast<int>(op.w) * 4);
    }
    return png.empty() ? std::string("-") : paint_b64_encode(png.data(), png.size());
}

// The reverse: the PNG's pixels into the entry, four channels forced exactly
// as an import does, since a layer is RGBA and a PNG that was greyscale on
// the way out would otherwise come back a different width in bytes.
static inline bool paint_raster_decode(const std::string& b64, PaintOp& out)
{
    if (b64 == "-") return false;
    std::vector<uint8_t> png;
    if (!paint_b64_decode(b64, png)) return false;
    int w = 0, h = 0, comp = 0;
    uint8_t* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 4);
    if (!px) return false;
    out.bytes.assign(px, px + size_t(w) * size_t(h) * 4);
    out.w = static_cast<uint32_t>(w);
    out.h = static_cast<uint32_t>(h);
    stbi_image_free(px);
    return true;
}

// A line past its sequence and author: what a node copies through unread, and
// what a pending entry's read-back is matched on (PaintOp::wire).
static inline std::string paint_line_rest(const std::string& line)
{
    size_t at = 0;
    for (int field = 0; field < 3 && at != std::string::npos; ++field)
    {
        at = line.find(' ', at);
        if (at != std::string::npos) ++at;
    }
    return (at == std::string::npos) ? std::string() : line.substr(at);
}

/*
 * A NUMBER EXACTLY. Nine significant digits is what brings every float back to
 * the same bits (FLT_DECIMAL_DIG); six decimal places -- std::to_string -- did
 * not, so a brush size or an opacity off a slider was replayed on every other
 * member as a neighbouring value, and a replay of the same input drew a
 * different picture. The line carries the input, not a rounding of it.
 */
static inline std::string paint_float_text(float f)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(f));
    return b;
}

static inline std::string paint_op_encode(const PaintOp& op)
{
    std::string out;
    out += std::to_string(op.seq);
    out += ' ';
    out += paint_op_name(op.kind);
    out += ' ';
    out += op.author.empty() ? "-" : op.author;
    out += ' ';
    out += std::to_string(op.layer);
    out += ' ';
    out += std::to_string(op.order);

    /*
     * A box: its key first -- the one field past the author the node reads, to
     * refuse a box somebody else is holding -- then its place, its type, its
     * colour, the two flags, and the string in base64 ("-" when empty), since
     * a caption may hold every character the line format uses.
     */
    if (op.kind == PaintOpKind::Text)
    {
        const PaintTextBox& b = op.box;
        out += ' ' + (b.key.empty() ? std::string("-") : b.key);
        out += ' ' + std::to_string(b.x) + ' ' + std::to_string(b.y)
             + ' ' + std::to_string(b.w) + ' ' + std::to_string(b.h)
             + ' ' + std::to_string(b.font) + ' ' + std::to_string(b.size);
        for (float c : b.rgba) out += ' ' + paint_float_text(c);
        out += op.removed  ? " 1" : " 0";
        out += op.keyframe ? " 1" : " 0";
        out += ' ';
        out += b.text.empty() ? std::string("-")
                              : paint_b64_encode(reinterpret_cast<const uint8_t*>(b.text.data()), b.text.size());
        return out;
    }

    // A retraction: whose entry and which. Layer and order above are zero.
    if (op.retraction())
    {
        out += ' ' + (op.target.empty() ? std::string("-") : op.target);
        out += ' ' + std::to_string(op.ordinal);
        return out;
    }
    // A hold or a release: the box's key, and whose hold a release ends.
    if (op.kind == PaintOpKind::Hold || op.kind == PaintOpKind::Free)
    {
        out += ' ' + (op.box.key.empty() ? std::string("-") : op.box.key);
        out += ' ' + (op.target.empty() ? std::string("-") : op.target);
        return out;
    }

    if (op.kind == PaintOpKind::Layers || op.kind == PaintOpKind::Page)
    {
        if (op.kind == PaintOpKind::Page)
            out += ' ' + std::to_string(op.w) + ' ' + std::to_string(op.h);
        // WHETHER IT STATES OR CHANGES, on the wire as it is in the notebook:
        // a keyframe roster is what an undo lands on and is not a step, so a
        // member that read one as a step counted every retraction after it
        // one off -- and undid the wrong entry, in the wrong direction.
        else out += op.keyframe ? " 1" : " 0";
        out += ' ';
        out += std::to_string(op.roster.size());
        for (const PaintOp::Face& f : op.roster)
        {
            // comma-separated within a layer, space between layers, so the
            // NAME is the one field that must carry neither: those two and
            // '%' go as %XX (paint_wire_escape). Turning them into '_' was
            // simpler and showed every other member a different name from
            // the one typed -- 'layer 3' here, 'layer_3' there -- which is a
            // divergence in the one field a person reads.
            out += ' ' + std::to_string(f.order) + ',' + paint_float_text(f.opacity)
                 + ',' + (f.visible ? "1" : "0") + ','
                 + (f.name.empty() ? std::string("-") : paint_wire_escape(f.name))
                 + ',' + std::to_string(f.key);
        }
        // The raster half, when the entry carries one (a merge's survivor, an
        // import): the same PNG a snapshot line ends with, after the faces.
        if (op.kind == PaintOpKind::Layers && !op.bytes.empty())
            out += ' ' + std::to_string(op.w) + ' ' + std::to_string(op.h) + ' ' + paint_raster_b64(op);
        return out;
    }

    if (op.kind == PaintOpKind::Snapshot)
    {
        out += ' '; out += std::to_string(op.w);
        out += ' '; out += std::to_string(op.h);
        out += ' ';
        out += paint_raster_b64(op);
        return out;
    }

    // A patch: where, then the rectangle as a snapshot line ends.
    if (op.kind == PaintOpKind::Patch)
    {
        const int32_t x = op.pts.size() >= 2 ? op.pts[0] : 0, y = op.pts.size() >= 2 ? op.pts[1] : 0;
        out += ' ' + std::to_string(x) + ' ' + std::to_string(y)
             + ' ' + std::to_string(op.w) + ' ' + std::to_string(op.h) + ' ' + paint_raster_b64(op);
        return out;
    }

    const PaintBrushState& b = op.brush;
    auto num = [](float f) { return paint_float_text(f); };
    out += ' ' + num(b.color.r) + ' ' + num(b.color.g) + ' ' + num(b.color.b) + ' ' + num(b.color.a);
    out += ' ' + num(b.size_px) + ' ' + num(b.hardness);
    out += ' ' + std::to_string(static_cast<int>(b.tip));
    out += ' ' + std::to_string(static_cast<int>(b.blend));
    out += ' ' + std::to_string(op.tolerance);
    out += ' ' + std::to_string(op.points());
    for (size_t i = 0; i + 1 < op.pts.size(); i += 2)
        out += ' ' + std::to_string(op.pts[i]) + ',' + std::to_string(op.pts[i + 1]);
    // The clip, when there was one: its box, then the mask as a one-channel
    // PNG -- a rectangle costs a few hundred bytes, a wand's edge a few
    // thousand, and the decoder is the one a snapshot already has.
    if (!op.clip.empty())
    {
        std::vector<uint8_t> png;
        stbi_write_png_to_func(
            [](void* ctx, void* data, int len)
            {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                const uint8_t* b = static_cast<const uint8_t*>(data);
                v->insert(v->end(), b, b + len);
            },
            &png, static_cast<int>(op.clip.w), static_cast<int>(op.clip.h), 1,
            op.clip.mask.data(), static_cast<int>(op.clip.w));
        if (!png.empty())
            out += " clip " + std::to_string(op.clip.x0) + ' ' + std::to_string(op.clip.y0)
                 + ' ' + std::to_string(op.clip.w) + ' ' + std::to_string(op.clip.h)
                 + ' ' + paint_b64_encode(png.data(), png.size());
    }
    return out;
}

/*
 * AN ENTRY BY REFERENCE, through a work function's 256-byte data channel: a
 * magic word containing a NUL and the entry's address, the shape RouteRef
 * gives a request (NetworkProvider/RouteRequest.h) and for the same reason. The channel is a
 * reference carrier; the entry is the input. A text answer can never produce
 * these bytes (writeString stops at the first NUL), so nothing a script sends
 * reads as one.
 */
struct PaintOpRef
{
    static constexpr size_t FRAME = 8 + sizeof(uint64_t);
    static const char* Magic() { return "PAINTOP\0"; }

    static bool Emit(ETCS::Buffer& io, const PaintOp* op)
    {
        io.reset();
        const uint64_t p = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(op));
        return io.writeRaw(Magic(), 8) && io.writeRaw(&p, sizeof(p));
    }

    static const PaintOp* Read(const ETCS::Buffer& io)
    {
        if (io.written != FRAME || std::memcmp(io.buf, Magic(), 8) != 0) return nullptr;
        uint64_t p = 0;
        std::memcpy(&p, io.buf + 8, sizeof(p));
        return reinterpret_cast<const PaintOp*>(static_cast<uintptr_t>(p));
    }
};

static inline bool paint_op_decode(const std::string& line, PaintOp& out)
{
    std::istringstream in(line);
    std::string kind, author;
    out = PaintOp{};
    if (!(in >> out.seq >> kind >> author >> out.layer >> out.order)) return false;
    out.kind   = paint_op_from(kind);
    out.author = (author == "-") ? std::string() : author;

    if (out.kind == PaintOpKind::Text)
    {
        PaintTextBox& b = out.box;
        int removed = 0, keyframe = 0;
        std::string body;
        if (!(in >> b.key >> b.x >> b.y >> b.w >> b.h >> b.font >> b.size
                 >> b.rgba[0] >> b.rgba[1] >> b.rgba[2] >> b.rgba[3] >> removed >> keyframe >> body))
            return false;
        out.removed  = (removed != 0);
        out.keyframe = (keyframe != 0);
        if (body != "-")
        {
            std::vector<uint8_t> raw;
            if (!paint_b64_decode(body, raw)) return false;
            b.text.assign(raw.begin(), raw.end());
        }
        return true;
    }

    if (out.retraction())
    {
        if (!(in >> out.target >> out.ordinal)) return false;
        if (out.target == "-") out.target.clear();
        return true;
    }
    if (out.kind == PaintOpKind::Hold || out.kind == PaintOpKind::Free)
    {
        if (!(in >> out.box.key >> out.target)) return false;
        if (out.target == "-") out.target.clear();
        return true;
    }

    if (out.kind == PaintOpKind::Layers || out.kind == PaintOpKind::Page)
    {
        if (out.kind == PaintOpKind::Page && !(in >> out.w >> out.h)) return false;
        int keyframe = 0;
        if (out.kind == PaintOpKind::Layers && !(in >> keyframe)) return false;
        out.keyframe = (keyframe != 0);
        size_t n = 0;
        if (!(in >> n)) return false;
        for (size_t i = 0; i < n; ++i)
        {
            std::string field;
            if (!(in >> field)) return false;
            PaintOp::Face f;
            size_t a = field.find(','), b = field.find(',', a + 1), c = field.find(',', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
            const size_t d = field.find(',', c + 1);          // the key, on lines that carry one
            f.order   = std::atoi(field.substr(0, a).c_str());
            f.opacity = static_cast<float>(std::atof(field.substr(a + 1, b - a - 1).c_str()));
            f.visible = (field.substr(b + 1, c - b - 1) != "0");
            f.name    = paint_wire_unescape(field.substr(c + 1, d == std::string::npos ? std::string::npos : d - c - 1));
            if (f.name == "-") f.name.clear();
            if (d != std::string::npos) f.key = std::strtoull(field.c_str() + d + 1, nullptr, 10);
            out.roster.push_back(std::move(f));
        }
        // The raster tail, if the line has one. Unreadable pixels make the
        // entry a roster without them rather than no entry: the stack change
        // is the half every member must agree on.
        std::string b64;
        if (out.kind == PaintOpKind::Layers && (in >> out.w >> out.h >> b64))
            if (!paint_raster_decode(b64, out)) { out.bytes.clear(); out.w = out.h = 0; }
        return true;
    }

    if (out.kind == PaintOpKind::Snapshot)
    {
        std::string b64;
        if (!(in >> out.w >> out.h >> b64)) return false;
        return paint_raster_decode(b64, out);
    }

    if (out.kind == PaintOpKind::Patch)
    {
        int32_t x = 0, y = 0;
        std::string b64;
        if (!(in >> x >> y >> out.w >> out.h >> b64)) return false;
        out.addPoint(x, y);
        return paint_raster_decode(b64, out);
    }

    int tip = 0, blend = 0;
    size_t n = 0;
    if (!(in >> out.brush.color.r >> out.brush.color.g >> out.brush.color.b >> out.brush.color.a
             >> out.brush.size_px >> out.brush.hardness >> tip >> blend
             >> out.tolerance >> n)) return false;
    out.brush.tip   = static_cast<PaintTipMode>(tip);
    out.brush.blend = static_cast<PaintBlendMode>(blend);
    out.pts.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        std::string pair;
        if (!(in >> pair)) return false;
        const size_t c = pair.find(',');
        if (c == std::string::npos) return false;
        out.pts.push_back(std::atoi(pair.substr(0, c).c_str()));
        out.pts.push_back(std::atoi(pair.substr(c + 1).c_str()));
    }
    std::string word;
    if (in >> word && word == "clip")
    {
        std::string b64;
        if (!(in >> out.clip.x0 >> out.clip.y0 >> out.clip.w >> out.clip.h >> b64)) return false;
        std::vector<uint8_t> png;
        if (!paint_b64_decode(b64, png)) return false;
        int w = 0, h = 0, comp = 0;
        uint8_t* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &comp, 1);
        if (!px) return false;
        out.clip.w = static_cast<uint32_t>(w);
        out.clip.h = static_cast<uint32_t>(h);
        out.clip.mask.assign(px, px + size_t(w) * size_t(h));
        stbi_image_free(px);
    }
    return true;
}

#endif // PAINTPROVIDER_PAINTNOTEBOOK_H__
