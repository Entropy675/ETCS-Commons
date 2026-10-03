#ifndef PAINTPROVIDER_H__
#define PAINTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_PaintProvider.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <map>
#include <memory>
#include <cctype>
#include <unordered_map>
#include <vector>

// PaintProvider is intentionally a thin ontology layer on top of the existing
// RenderProvider::Surface family. It does not invent a second render backend; it
// composes the existing window- and image-surface verbs and adds a Pinta-like
// document/canvas/tool model around them.
//
// Script-facing surface: only the ETCS work/stream verbs that are useful and
// safe at the language boundary are exported. Internal helper setters and state
// mutation remain C++-only: they are part of the runtime model but not part of
// the user-visible ETCS contract. This matches ChessProvider's intent: the
// language exposes the valid subset; the C++ type enforces the rest.
//
// Input affinity: ConsumeInput is meant to run on the first detached script
// thread (the same side as Window::ProduceEvents / the OS event pump) so brush
// and canvas state never cross onto a generic worker mid-stroke.
//
// Pointer events: InputEvent carries an ABSOLUTE, content-area-relative
// position for INPUT_MOTION (see ontology/InputSource.h). There is no cursor to
// integrate -- the event already says where the pointer is, in the same space
// the canvas is measured in, so the brush lands under the actual cursor from
// the first sample rather than from wherever an accumulator started.
//
// That primitive matters more here than anywhere else in the codebase. An
// integrated cursor drifts by exactly the events it missed, and a stroke offset
// from the pointer is not a stroke anybody wants. It also means CaptureMouse
// stays off, which is correct: painting wants the desktop pointer, not the FPS
// look mode.


// ── work / stream surface ───────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintTool, SetRadius)
{
    (void)ctx;
    float radius = 0.0f;
    data >> radius;
    self.SetRadius(radius);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, SetMotionCoalesceMs, (double, ms))
{
    (void)ctx;
    self.SetMotionCoalesceMs(ms);
}

// SetAlphaPercent <0..100> -- the opacity the tool's colour is laid down at.
DEFINE_WORK_FUNC_TYPED(PaintTool, SetAlphaPercent, (int32_t, pct))
{
    (void)ctx;
    self.SetAlphaPercent(pct);
}

// SetKind <brush|line|rect|ellipse|fill|smudge|ruler|glyph|select>
DEFINE_WORK_FUNC(PaintTool, SetKind)
{
    (void)ctx;
    self.SetKind(data.restAsString());
}

// SetMode <rect|ellipse|wand|lasso> -- how the select tool draws its boundary.
// Read by that kind alone; see PaintSelectMode.
// SetShape <rect|oval|triangle|diamond|star> -- the shape tool's outline.
DEFINE_WORK_FUNC(PaintTool, SetShape)
{
    (void)ctx;
    std::string name; data >> name;
    self.SetShape(name);
}

DEFINE_WORK_FUNC(PaintTool, SetMode)
{
    (void)ctx;
    self.SetMode(data.restAsString());
}

// SetTip <round|stylus|erase> -- which nib is loaded, which is the stamp's
// shape and what it writes at once (PaintTipMode).
DEFINE_WORK_FUNC(PaintTool, SetTip)
{
    (void)ctx;
    self.SetTip(data.restAsString());
}

// SetBlendMode <normal|multiply|screen|erase> -- the blend alone, for the
// combination the tip's stepped list has no room for (a stylus that erases).
// Only Normal and Erase are read today; see PaintLayer::DrawBrush.
DEFINE_WORK_FUNC(PaintTool, SetBlendMode)
{
    (void)ctx;
    self.SetBlendMode(data.restAsString());
}

// SetHardness <0..1>. Held on the brush and not yet read by the stamp -- the
// nib's shape is PaintTipMode's business (paint_stamp_of).
DEFINE_WORK_FUNC_TYPED(PaintTool, SetHardness, (float, hardness))
{
    (void)ctx;
    self.SetHardness(hardness);
}

// SetText <text...> -- what the glyph tool places. The rest of the line, so a
// caption may contain spaces without the script quoting it.
DEFINE_WORK_FUNC(PaintTool, SetText)
{
    (void)ctx;
    self.SetText(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintTool, SetTextSize, (uint32_t, px))
{
    (void)ctx;
    self.SetTextSize(px);
}

// 0..255 per channel, alpha included -- see PaintTool::SetTolerance for why a
// zero-tolerance fill leaves a halo on anything antialiased.
DEFINE_WORK_FUNC_TYPED(PaintTool, SetTolerance, (uint32_t, tolerance))
{
    (void)ctx;
    self.SetTolerance(tolerance);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, SetColor, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetColor(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, BeginStroke, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.BeginStroke(x, y);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, MoveStroke, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.MoveStroke(x, y);
}

DEFINE_WORK_FUNC(PaintTool, EndStroke)
{
    (void)ctx; (void)data;
    self.EndStroke();
}

DEFINE_WORK_FUNC(PaintTool, CancelStroke)
{
    (void)ctx; (void)data;
    self.CancelStroke();
}

DEFINE_WORK_FUNC(PaintTool, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.Create(w, h);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, Clear, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, DrawPixel,
    (int32_t, x), (int32_t, y), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawPixel(x, y, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, DrawLine,
    (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1),
    (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawLine(x0, y0, x1, y1, r, g, b, a);
}

/*
 * The order key, set from a script or from a layer window's drag. One number and
 * nothing else moves: see PaintLayer's own header note on why a stack is a
 * relation rather than a container.
 */
DEFINE_WORK_FUNC_TYPED(PaintLayer, SetOrder, (int32_t, order))
{
    (void)ctx;
    self.SetOrder(order);
}

// SetName <text...> -- the rest of the line, so a layer may be called "Line art"
// without the script quoting it.
DEFINE_WORK_FUNC(PaintLayer, SetName)
{
    (void)ctx;
    self.SetName(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, SetVisible, (int32_t, visible))
{
    (void)ctx;
    self.SetVisible(visible != 0);
}

DEFINE_WORK_FUNC(PaintLayer, ToggleVisible)
{
    (void)ctx; (void)data;
    self.ToggleVisible();
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, SetOpacity, (float, opacity))
{
    (void)ctx;
    self.SetOpacity(opacity);
}

DEFINE_WORK_FUNC(PaintLayer, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintLayer, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, Create, (uint32_t, w), (uint32_t, h), (std::string, name))
{
    (void)ctx;
    self.Create(w, h, name);
}

// MergeDown / MergeUp <layer> -- two verbs, one act, named by direction because
// that is how a person means it; the sign is an implementation detail. No answer
// written back: a TYPED work function's arguments come out of the buffer and it
// has none to write into, and the refusals all log their own reason.
DEFINE_WORK_FUNC_TYPED(PaintDocument, MergeDown, (ETCS::RID, layer))
{
    (void)ctx;
    self.MergeLayer(layer, -1);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, MergeUp, (ETCS::RID, layer))
{
    (void)ctx;
    self.MergeLayer(layer, 1);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetActiveLayer, (ETCS::RID, layer))
{
    (void)ctx;
    self.SetActiveLayer(layer);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, ClearLayer,
    (ETCS::RID, layer), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.ClearLayer(layer, r, g, b, a);
}

// The glyph provider the text boxes are drawn with -- any leaf claiming Glyphs.
DEFINE_WORK_FUNC_TYPED(PaintDocument, BindGlyphs, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindGlyphs(glyphs);
}

// AddTextBox <x> <y> <w> <h>, in document coordinates.
DEFINE_WORK_FUNC_TYPED(PaintDocument, AddTextBox,
                       (int32_t, x), (int32_t, y), (int32_t, w), (int32_t, h))
{
    (void)ctx;
    self.AddTextBox(x, y, w, h);
}

// SetTextBoxText <id> <the rest of the line>. The text is taken raw rather than
// parsed as a field, because a caption contains spaces and commas.
DEFINE_WORK_FUNC(PaintDocument, SetTextBoxText)
{
    (void)ctx;
    uint32_t id = 0;
    data >> id;
    ETCS::Buffer rest;
    data >> rest;
    if (!self.SetTextBoxText(id, rest.toString()))
        ETCS_LOG("PaintDocument", "no text box " << id << " to set text on.");
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RemoveTextBox, (uint32_t, id))
{
    (void)ctx;
    self.RemoveTextBox(id);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, ShowTextBoxes, (int32_t, on))
{
    (void)ctx;
    self.ShowTextBoxes(on != 0);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectTextBox, (uint32_t, id))
{
    (void)ctx;
    self.SelectTextBox(id);
}

// ── the selection ────────────────────────────────────────────────────────
//
// The four ways in, the way out, and the carry, all in document coordinates.
// Verbs so that a script can select and move without a pointer -- which is
// also what makes the carry assertable: MoveSelection then two Reports, and
// the inked count has to have moved with it.

// SelectRect <x0> <y0> <x1> <y1> -- opposite corners, inclusive.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectRect,
                       (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SelectRect(x0, y0, x1, y1);
}

// SelectEllipse <x0> <y0> <x1> <y1> -- inscribed in that box.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectEllipse,
                       (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SelectEllipse(x0, y0, x1, y1);
}

// SelectColor <x> <y> <tolerance> -- the wand, on the active layer. Tolerance
// as PaintTool::SetTolerance takes it, 0..255 per channel.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectColor,
                       (int32_t, x), (int32_t, y), (uint32_t, tolerance))
{
    (void)ctx;
    self.SelectColor(x, y, tolerance);
}

// SelectPath <x> <y> <x> <y> ... -- the lasso, closed back to its first point.
// The rest of the line as pairs, since a path has no fixed arity; an odd
// trailing number is dropped rather than paired with nothing.
DEFINE_WORK_FUNC(PaintDocument, SelectPath)
{
    (void)ctx;
    std::vector<PaintStrokePoint> path;
    while (data.read_offset < data.written)
    {
        const size_t before = data.read_offset;
        PaintStrokePoint p{};
        data >> p.x;
        if (data.read_offset >= data.written) break;
        data >> p.y;
        if (data.read_offset == before) break;
        path.push_back(p);
    }
    if (!self.SelectPath(path))
        ETCS_LOG("PaintDocument", "SelectPath needs at least three points inside the page.");
}

DEFINE_WORK_FUNC(PaintDocument, ClearSelection)
{
    (void)ctx; (void)data;
    self.ClearSelection();
}

// MoveSelection <dx> <dy> -- lift the selected pixels off the active layer and
// drop them that far away; the selection goes with them.
// CopySelection / CutSelection / PasteSelection -- the clipboard, as verbs.
// ctrl+c / ctrl+x / ctrl+v are glue onto these (PaintInput::KeyDown).
DEFINE_WORK_FUNC(PaintDocument, CopySelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "copy: " << (self.CopySelection() ? "taken" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, DeleteSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "delete: " << (self.DeleteSelection() ? "cleared" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, CutSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "cut: " << (self.CutSelection() ? "taken" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, PasteSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "paste: " << (self.PasteSelection() ? "landed" : "nothing to paste"));
}

// Undo / Redo -- the three-deep placeholder history (PaintDocument::Remember).
DEFINE_WORK_FUNC(PaintDocument, Undo)
{
    (void)ctx; (void)data;
    const bool did = self.Undo();
    ETCS_LOG("PaintDocument", "undo: " << (did ? "stepped" : "nothing") << " ("
             << self.undoDepth() << " back, " << self.redoDepth() << " forward)");
}
DEFINE_WORK_FUNC(PaintDocument, Redo)
{
    (void)ctx; (void)data;
    const bool did = self.Redo();
    ETCS_LOG("PaintDocument", "redo: " << (did ? "stepped" : "nothing") << " ("
             << self.undoDepth() << " back, " << self.redoDepth() << " forward)");
}
// RedoAlt -- forward along the second-newest branch under the cursor, when
// there is one (PaintDocument::RedoAlt). RedoBranches answers how many ways
// forward there are, which is what a page shows the control on.
DEFINE_WORK_FUNC(PaintDocument, RedoAlt)
{
    (void)ctx; (void)data;
    const bool did = self.RedoAlt();
    ETCS_LOG("PaintDocument", "redo alt: " << (did ? "stepped" : "nothing") << " ("
             << self.undoDepth() << " back, " << self.redoBranches() << " branch(es) forward)");
}
DEFINE_WORK_FUNC(PaintDocument, RedoBranches)
{
    (void)ctx;
    data.writeString(std::to_string(self.redoBranches()).c_str());
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, MoveSelection, (int32_t, dx), (int32_t, dy))
{
    (void)ctx;
    if (!self.MoveSelection(dx, dy))
        ETCS_LOG("PaintDocument", "MoveSelection with nothing selected, or no active layer.");
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RenderToSurface,
    (ETCS::RID, target), (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.RenderToSurface(target, x, y);
}

// ImportImage <path> / ExportImage <path> / ExportLayer <path> -- the rest of
// the line, as SetName takes it, because a path has dots and may have spaces;
// a quoted one is unquoted, since a script that quoted it meant the inside.
// The same three lines from the terminal on either substrate and from the
// page's two buttons (index.html), which write and read the browser's own
// filesystem and call these -- see the PAM note above PaintImage.
static inline std::string paint_path_arg(ETCS::Buffer& data)
{
    std::string s = data.restAsString();
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(ws);
    s = s.substr(a, b - a + 1);
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

DEFINE_WORK_FUNC(PaintDocument, ImportImage)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportImage needs a path."); return; }
    self.ImportImage(path);
}

// ImportCanvas <path> -- a new page the image's size, with the image on it.
// See PaintDocument::ImportCanvas.
// NewLayer -- an empty, page-sized layer above the active one, made active.
DEFINE_WORK_FUNC(PaintDocument, NewLayer)
{
    (void)ctx; (void)data;
    self.NewLayer();
}

DEFINE_WORK_FUNC(PaintDocument, ImportCanvas)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportCanvas needs a path."); return; }
    self.ImportCanvas(path);
}

DEFINE_WORK_FUNC(PaintDocument, ExportImage)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportImage needs a path."); return; }
    self.ExportImage(path);
}

// ExportOps <path> -- what this page made and has not sent, as lines; a whole
// baseline instead when the page itself changed (PaintDocument::ExportOps).
// Answers the count and the notebook head.
DEFINE_WORK_FUNC(PaintDocument, ExportOps)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path;
    in >> path;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportOps needs a path."); data.writeString("0"); return; }
    const size_t n = self.ExportOps(path);
    data.writeString((std::to_string(n) + " " + std::to_string(self.notebookHead())).c_str());
}

// ExportBaseline <path> -- the whole page as a session's starting point
// (PaintDocument::ExportBaseline). Answers the line count.
DEFINE_WORK_FUNC(PaintDocument, ExportBaseline)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path;
    in >> path;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportBaseline needs a path."); data.writeString("0"); return; }
    data.writeString(std::to_string(self.ExportBaseline(path)).c_str());
}

// TextDenied <key>, <holder> -- the node gave that box to somebody else
// (PaintDocument::TextDenied).
DEFINE_WORK_FUNC_TYPED(PaintDocument, TextDenied, (std::string, key), (std::string, holder))
{
    (void)ctx;
    self.TextDenied(key, holder);
}


// SetTextFont <id> <font> / SetTextSize <id> <px> / SetTextColor <id> <r> <g> <b> <a>
// -- the open box's type, as the text bar sets it (PaintDocument::SetTextFont).
DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextFont, (uint32_t, id), (uint32_t, font))
{
    (void)ctx;
    self.SetTextFont(id, font);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextSize, (uint32_t, id), (uint32_t, size))
{
    (void)ctx;
    self.SetTextSize(id, size);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextColor, (uint32_t, id), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetTextColor(id, r, g, b, a);
}

// Syncing <0|1> -- the room is putting this page right (PaintDocument::Syncing).
DEFINE_WORK_FUNC_TYPED(PaintDocument, Syncing, (int32_t, on))
{
    (void)ctx;
    self.Syncing(on != 0);
}

// Restate -- send the whole page to the room with the next push.
DEFINE_WORK_FUNC(PaintDocument, Restate)
{
    (void)ctx;
    self.Restate();
    data.writeString("1");
}

// SetReadOnly <0|1> -- view only: every edit refused here (a share never raises it).
DEFINE_WORK_FUNC_TYPED(PaintDocument, SetReadOnly, (int32_t, on))
{
    (void)ctx;
    self.SetReadOnly(on != 0);
}

// ImportOps <path> [since] [seed] -- every line of a read, `since` being where
// the read started (zero restarts the record chain, and then this page's own
// lines are applied too; a hex `seed` is the same restart from `since`, with the
// chain the record gave for it -- a read that began at its checkpoint).
// Answers "<taken> <head> <chain-hex> <chain-seq>
// <unreadable>": the page compares the chain with what the node said in the
// same read.
DEFINE_WORK_FUNC(PaintDocument, ImportOps)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path, seed_hex;
    uint64_t since = 0;
    in >> path >> since >> seed_hex;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportOps needs a path."); data.writeString("0"); return; }
    uint64_t seed = 0;
    if (!seed_hex.empty()) seed = std::strtoull(seed_hex.c_str(), nullptr, 16);
    const size_t n = self.ImportOps(path, since, seed_hex.empty() ? nullptr : &seed);
    char chain[17];
    std::snprintf(chain, sizeof(chain), "%016llx", static_cast<unsigned long long>(self.recordChain()));
    // taken head chain chain-seq unreadable
    data.writeString((std::to_string(n) + " " + std::to_string(self.notebookHead()) + " " + chain
                      + " " + std::to_string(self.recordChainSeq())
                      + " " + std::to_string(self.unreadable())).c_str());
}

// doc.Emit() -> record.Take() -- what this page makes, as it makes it, one
// entry per message; the whole page first when it has changed. Ends when the
// far side refuses (a turned key) or the link goes, and what did not go comes
// off (PaintShare::emitEnded).
DEFINE_STREAM_FUNC_PRODUCE_STANDING(PaintDocument, Emit)
{
    (void)data;
    // The reader going is noticed between sends (readerGone): a page with
    // nothing to draw would otherwise never learn its way in had closed.
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        const int state = self.emitState();
        if (state < 0) break;
        if (state == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        bool ended = false;
        for (const std::string& line : self.takeEmit())
            if (!stream.writeMessage(line)) { ended = true; break; }
        if (ended) break;
    }
    if (ETCS::Entity* share = self.share())
        static_cast<PaintShare*>(share->getTrueType())->emitEnded();
}
// record.Follow(<seq>) -> doc.Absorb() -- the record, every member's lines in
// its order, this page's own included (they confirm what it drew).
DEFINE_STREAM_FUNC_CONSUME(PaintDocument, Absorb)
{
    (void)data;
    std::string line;
    size_t n = 0;
    while (!ctx.isInterrupted() && stream.readMessage(line, 9u << 20)) { self.absorb(line); ++n; }
    ETCS_LOG("PaintDocument", "Absorb: the record ended after " << n << " line(s).");
    if (ETCS::Entity* share = self.share())
        static_cast<PaintShare*>(share->getTrueType())->absorbEnded();
}

// PictureReport -- PictureHash's parts, one per layer, for chasing a divergence.
DEFINE_WORK_FUNC(PaintDocument, PictureReport)
{
    (void)ctx;
    const std::string r = self.PictureReport();
    ETCS_LOG("PaintDocument", r);
    // The report is the log line; the answer is its length, since a report of
    // a few layers is past the data channel (256 bytes) already.
    data.writeString(std::to_string(r.size()).c_str());
}

// RevertPending -- the room refused this page's push: what it drew and the
// record does not hold comes off (PaintDocument::RevertPending). Answers 1 when
// anything did.
DEFINE_WORK_FUNC(PaintDocument, RevertPending)
{
    (void)ctx;
    data.writeString(self.RevertPending() ? "1" : "0");
}

/*
 * Accept <entry by reference> -- an entry from the record, applied: the
 * replay's one door (PaintDocument::AcceptOp). Reached from ImportOps as a
 * call, so a replayed change passes the same dispatch a made one does.
 */
DEFINE_WORK_FUNC(PaintDocument, Accept)
{
    (void)ctx;
    const PaintOp* op = PaintOpRef::Read(data);
    if (!op) { ETCS_LOG("PaintDocument", "Accept takes an entry by reference."); data.writeString("0"); return; }
    const bool ok = self.AcceptOp(*op);
    data.writeString(ok ? "1" : "0");
}

/*
 * Perform <entry> -- a change made here, from its entry: by reference, or as
 * the text of a record line without its sequence ("dab <author> <layer key>
 * <order> ..." -- a line from the record, replayed from a script). Applied
 * through the replay's own code and written down (PaintDocument::Perform).
 */
DEFINE_WORK_FUNC(PaintDocument, Perform)
{
    (void)ctx;
    PaintOp op;
    if (const PaintOp* ref = PaintOpRef::Read(data)) op = *ref;
    else if (!paint_op_decode("0 " + data.restAsString(), op))
    {
        ETCS_LOG("PaintDocument", "Perform: not an entry.");
        data.writeString("0");
        return;
    }
    const bool ok = self.Perform(std::move(op));
    data.writeString(ok ? "1" : "0");
}

// PictureHash -- what this page's picture is, as sixteen hex digits. Sent with
// presence so members can tell they have diverged (PaintDocument::PictureHash).
// Answers "<hex> <record-seq>": the picture AND the record position it is the
// picture OF (the last line this page chained, ImportOps), taken together --
// a picture paired with a position read at another moment is what made two
// members at one head disagree about a picture they shared.
// -- and whether the picture is the record's yet (settled: nothing of this
// page's is still to be pushed).
DEFINE_WORK_FUNC(PaintDocument, PictureHash)
{
    (void)ctx;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(self.PictureHash()));
    data.writeString((std::string(hex) + " " + std::to_string(self.recordChainSeq())
                      + (self.settled() ? " 1" : " 0")).c_str());
}

// The notebook's newest sequence, in this page's own numbering.
DEFINE_WORK_FUNC(PaintDocument, NotebookHead)
{
    (void)ctx;
    data.writeString(std::to_string(self.notebookHead()).c_str());
}

// Who authors entries from now on. Set to the name this page joined a session
// under, so a host's own entries carry the host's name rather than a blank.
DEFINE_WORK_FUNC(PaintDocument, SetAuthor)
{
    (void)ctx;
    self.SetAuthor(data.restAsString());
    data.writeString(self.author().c_str());
}

DEFINE_WORK_FUNC(PaintDocument, ExportLayer)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportLayer needs a path."); return; }
    self.ExportLayer(path);
}

// Resize <w> <h> <anchor 0..8> -- the page re-stated around its pixels, the
// anchored cell of a 3x3 grid staying put (0 top-left, 4 centre, 8
// bottom-right). New <w> <h> -- the same extent with every layer cleared.
// Neither is a history step: see PaintDocument::Resize. Render the surface
// after either, as after any scripted change.
DEFINE_WORK_FUNC_TYPED(PaintDocument, Resize, (uint32_t, w), (uint32_t, h), (int32_t, anchor))
{
    (void)ctx;
    self.Resize(w, h, anchor);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, New, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.New(w, h);
}

/*
 * MoveLayerTo <layer_rid> <depth> -- depth counted from the bottom, 0 being
 * first composited. What a dragged row means is a statement about the whole
 * stack, so this renumbers it densely; see PaintDocument::MoveLayerTo.
 */
DEFINE_WORK_FUNC_TYPED(PaintDocument, MoveLayerTo, (ETCS::RID, layer), (int32_t, depth))
{
    (void)ctx;
    self.MoveLayerTo(layer, depth);
}

// RenameLayer <layer_rid> <text...> -- RID first like every verb here that names
// an entity, then the rest of the line as the name.
DEFINE_WORK_FUNC(PaintDocument, RenameLayer)
{
    (void)ctx;
    ETCS::RID layer = 0;
    data >> layer;
    self.RenameLayer(layer, data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RemoveLayer, (ETCS::RID, layer))
{
    (void)ctx;
    self.RemoveLayer(layer);
}

/*
 * IsolateLayer <layer_rid> <dim> -- the hover. A RID of 0 is "nobody", which is
 * the same thing ClearIsolate says and the call a row's pointer-leave makes.
 */
DEFINE_WORK_FUNC_TYPED(PaintDocument, IsolateLayer, (ETCS::RID, layer), (float, dim))
{
    (void)ctx;
    self.IsolateLayer(layer, dim);
}

DEFINE_WORK_FUNC(PaintDocument, ClearIsolate)
{
    (void)ctx; (void)data;
    self.ClearIsolate();
}

DEFINE_WORK_FUNC(PaintDocument, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintDocument, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, Create, (ETCS::RID, target))
{
    (void)ctx;
    self.Create(target);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, AttachDocument, (ETCS::RID, doc))
{
    (void)ctx;
    self.AttachDocument(doc);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetTarget, (ETCS::RID, target))
{
    (void)ctx;
    self.SetTarget(target);
}

// ── the projection ───────────────────────────────────────────────────────
//
// Pan is where the document's origin sits in view space; zoom is how many view
// pixels one document pixel occupies. Both are verbs because both are driven
// from outside -- a wheel handler in the page, a +/- button in a toolbar, a
// script restoring a saved view.

/*
 * ── the peers' frames ───────────────────────────────────────────────────────
 *
 * ClearPeers then one SetPeer per participant, which is the shape the 256-byte
 * call buffer wants and the reason no file bridge is involved: a peer line is
 * about forty bytes, a roster of eight is over the ceiling, and one call each
 * is under it with room to spare.
 *
 * The rectangle is in DOCUMENT space. Each page draws it through its own
 * projection, so two people at different zooms see each other's frame in the
 * right place on the picture -- view-space pixels would only be correct for
 * whoever sent them.
 */
DEFINE_WORK_FUNC(PaintSurface, ClearPeers)
{
    (void)ctx; (void)data;
    self.ClearPeers();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetPeer,
                       (std::string, name),
                       (int32_t, x), (int32_t, y), (int32_t, w), (int32_t, h),
                       (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.SetPeer(name, x, y, w, h, r, g, b);
}

// This pane's own visible rectangle, in document space -- what the page sends
// to the node so everyone else can draw it. Derived from the projection on
// every call rather than remembered, so it cannot go stale behind a pan.
DEFINE_WORK_FUNC(PaintSurface, ViewRect)
{
    (void)ctx;
    int32_t x = 0, y = 0, w = 0, h = 0;
    self.ViewRect(x, y, w, h);
    data.writeString((std::to_string(x) + " " + std::to_string(y) + " "
                    + std::to_string(w) + " " + std::to_string(h)).c_str());
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetPan, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPan(x, y);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, PanBy, (int32_t, dx), (int32_t, dy))
{
    (void)ctx;
    self.PanBy(dx, dy);
    self.Render();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetZoom, (float, zoom))
{
    (void)ctx;
    self.SetZoom(zoom);
    self.Render();
}

/*
 * ZoomAt <zoom> <vx> <vy> -- hold the view point (vx,vy) fixed.
 *
 * THE VERB A SCROLL WHEEL WANTS, and the reason it is a verb at all: InputEvent
 * carries keys, positions and buttons, and has no scroll axis -- adding one is
 * an ontology change touching every input source. A work function is exported
 * and callable from the page's own wheel handler through etcs_web_call, which
 * is the same seam the toolbar already uses, so the wheel works today and the
 * ontology question stays open on its own merits.
 */
DEFINE_WORK_FUNC_TYPED(PaintSurface, ZoomAt,
    (float, zoom), (int32_t, vx), (int32_t, vy))
{
    (void)ctx;
    self.ZoomAt(zoom, vx, vy);
    self.Render();
    // Logged because the usual caller is the page's wheel handler, and a verb
    // driven from outside the runtime is one you cannot otherwise watch.
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY() << "  (about " << vx << "," << vy << ")");
}

// ZoomBy <factor> <vx> <vy> -- multiplicative, because zoom is perceived that
// way: 1.25 is one notch in at every magnification.
DEFINE_WORK_FUNC_TYPED(PaintSurface, ZoomBy,
    (float, factor), (int32_t, vx), (int32_t, vy))
{
    (void)ctx;
    self.ZoomBy(factor, vx, vy);
    self.Render();
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY() << "  (x" << factor
             << " about " << vx << "," << vy << ")");
}

// The TextLabel (or any Drawable2D exporting SetText) that shows the zoom. See
// PaintSurface::BindZoomLabel on why the surface pushes it.
// The glyph provider the edge ruler's numbers come from.
DEFINE_WORK_FUNC_TYPED(PaintSurface, BindGlyphs, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindGlyphs(glyphs);
}

// ShowEdgeRuler <0|1> -- the marks every 100 px around the drawable pane's edge.
DEFINE_WORK_FUNC_TYPED(PaintSurface, ShowEdgeRuler, (int32_t, on))
{
    (void)ctx;
    self.ShowEdgeRuler(on != 0);
}

// BindRulerFrame <rid> -- the surface the pane is inset in, which is where the
// ruler goes so that it is outside anywhere you can draw.
DEFINE_WORK_FUNC_TYPED(PaintSurface, BindRulerFrame, (ETCS::RID, frame))
{
    (void)ctx;
    self.BindRulerFrame(frame);
}

DEFINE_WORK_FUNC(PaintSurface, BindZoomLabel)
{
    (void)ctx;
    ETCS::RID label = 0;
    data >> label;
    self.BindZoomLabel(label);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetBackground,
    (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBackground(r, g, b, a);
}

// The readout a zoom widget shows, written back into the caller's buffer so a
// script or the page can display it without a second call.
DEFINE_WORK_FUNC(PaintSurface, ZoomPercent)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY());
    data.writeString(std::to_string(self.zoomPercent()).c_str());
}

DEFINE_WORK_FUNC(PaintSurface, Render)
{
    (void)ctx; (void)data;
    self.Render();
}

DEFINE_WORK_FUNC(PaintSurface, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintInput, Create,
    (ETCS::RID, document), (ETCS::RID, tool), (ETCS::RID, surface))
{
    (void)ctx;
    self.BindDocument(document);
    self.BindTool(tool);
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindDocument, (ETCS::RID, document))
{
    (void)ctx;
    self.BindDocument(document);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindTool, (ETCS::RID, tool))
{
    (void)ctx;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, SetBrush,
    (float, radius), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBrush(radius, r, g, b, a);
}

/*
 * The keyboard edge: presses begin and end strokes.
 *
 * `stream`, not `data` -- `data` is the config buffer delivered once when the
 * edge opens, and the events arrive on the stream. Blocking readRaw, because a
 * cross-tag pair is a pipe with a blocking consumer fd.
 */
/*
 * ── THE ROUTED EDGES ─────────────────────────────────────────────────────
 *
 * The same two channels, with the frame translation in front of them. Separate
 * verbs rather than a flag on the old ones because the two are genuinely
 * different contracts: ConsumeInput promises "these coordinates are the
 * canvas's", and this one promises "these coordinates are the window's and I
 * will find out whose they should be". A script binding a root and then reading
 * ConsumeInput in the ledger would be reading a lie.
 *
 * Structurally identical otherwise -- same drain, same break conditions, one
 * call different -- so nothing about the edge's behaviour has to be learned
 * twice.
 */
// ── PaintPalette ─────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintPalette, BindTool)
{
    (void)ctx;
    ETCS::RID tool = 0;
    data >> tool;
    self.BindTool(tool);
}

// AddColor <node_rid> <r> <g> <b> <a> -- the RID first, matching every other
// verb here that names an entity, and matching what a pick hands back.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddColor,
    (ETCS::RID, node), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.AddColor(node, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddSize, (ETCS::RID, node), (float, radius))
{
    (void)ctx;
    self.AddSize(node, radius);
}

// SetHoldCapacity <frames> -- how long a held stepper keeps stepping with no
// release in sight. 600 by default; see PaintPalette::AdvanceConcrete.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetHoldCapacity, (int32_t, n))
{
    (void)ctx;
    self.SetHoldCapacity(static_cast<uint16_t>(n < 1 ? 1 : n));
}

// ─────────────────────────────────────────────────────────────────────────────

// Create <doc_rid> <db_rid> -- the database must already be Connected: the
// schema is applied here, and a connection is the script's decision (a path
// is where a page store lives, and only the script knows the substrate).
DEFINE_WORK_FUNC_TYPED(PaintPages, Create, (ETCS::RID, document), (ETCS::RID, database))
{
    (void)ctx;
    self.Create(document, database);
}

// BindSurface <rid> -- the surface repainted when a page is loaded.
DEFINE_WORK_FUNC_TYPED(PaintPages, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC(PaintPages, Save)
{
    (void)ctx; (void)data;
    self.Save();
}

// Stash -- save the present if it changed, then leave its slot (PaintPages::Stash).
DEFINE_WORK_FUNC(PaintPages, Stash)
{
    (void)ctx;
    data.writeString(self.Stash() ? "ok" : "failed");
}

DEFINE_WORK_FUNC_TYPED(PaintPages, Load, (int64_t, id))
{
    (void)ctx;
    self.Load(id);
}

// Rename <id> <rest of line> -- the page's name in the store (and on the
// document when it is the page on screen).
DEFINE_WORK_FUNC_TYPED(PaintPages, Rename, (int64_t, id), (std::string, name))
{
    (void)ctx;
    self.Rename(id, name);
}

DEFINE_WORK_FUNC(PaintPages, New)
{
    (void)ctx; (void)data;
    self.New();
}

DEFINE_WORK_FUNC(PaintPages, Next)
{
    (void)ctx; (void)data;
    self.Next();
}

DEFINE_WORK_FUNC(PaintPages, Prev)
{
    (void)ctx; (void)data;
    self.Prev();
}

DEFINE_WORK_FUNC(PaintPages, List)
{
    (void)ctx; (void)data;
    self.List();
}

DEFINE_WORK_FUNC_TYPED(PaintPages, Delete, (int64_t, id))
{
    (void)ctx;
    self.Delete(id);
}

// The entity, not a page: Delete takes a page id, so the verb every other
// type spells Delete is Destroy here -- two meanings of one word on one type
// would make `pages.Delete(3)` a coin toss.
DEFINE_WORK_FUNC(PaintPages, Destroy)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// AddHoverLabel <entry_rid> <label_rid> -- a caption shown while that entry's
// group is hovered, hidden otherwise. See PaintPalette::AddHoverLabel.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddHoverLabel, (ETCS::RID, entry), (ETCS::RID, label))
{
    (void)ctx;
    self.AddHoverLabel(entry, label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddRadiusDelta, (ETCS::RID, node), (float, delta))
{
    (void)ctx;
    self.AddRadiusDelta(node, delta);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, BindWheel, (ETCS::RID, wheel))
{
    (void)ctx;
    self.BindWheel(wheel);
}

// AddWheelArrow <arrow node> <colour entry it picks for>
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddWheelArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddWheelArrow(node, slot);
}

// AddModeArrow <arrow node> <the select tool's entry> -- steps the mode.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddModeArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddModeArrow(node, slot);
}

// AddArrow <arrow node> <the cell it belongs to> -- wheel or mode, decided by
// what the cell already is. What paint_cell_arrow.etcs calls, since a script
// cannot be handed the word "wheel" (run carries RIDs only).
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddArrow(node, slot);
}

// SetShapeReadout <label> -- the shape slice's current outline, by name.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetShapeReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetShapeReadout(label);
}

// SetTipReadout <label> -- the brush slice's caption, which nib is loaded.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetTipReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetTipReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetModeReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetModeReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddAlphaDelta, (ETCS::RID, node), (float, delta_pct))
{
    (void)ctx;
    self.AddAlphaDelta(node, delta_pct);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetRadiusReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetRadiusReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetAlphaReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetAlphaReadout(label);
}

// AddTool <node_rid> <kind> -- a third thing a toolbar node can mean.
// AddZoom <node_rid> <factor> -- a node that steps the zoom. See
// PaintPalette::AddZoom on why a view setting is mapped by the same type that
// maps tool settings.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddZoom, (ETCS::RID, node), (float, factor))
{
    (void)ctx;
    self.AddZoom(node, factor);
}

DEFINE_WORK_FUNC(PaintPalette, BindSurface)
{
    (void)ctx;
    ETCS::RID surface = 0;
    data >> surface;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, PickTool, (std::string, kind))
{
    (void)ctx;
    self.PickTool(kind);
}

DEFINE_WORK_FUNC(PaintPalette, AddTool)
{
    (void)ctx;
    ETCS::RID node = 0;
    data >> node;
    self.AddTool(node, data.restAsString());
}

/*
 * SetColorOf <node_rid> <r> <g> <b> <a> -- replace what a swatch MEANS.
 *
 * The swatch's own fill is the caller's to change (SetFill on the drawable),
 * because a palette holds no drawables. Two calls for one intent, and that is
 * the honest split: this module owns the mapping, the tree owns the appearance.
 */
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetColorOf,
    (ETCS::RID, node), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    if (!self.SetColorOf(node, r, g, b, a))
        ETCS_LOG("PaintPalette", "SetColorOf RID:" << node
                 << " -- not a colour swatch of this palette.");
}

/*
 * AddCall <node_rid> <target_rid> <Tag.Action> <args...> -- pressing the node
 * calls that verb on that entity with the rest of the line as its argument
 * text, exactly as a script line would. The general entry; see
 * PaintPalette::AddCall for why the older kinds are not spelled with it.
 */
DEFINE_WORK_FUNC(PaintPalette, AddCall)
{
    (void)ctx;
    ETCS::RID node = 0, target = 0;
    std::string action;
    data >> node >> target >> action;
    self.AddCall(node, target, action, data.restAsString());
}

// AddPopup <node_rid> <pane_rid> <input_rid> -- pressing the node opens the
// pane (routed through that input) and pressing anywhere closes it. BindRouter
// <rid> is what it opens into.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddPopup, (ETCS::RID, node), (ETCS::RID, pane), (ETCS::RID, input))
{
    (void)ctx;
    self.AddPopup(node, pane, input);
}

// OpenPopup <pane_rid> <input_rid> / ClosePopup -- the popup nothing was
// pressed for. See PaintPalette::OpenPopup.
DEFINE_WORK_FUNC_TYPED(PaintPalette, OpenPopup, (ETCS::RID, pane), (ETCS::RID, input))
{
    (void)ctx;
    self.OpenPopup(pane, input);
}

DEFINE_WORK_FUNC(PaintPalette, ClosePopup)
{
    (void)ctx; (void)data;
    self.ClosePopup();
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, BindRouter, (ETCS::RID, router))
{
    (void)ctx;
    self.BindRouter(router);
}

DEFINE_WORK_FUNC(PaintPalette, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintPalette, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintLayerPanel ──────────────────────────────────────────────────────
//
// Rows are declared by the script that draws them; everything else here is a
// look the script states or an interaction the input edge routes in.

DEFINE_WORK_FUNC(PaintLayerPanel, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindDocument, (ETCS::RID, document))
{
    (void)ctx;
    self.BindDocument(document);
}

// ── PaintAnimation ─────────────────────────────────────────────────────────
//
// A region of the page and a reel of frames its size; see PaintAnimation. The
// window's buttons are palette calls to these verbs (paint_anim.etcs).
DEFINE_WORK_FUNC_TYPED(PaintAnimation, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindPreview, (ETCS::RID, node))
{
    (void)ctx;
    self.BindPreview(node);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindReadout, (ETCS::RID, node))
{
    (void)ctx;
    self.BindReadout(node);
}

// BindTitle <node> -- a press here carries the window (PaintAnimation::PressTitle).
DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindTitle, (ETCS::RID, node))
{
    (void)ctx;
    self.BindTitle(node);
}

DEFINE_WORK_FUNC(PaintAnimation, BeginTitle) { (void)ctx; (void)data; self.BeginTitle(); }
DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindPalette, (ETCS::RID, palette)) { (void)ctx; self.BindPalette(palette); }
DEFINE_WORK_FUNC_TYPED(PaintAnimation, Fold, (int32_t, folded)) { (void)ctx; self.Fold(folded != 0); }
DEFINE_WORK_FUNC(PaintAnimation, Close)  { (void)ctx; (void)data; self.Close(); }
DEFINE_WORK_FUNC(PaintAnimation, Open)   { (void)ctx; (void)data; self.Open(); }

DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

DEFINE_WORK_FUNC(PaintAnimation, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

// SetRegion x0 y0 x1 y1 -- the frame, in document pixels; what the tool's
// drag calls, and a script's way of saying the same.
DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetRegion, (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SetRegion(x0, y0, x1, y1);
}

DEFINE_WORK_FUNC(PaintAnimation, Snap)   { (void)ctx; (void)data; self.Snap(); }
DEFINE_WORK_FUNC(PaintAnimation, Put)    { (void)ctx; (void)data; self.Put(); }
DEFINE_WORK_FUNC(PaintAnimation, Next)   { (void)ctx; (void)data; self.Next(); }
DEFINE_WORK_FUNC(PaintAnimation, Prev)   { (void)ctx; (void)data; self.Prev(); }
DEFINE_WORK_FUNC(PaintAnimation, Play)   { (void)ctx; (void)data; self.Play(); }
DEFINE_WORK_FUNC(PaintAnimation, Pause)  { (void)ctx; (void)data; self.Pause(); }
DEFINE_WORK_FUNC(PaintAnimation, Toggle) { (void)ctx; (void)data; self.Toggle(); }
DEFINE_WORK_FUNC(PaintAnimation, Refresh){ (void)ctx; (void)data; self.Refresh(); }
DEFINE_WORK_FUNC(PaintAnimation, Download){ (void)ctx; (void)data; self.Download(); }
DEFINE_WORK_FUNC(PaintAnimation, Report) { (void)ctx; (void)data; self.Report(); }
DEFINE_WORK_FUNC(PaintAnimation, Delete) { (void)ctx; (void)data; self.DeleteConcrete(); }

// Remove <index> / Select <index> -- one-based, as the rows show them.
DEFINE_WORK_FUNC_TYPED(PaintAnimation, Remove, (int32_t, index))
{
    (void)ctx;
    if (index >= 1) self.Remove(static_cast<size_t>(index - 1));
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, Select, (int32_t, index))
{
    (void)ctx;
    if (index >= 1) self.Select(static_cast<size_t>(index - 1));
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetFps, (int32_t, fps))
{
    (void)ctx;
    self.SetFps(fps);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, StepFps, (int32_t, by))
{
    (void)ctx;
    self.StepFps(by);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

// ImportGif <path> / ExportGif <path> -- the reel in and out as a GIF.
DEFINE_WORK_FUNC(PaintAnimation, ImportGif)
{
    (void)ctx;
    self.ImportGif(data.restAsString());
}

DEFINE_WORK_FUNC(PaintAnimation, ExportGif)
{
    (void)ctx;
    self.ExportGif(data.restAsString());
}

// ── PaintPagePanel ─────────────────────────────────────────────────────────
//
// The store as a list: rows assembled by paint_page_row.etcs, a press on a
// row loads that page, its x deletes it, a second press on the name of the
// page on screen renames it. See PaintPagePanel.
DEFINE_WORK_FUNC(PaintPagePanel, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindPages, (ETCS::RID, pages))
{
    (void)ctx;
    self.BindPages(pages);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindMenu, (ETCS::RID, menu))
{
    (void)ctx;
    self.BindMenu(menu);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

DEFINE_WORK_FUNC(PaintPagePanel, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

// Scroll <rows> -- negative toward the newest page.
DEFINE_WORK_FUNC_TYPED(PaintPagePanel, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

DEFINE_WORK_FUNC(PaintPagePanel, Refresh)
{
    (void)ctx; (void)data;
    self.Refresh();
}

DEFINE_WORK_FUNC(PaintPagePanel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintPagePanel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// AddRow <bg> <eye> <thumb> <label> <delete> -- top of the window first,
// matching the order a script lays them out in. Any of the five may be 0; the
// thumbnail is a raster the panel paints the layer into (see paint_thumb).
DEFINE_WORK_FUNC(PaintLayerPanel, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

// RowNode <what> <rid> -- attaches one part to the row BeginRow opened. See the
// header note for why a row is assembled rather than declared: a script cannot
// hand another script a node it spawned, so a row that needed all its parts in
// one call could only ever live in one file.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindView, (ETCS::RID, node))
{
    (void)ctx;
    self.BindView(node);
}

// BeginTitle -- until the first BeginRow, an eye registered by RowNode is the
// title bar's view toggle rather than a row's. See RowNode.
DEFINE_WORK_FUNC(PaintLayerPanel, BeginTitle)
{
    (void)ctx; (void)data;
    self.BeginTitle();
}

// BindBody <node> -- hidden with the rows when the window is collapsed.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindBody, (ETCS::RID, node))
{
    (void)ctx;
    self.BindBody(node);
}

// BindAdd <node> -- pressing it adds a layer above the active one
// (PaintDocument::NewLayer).
// BindSurface <surface> -- what to re-render when the window changes the
// picture: a restack, an eye, a delete (PaintLayerPanel::BindSurface).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindAdd, (ETCS::RID, node))
{
    (void)ctx;
    self.BindAdd(node);
}

// BindTitle <node> -- a press here drags the window; BindWindow <pane> -- the
// pane it drags (PaintLayerPanel::BindTitle). Several handles may be bound.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindTitle, (ETCS::RID, handle))
{
    (void)ctx;
    self.BindTitle(handle);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetHoverDim, (float, dim))
{
    (void)ctx;
    self.SetHoverDim(dim);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetEyeColors,
    (float, vr), (float, vg), (float, vb), (float, hr), (float, hg), (float, hb))
{
    (void)ctx;
    self.SetEyeColors(vr, vg, vb, hr, hg, hb);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetIrisColors,
    (float, vr), (float, vg), (float, vb), (float, hr), (float, hg), (float, hb))
{
    (void)ctx;
    self.SetIrisColors(vr, vg, vb, hr, hg, hb);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

// SetDragColor <r> <g> <b> -- the carried row and its ghost (PaintLayerPanel::DragRow).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetDragColor, (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.SetDragColor(r, g, b);
}

// BindGhost <pane> / GhostNode <thumb|label> <node> / BindDropMark <node> --
// what a row drag shows (PaintLayerPanel::BindGhost).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindGhost, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindGhost(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, GhostNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.GhostNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindDropMark, (ETCS::RID, node))
{
    (void)ctx;
    self.BindDropMark(node);
}

// In ROWS, so a wheel notch is +/-1 and nothing outside has to know the row
// height. Negative scrolls toward the top of the stack.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

DEFINE_WORK_FUNC(PaintLayerPanel, Refresh)
{
    (void)ctx; (void)data;
    self.Refresh();
}

// CommitRename <text...> -- the rest of the line, applied to whichever layer a
// second press on a label armed. Says so when nothing is armed rather than
// renaming something arbitrary.
DEFINE_WORK_FUNC(PaintLayerPanel, CommitRename)
{
    (void)ctx;
    if (!self.CommitRename(data.restAsString()))
        ETCS_LOG("PaintLayerPanel", "CommitRename with no rename armed -- press a "
                 "layer's name twice first.");
}

// The four row actions, addressed by index rather than by a synthesised pick --
// see PaintLayerPanel::SelectRow for why a caller should not have to pretend to
// be a mouse. All of them are exported, so the page's JS reaches them through
// etcs_web_call exactly as a script does.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SelectRow, (uint32_t, row))
{
    (void)ctx;
    self.SelectRow(row);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, ToggleRow, (uint32_t, row))
{
    (void)ctx;
    self.ToggleRow(row);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, RemoveRow, (uint32_t, row))
{
    (void)ctx;
    self.RemoveRow(row);
}

// MoveRow <from> <to> -- the layer at `from` takes the depth of the row at
// `to`, which is the drag a pointer would have performed.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, MoveRow, (uint32_t, from), (uint32_t, to))
{
    (void)ctx;
    self.MoveRow(from, to);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, ArmRename, (uint32_t, row))
{
    (void)ctx;
    self.ArmRename(row);
}

// HoverRow <row> -- any out-of-range index, -1 by convention, means "the
// pointer is nowhere near this window" and clears the isolation.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, HoverRow, (int32_t, row))
{
    (void)ctx;
    self.HoverRow(row);
}

DEFINE_WORK_FUNC(PaintLayerPanel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintLayerPanel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintColorWheel ──────────────────────────────────────────────────────
//
// Geometry and bindings are the script's; the pick and the replacement are this
// type's. Open/Close are verbs so the page or a toolbar button can raise it,
// which is the usual way it gets opened.

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, Create,
    (int32_t, cx), (int32_t, cy), (uint32_t, radius))
{
    (void)ctx;
    self.Create(cx, cy, radius);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindTool)
{
    (void)ctx;
    ETCS::RID tool = 0;
    data >> tool;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindPalette)
{
    (void)ctx;
    ETCS::RID palette = 0;
    data >> palette;
    self.BindPalette(palette);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindRouter)
{
    (void)ctx;
    ETCS::RID router = 0;
    data >> router;
    self.BindRouter(router);
}

// BindPane <root_rid> <input_rid> -- what gets added to the router when this
// opens. Open means "in the routing set"; see the class note.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, BindPane, (ETCS::RID, root), (ETCS::RID, input))
{
    (void)ctx;
    self.BindPane(root, input);
}

// The third dimension a disc cannot carry. Without it the picker cannot reach
// anything dark.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, SetValue, (float, v))
{
    (void)ctx;
    self.SetValue(v);
}

// OpenAt <x> <y> <slot> -- open the wheel above a control, aimed at one swatch.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, OpenAt,
                       (int32_t, x), (int32_t, y), (ETCS::RID, slot))
{
    (void)ctx;
    self.OpenAt(x, y, slot);
}

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, SetTargetSlot, (ETCS::RID, slot))
{
    (void)ctx;
    self.SetTargetSlot(slot);
}

DEFINE_WORK_FUNC(PaintColorWheel, Open)
{
    (void)ctx; (void)data;
    self.Open();
}

DEFINE_WORK_FUNC(PaintColorWheel, Close)
{
    (void)ctx; (void)data;
    self.Close();
}

// Pick <x> <y> in the wheel's own space -- the pointer path calls this itself,
// so this verb is for a page or a test that has a coordinate and no pointer.
// Pick <x> <y>, in the pane's own space. Logs which of the three things it was,
// because "nothing happened" and "the brightness moved" look the same otherwise.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, Pick, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    const PaintPick r = self.Pick(x, y);
    ETCS_LOG("PaintColorWheel", "pick at " << x << "," << y << " -> "
             << (r == PaintPick::Picked   ? "colour"
               : r == PaintPick::Adjusted ? "value strip" : "missed"));
}

DEFINE_WORK_FUNC(PaintColorWheel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintColorWheel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintCanvasMenu ──────────────────────────────────────────────────────
//
// The menu's look is paint_menu.etcs; every verb here is what one of its
// nodes says when pressed (PaintPalette::AddCall), and every one is equally a
// line a script or the page can type.

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

// StepWidth <delta> / StepHeight <delta> -- in pixels, clamped to 64..8192.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, StepWidth, (int32_t, delta))
{
    (void)ctx;
    self.StepWidth(delta);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, StepHeight, (int32_t, delta))
{
    (void)ctx;
    self.StepHeight(delta);
}

// SetAnchor <0..8> -- which cell of the 3x3 grid stays put on resize.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, SetAnchor, (int32_t, anchor))
{
    (void)ctx;
    self.SetAnchor(anchor);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindWidthReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.BindWidthReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindHeightReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.BindHeightReadout(label);
}

// BindAnchorCell <index 0..8> <node_rid> -- the cell's fill shows whether it
// is the chosen one, from now on.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnchorCell, (int32_t, index), (ETCS::RID, node))
{
    (void)ctx;
    self.BindAnchorCell(index, node);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ApplyResize)
{
    (void)ctx; (void)data;
    self.ApplyResize();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ApplyNew)
{
    (void)ctx; (void)data;
    self.ApplyNew();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Save)
{
    (void)ctx; (void)data;
    self.Save();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Load)
{
    (void)ctx; (void)data;
    self.Load();
}

// BindImportPrompt <palette> <pane> <input> <caption> -- the popup that asks
// what an arriving file is for; OfferImport <path> asks it. See
// PaintCanvasMenu::OfferImport.
// BindPages <pages> -- the store `new` adds to and save writes; the list of
// pages is PaintPagePanel's. See PaintCanvasMenu's pages note.
// BindAnchorArrow <index> <label> -- the arrow drawn on that cell, pointing
// away from whichever cell is chosen (PaintCanvasMenu::BindAnchorArrow).
// SetExtent <w> <h> -- both numbers at once, for the presets
// (PaintCanvasMenu::SetExtent).
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, SetExtent, (int32_t, w), (int32_t, h))
{
    (void)ctx;
    self.SetExtent(w, h);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnchorArrow,
    (int32_t, index), (ETCS::RID, label))
{
    (void)ctx;
    self.BindAnchorArrow(index, label);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindPages, (ETCS::RID, pages))
{
    (void)ctx;
    self.BindPages(pages);
}

// BindAnimation <anim> -- where a multi-frame GIF goes on upload.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnimation, (ETCS::RID, anim))
{
    (void)ctx;
    self.BindAnimation(anim);
}

// BindPagePanel <panel> -- the PaintPagePanel on this pane, re-read after a
// new page or a save.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindPagePanel, (ETCS::RID, panel))
{
    (void)ctx;
    self.BindPagePanel(panel);
}

// PageChanged -- the page on screen was swapped by something else (the list);
// the readouts follow it.
DEFINE_WORK_FUNC(PaintCanvasMenu, PageChanged)
{
    (void)ctx; (void)data;
    self.PageChanged();
}

// BindTool <tool> -- what an import leaves selected (PaintCanvasMenu::BindTool).
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindTool, (ETCS::RID, tool))
{
    (void)ctx;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindImportPrompt,
    (ETCS::RID, palette), (ETCS::RID, pane), (ETCS::RID, input), (ETCS::RID, caption))
{
    (void)ctx;
    self.BindImportPrompt(palette, pane, input, caption);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, OfferImport)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintCanvasMenu", "OfferImport needs a path."); return; }
    self.OfferImport(path);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportAsLayer)
{
    (void)ctx; (void)data;
    self.ImportAsLayer();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportAsCanvas)
{
    (void)ctx; (void)data;
    self.ImportAsCanvas();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportCancel)
{
    (void)ctx; (void)data;
    self.ImportCancel();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintRouter ──────────────────────────────────────────────────────────
//
// The arbiter's script surface. Panes are added by RID pairs and the priority is
// never stated here -- it is the roots' own SetOrder, read per event. See the
// class for why that is one relation rather than two.

DEFINE_WORK_FUNC_TYPED(PaintRouter, Create, (uint32_t, passes))
{
    (void)ctx;
    self.Create(passes);
}

// AddPane <root_rid> <input_rid> -- what to hit-test, and who gets the hit.
DEFINE_WORK_FUNC_TYPED(PaintRouter, AddPane, (ETCS::RID, root), (ETCS::RID, input))
{
    (void)ctx;
    self.AddPane(root, input);
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, RemovePane, (ETCS::RID, root))
{
    (void)ctx;
    self.RemovePane(root);
}

// How many panes one event may be consumed by. 1 is the topmost pane alone; 2
// lets the pane underneath see it too.
DEFINE_WORK_FUNC_TYPED(PaintRouter, SetPassBudget, (uint32_t, passes))
{
    (void)ctx;
    self.SetPassBudget(passes);
}

// Key <glfw keycode> -- the same edge the key ring drives, reachable from a
// script. Down only: nothing acts on release (PaintRouter::RouteKey).
DEFINE_WORK_FUNC_TYPED(PaintRouter, Key, (int32_t, key))
{
    (void)ctx;
    self.RouteKey(static_cast<uint16_t>(key), true);
}

// Editing -- "1" while any pane wants keys (a box open, a name field), else
// "0". What a page polls to raise the soft keyboard (PaintInput::editingText).
DEFINE_WORK_FUNC(PaintRouter, Editing)
{
    (void)ctx;
    data.writeString(self.Editing() ? "1" : "0");
}

// Type <text> -- the text as key presses, for a keyboard that sends characters
// (PaintRouter::Type). The payload is the rest of the line, spaces included.
DEFINE_WORK_FUNC(PaintRouter, Type)
{
    (void)ctx;
    self.Type(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, Pointer, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.ScriptPointer(x, y);
}

DEFINE_WORK_FUNC(PaintRouter, Press)
{
    (void)ctx; (void)data;
    self.ScriptPress();
}

DEFINE_WORK_FUNC(PaintRouter, Release)
{
    (void)ctx; (void)data;
    self.ScriptRelease();
}

// PressButton <n> / ReleaseButton <n> -- 0 left, 1 right (PAINT_BUTTON_*).
// Press/Release above are the left-button shorthands, which is what a bare
// "press" means everywhere else.
DEFINE_WORK_FUNC_TYPED(PaintRouter, PressButton, (uint32_t, button))
{
    (void)ctx;
    self.ScriptPressButton(static_cast<uint16_t>(button));
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, ReleaseButton, (uint32_t, button))
{
    (void)ctx;
    self.ScriptReleaseButton(static_cast<uint16_t>(button));
}

DEFINE_WORK_FUNC(PaintRouter, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintRouter, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

/*
 * THE EDGE A MULTI-PANE SESSION BINDS INSTEAD OF PaintInput's.
 *
 * Shaped exactly like PaintInput::ConsumePointer -- same channel, same filter --
 * because from the window's side nothing has changed: it is still one producer
 * writing positions and presses. What changed is that the far end arbitrates
 * before it interprets, so a session grows a second pane by adding a pane here
 * rather than by binding a second consumer to the same producer and hoping.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintRouter, ConsumePointer)
{
    (void)data;

    ETCS_LOG("PaintRouter::ConsumePointer", "routed pointer edge open on RID:" << self.getRID()
             << ", " << self.paneCount() << " pane(s), budget " << self.passBudget());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action != INPUT_MOTION && ev.action != INPUT_SCROLL
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.Route(ev);
    }

    ETCS_LOG("PaintRouter::ConsumePointer", "routed pointer edge closed.");
}

/*
 * The key channel, arbitrated the same way. A press arrives here with no position
 * of its own (ontology/InputSource.h), so the walk uses the last routed position
 * -- the same rule PaintInput::ScriptPress follows, for the same reason.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintRouter, ConsumeInput)
{
    (void)data;

    ETCS_LOG("PaintRouter::ConsumeInput", "routed key edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        // Keys as keys -- see PaintRouter::RouteKey for what this used to do.
        if (ev.action == INPUT_DOWN)      self.RouteKey(ev.key, true);
        else if (ev.action == INPUT_UP)   self.RouteKey(ev.key, false);
    }

    ETCS_LOG("PaintRouter::ConsumeInput", "routed key edge closed.");
}

// ── PaintInput routing ───────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintInput, BindRoot)
{
    (void)ctx;
    ETCS::RID root = 0;
    data >> root;
    self.BindRoot(root);
}

// SetHoldCapacity <n> -- how many unconfirmed events a held button survives
// before this input releases it itself. 4 for paint; raise it for anything that
// treats a hold as a mode. See HeldCharge (ontology/InputSource.h).
DEFINE_WORK_FUNC_TYPED(PaintInput, SetHoldCapacity, (int32_t, n))
{
    (void)ctx;
    self.SetHoldCapacity(static_cast<uint16_t>(n < 1 ? 1 : n));
}

// BindAnimation <anim> -- the reel the animation tool's drag frames (PaintInput::BindAnimation).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindAnimation, (ETCS::RID, anim))
{
    (void)ctx;
    self.BindAnimation(anim);
}

// BindTextBar <bar> -- the bar over an open text box (PaintInput::BindTextBar).
// Undo / Redo -- the pane's step: the document's, then the view repainted. What
// the undo and redo buttons under the picture call (boot_paint_panels.etcs).
DEFINE_WORK_FUNC(PaintInput, Undo)
{
    (void)ctx; (void)data;
    self.Undo();
}
DEFINE_WORK_FUNC(PaintInput, Redo)
{
    (void)ctx; (void)data;
    self.Redo();
}
// RedoAlt -- forward along the older branch (PaintDocument::RedoAlt), and the
// view repainted, as Redo above.
DEFINE_WORK_FUNC(PaintInput, RedoAlt)
{
    (void)ctx; (void)data;
    self.RedoAlt();
}
// BindRedoAlt <node> -- a part of the control that offers it (once per part),
// hidden while the history has no second way forward (PaintInput::BindRedoAlt).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindRedoAlt, (ETCS::RID, node))
{
    (void)ctx;
    self.BindRedoAlt(node);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindTextBar, (ETCS::RID, bar))
{
    (void)ctx;
    self.BindTextBar(bar);
}

// BindVisitors <visitors> -- the sharing window on this pane (PaintInput::BindVisitors).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindVisitors, (ETCS::RID, visitors))
{
    (void)ctx;
    self.BindVisitors(visitors);
}

// BindPagePanel <panel> -- the page list on this pane (PaintInput::BindPagePanel).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindPagePanel, (ETCS::RID, panel))
{
    (void)ctx;
    self.BindPagePanel(panel);
}

// BindPages <rid> -- the page store the ctrl+PageUp/PageDown chords switch.
DEFINE_WORK_FUNC(PaintInput, BindPages)
{
    (void)ctx;
    ETCS::RID pages = 0;
    data >> pages;
    self.BindPages(pages);
}

DEFINE_WORK_FUNC(PaintInput, BindCanvas)
{
    (void)ctx;
    ETCS::RID canvas = 0;
    data >> canvas;
    self.BindCanvas(canvas);
}

// Any leaf claiming Glyphs -- RenderProvider::TextLabel today. Needed only by
// the glyph tool; every other kind ignores it.
DEFINE_WORK_FUNC(PaintInput, BindGlyphs)
{
    (void)ctx;
    ETCS::RID glyphs = 0;
    data >> glyphs;
    self.BindGlyphs(glyphs);
}

DEFINE_WORK_FUNC(PaintInput, BindWheel)
{
    (void)ctx;
    ETCS::RID wheel = 0;
    data >> wheel;
    self.BindWheel(wheel);
}

// 1 on the wheel's own pane (a press picks), 0 on the canvas (a press
// dismisses). Two inputs bind the same wheel and mean different things by it --
// see PaintInput::BindWheel.
DEFINE_WORK_FUNC_TYPED(PaintInput, SetWheelPane, (int32_t, is_wheel))
{
    (void)ctx;
    self.SetWheelPane(is_wheel != 0);
}

DEFINE_WORK_FUNC(PaintInput, BindPanel)
{
    (void)ctx;
    ETCS::RID panel = 0;
    data >> panel;
    self.BindPanel(panel);
}

DEFINE_WORK_FUNC(PaintInput, BindPalette)
{
    (void)ctx;
    ETCS::RID palette = 0;
    data >> palette;
    self.BindPalette(palette);
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeRouted)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeRouted", "press edge open on RID:" << self.getRID()
             << " -- events are routed through the 2D tree before they mean anything.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_MOTION) continue;
        /*
     * A KEY EVENT CARRIES NO POSITION (ontology/InputSource.h: x/y are
     * meaningful for INPUT_MOTION only), so routing one on its own
     * coordinates would pick whatever sits at the origin. The last position
     * the pointer channel delivered is where the press happened, which is
     * the same assumption the unrouted path already makes and the same one
     * absolute positions make safe.
     */
        ev.x = static_cast<int16_t>(self.RoutedCursorX());
        ev.y = static_cast<int16_t>(self.RoutedCursorY());
        self.RouteEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeRouted", "press edge closed.");
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeRoutedPointer)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeRoutedPointer", "pointer edge open on RID:"
             << self.getRID() << " -- window frame in, node frame out.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        // Buttons ride this ring too, and carry their own position -- so this
        // edge is where a click becomes a stroke, not just where the brush
        // follows the cursor.
        if (ev.action != INPUT_MOTION
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.NoteRoutedCursor(ev.x, ev.y);
        self.RouteEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeRoutedPointer", "pointer edge closed.");
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeInput)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeInput", "key edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_MOTION) continue;
        self.HandleEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeInput", "key edge closed.");
}

/*
 * The pointer edge: positions place the brush.
 *
 * TWO EDGES, and the correlation cost is nil because the position is ABSOLUTE.
 * A press means "begin a stroke where the pointer is", and the pointer's
 * position is already known from the last sample -- it does not have to arrive
 * in the same stream, or in any particular order relative to the press. The
 * worst a split costs is that a press lands on a position one sample old, which
 * at pointer rates is invisible; what it buys is that a keystroke never queues
 * behind a burst of motion.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumePointer)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumePointer", "pointer edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        // Buttons share this channel and carry their own position, so an
        // unrouted session gets click-and-drag for free.
        if (ev.action != INPUT_MOTION
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.HandleEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumePointer", "pointer edge closed.");
}

// Pointer <x> <y> / Press / Release -- a stroke without a device. See
// PaintInput::ScriptPointer.
DEFINE_WORK_FUNC_TYPED(PaintInput, Pointer, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.ScriptPointer(x, y);
}

DEFINE_WORK_FUNC(PaintInput, Press)
{
    (void)ctx; (void)data;
    self.ScriptPress();
}

DEFINE_WORK_FUNC(PaintInput, Release)
{
    (void)ctx; (void)data;
    self.ScriptRelease();
}

// Report the machine's state, so a caller can tell a refused stroke from a
// completed one -- Press before the pointer has ever been seen is refused on
// purpose (HandleEvent), and silently.
DEFINE_WORK_FUNC(PaintInput, Report)
{
    (void)data; (void)ctx;
    /*
     * THE TOOL'S STATE, not just the cursor's. This reported two numbers and a
     * flag, which answers almost nothing you would ask it: the questions that
     * actually come up are what is loaded and how it will mark -- which kind,
     * what size, what colour, at what opacity.
     */
    ETCS_LOG("PaintInput::Report", "cursor (" << self.cursorX() << ", " << self.cursorY()
             << ")  stroke " << (self.StrokeActive() ? "ACTIVE" : "idle"));
    if (PaintTool* t = self.tool())
    {
        const PaintBrushState& br = t->brush();
        ETCS_LOG("PaintInput::Report", "  tool " << paint_tool_kind_name(t->kind())
                 << (t->kind() == PaintToolKind::Select
                     ? std::string(" (") + paint_select_mode_name(t->mode()) + ")" : std::string())
                 << " size " << br.size_px
                 << " colour " << br.color.r << ", " << br.color.g << ", " << br.color.b
                 << " alpha " << t->alphaPercent() << "% (" << br.color.a << ")");
    }
    else ETCS_LOG("PaintInput::Report", "  no tool bound.");
}

DEFINE_WORK_FUNC(PaintInput, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintFonts ──────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintFonts, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

// BindPixel <glyphs> -- font 0, the sheet's own lettering.
DEFINE_WORK_FUNC_TYPED(PaintFonts, BindPixel, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindPixel(glyphs);
}

// Load <name> <path> -- a TrueType file, as the next font number.
DEFINE_WORK_FUNC(PaintFonts, Load)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string name, path;
    in >> name >> path;
    if (!name.empty() && name.back() == ',') name.pop_back();
    const bool ok = self.Load(name, path);
    data.writeString(ok ? std::to_string(self.count() - 1).c_str() : "missing");
}

DEFINE_WORK_FUNC(PaintFonts, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintFonts, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintTextBar ────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(PaintTextBar, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

// BindFont <node> <font> -- a button meaning that font.
DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindFont, (ETCS::RID, node), (uint32_t, font))
{
    (void)ctx;
    self.BindFont(node, font);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindSizeLabel, (ETCS::RID, node))
{
    (void)ctx;
    self.BindSizeLabel(node);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindColor, (ETCS::RID, node), (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.BindColor(node, r, g, b);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, SetFontTints,
                       (float, r0), (float, g0), (float, b0), (float, r1), (float, g1), (float, b1))
{
    (void)ctx;
    self.SetFontTints(r0, g0, b0, r1, g1, b1);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, PickFont, (ETCS::RID, node))
{
    (void)ctx;
    self.PickFont(node);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, StepSize, (int32_t, by))
{
    (void)ctx;
    self.StepSize(by);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, PickColor, (ETCS::RID, node))
{
    (void)ctx;
    self.PickColor(node);
}

DEFINE_WORK_FUNC(PaintTextBar, Done)
{
    (void)ctx; (void)data;
    self.Done();
}

DEFINE_WORK_FUNC(PaintTextBar, Remove)
{
    (void)ctx; (void)data;
    self.Remove();
}

DEFINE_WORK_FUNC(PaintTextBar, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintVisitors ───────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintVisitors, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC(PaintVisitors, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

// BeginTitle -- until the first BeginRow, an eye registered by RowNode is the
// window's fold toggle (paint_eye.etcs on the bar).
DEFINE_WORK_FUNC(PaintVisitors, BeginTitle)
{
    (void)ctx; (void)data;
    self.BeginTitle();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetRowColors, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetRowColors(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetInk,
                       (float, wr), (float, wg), (float, wb),
                       (float, rr), (float, rg), (float, rb))
{
    (void)ctx;
    self.SetInk(wr, wg, wb, rr, rg, rb);
}

// The roster as the node answered it. Small by construction -- "luke reader"
// is twelve bytes -- but this crosses the 256-byte call buffer, so a session
// larger than about twenty is where the page should start sending it the way
// the notebook crosses, as a file.
DEFINE_WORK_FUNC(PaintVisitors, SetRoster)
{
    (void)ctx;
    self.SetRoster(data.restAsString());
    data.writeString(std::to_string(self.count()).c_str());
}

DEFINE_WORK_FUNC(PaintVisitors, Open)
{
    (void)ctx; (void)data;
    self.Open();
}

// OpenAs <owner|writer|reader> -- the window for what this page is in the
// session (PaintVisitors::OpenAs).
DEFINE_WORK_FUNC_TYPED(PaintVisitors, OpenAs, (std::string, role))
{
    (void)ctx;
    self.OpenAs(role);
}

// The session is over and the page knows it: down, with nothing sent back.
DEFINE_WORK_FUNC(PaintVisitors, Hide)
{
    (void)ctx; (void)data;
    self.Hide();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindTitle, (ETCS::RID, node))
{
    (void)ctx;
    self.BindTitle(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindHostOnly, (ETCS::RID, node))
{
    (void)ctx;
    self.BindHostOnly(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindGuestOnly, (ETCS::RID, node))
{
    (void)ctx;
    self.BindGuestOnly(node);
}

// BindMe <name label> <colour chip> -- the "you" line.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindMe, (ETCS::RID, name), (ETCS::RID, chip))
{
    (void)ctx;
    self.BindMe(name, chip);
}

// BindSwatch <node> <r> <g> <b> -- a colour this page can be seen in.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindSwatch, (ETCS::RID, node), (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.BindSwatch(node, r, g, b);
}

// SetMe <name> <rrggbb> -- from the page, which holds both.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetMe, (std::string, name), (std::string, hex))
{
    (void)ctx;
    self.SetMe(name, hex);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, PickColor, (ETCS::RID, node))
{
    (void)ctx;
    self.PickColor(node);
}

DEFINE_WORK_FUNC(PaintVisitors, EditName)
{
    (void)ctx; (void)data;
    self.EditName();
}

DEFINE_WORK_FUNC(PaintVisitors, CopyLink)
{
    (void)ctx; (void)data;
    self.CopyLink();
}

// The end button: the host's ends the session, a guest's leaves it -- the page
// hears which and tells the node. Nothing here knows that, which is the point:
// this raises the event and the page decides what it means.
DEFINE_WORK_FUNC(PaintVisitors, Close)
{
    (void)ctx; (void)data;
    self.Close();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Promote, (ETCS::RID, node))
{
    (void)ctx;
    self.Promote(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Demote, (ETCS::RID, node))
{
    (void)ctx;
    self.Demote(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Remove, (ETCS::RID, node))
{
    (void)ctx;
    self.Remove(node);
}

DEFINE_WORK_FUNC(PaintVisitors, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintShare ──────────────────────────────────────────────────────────────

// Attach <doc> <canvas> <visitors> -- this page's own.
DEFINE_WORK_FUNC_TYPED(PaintShare, Attach, (ETCS::RID, doc), (ETCS::RID, canvas), (ETCS::RID, visitors))
{
    (void)ctx;
    self.Attach(doc, canvas, visitors);
}
// Host <record> <intake> <seal> <presence> <room> <name>
DEFINE_WORK_FUNC_TYPED(PaintShare, Host, (ETCS::RID, record), (ETCS::RID, intake), (ETCS::RID, seal),
                       (ETCS::RID, presence), (ETCS::RID, room), (std::string, name))
{
    (void)ctx;
    self.Host(record, intake, seal, presence, room, name);
}
// Join <record> <intake> <seal> <presence> <peer> <name>
DEFINE_WORK_FUNC_TYPED(PaintShare, Join, (ETCS::RID, record), (ETCS::RID, intake), (ETCS::RID, seal),
                       (ETCS::RID, presence), (ETCS::RID, peer), (std::string, name))
{
    (void)ctx;
    self.Join(record, intake, seal, presence, peer, name);
}
// Key -- the intake's key, for the host to post.
DEFINE_WORK_FUNC(PaintShare, Key) { (void)ctx; data.reset(); data.writeString(self.Key().c_str()); }
// Role <who> writer|reader|out -- the host's word.
DEFINE_WORK_FUNC_TYPED(PaintShare, Role, (std::string, who), (std::string, role))
{
    (void)ctx;
    self.Role(who, role);
}
// Writers -- names, space-separated.
DEFINE_WORK_FUNC(PaintShare, Writers) { (void)ctx; data.reset(); data.writeString(self.Writers().c_str()); }
// Hue <hex> -- this page's colour in the room.
DEFINE_WORK_FUNC_TYPED(PaintShare, Hue, (std::string, hex)) { (void)ctx; self.Hue(hex); }
// Tick -- once a second, from the page.
DEFINE_WORK_FUNC(PaintShare, Tick)  { (void)ctx; (void)data; self.Tick(); }
// Leave -- out of the session; the link is the script's to close.
DEFINE_WORK_FUNC(PaintShare, Leave) { (void)ctx; (void)data; self.Leave(); }
DEFINE_WORK_FUNC(PaintShare, Delete) { (void)ctx; (void)data; self.Delete(); }
// presence.Watch() -> share.Roster() -- the listing, whole, on every change.
DEFINE_STREAM_FUNC_CONSUME(PaintShare, Roster)
{
    (void)data;
    std::string listing;
    while (!ctx.isInterrupted() && stream.readMessage(listing)) self.roster(listing);
}
// mail.Follow(0) -> share.Mail() -- what the host posts this page.
DEFINE_STREAM_FUNC_CONSUME(PaintShare, Mail)
{
    (void)data;
    std::string line;
    while (!ctx.isInterrupted() && stream.readMessage(line)) self.mail(line);
}

#endif // PAINTPROVIDER_H__
