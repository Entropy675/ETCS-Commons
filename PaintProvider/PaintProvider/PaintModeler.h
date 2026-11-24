#ifndef PAINTPROVIDER_PAINTMODELER_H__
#define PAINTPROVIDER_PAINTMODELER_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintVisitors.h"   // in order: everything above this in the module is visible here

#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/*
 * ── PaintModeler: the hand on a 3D model ────────────────────────────────────
 *
 * The page's other half: where PaintSurface shows a document, a Camera3D shows
 * a scene, and this is what a pointer on that camera means. Shaped like
 * GolfGame -- a scene, a camera, a pointer -- and it knows RenderProvider the
 * same way: nodes by family, writes as verbs on the node's own tag.
 *
 * THE MODEL IS ITS LINES, as the battle is in KartBattle. A part is one slot
 * of a pool of Scene3D nodes the script spawned (paint_model_part.etcs) and
 * every change to it is a line:
 *
 *   add   <id> <shape> <x y z> <hx hy hz> <r g b a> <qw qx qy qz>
 *   del   <id>
 *   move  <id> <x y z>
 *   turn  <id> <qw qx qy qz>
 *   size  <id> <hx hy hz>
 *   color <id> <r g b a>
 *   shape <id> <shape>
 *
 * Numbers are Fixed raws (ontology/Fixed.h), stated outright -- a line says
 * where a part IS, never how far it went -- so the table below is the same
 * integers on every runtime fed the same lines, and the nodes are its picture.
 * A drag is drawn as it goes and is not a line; letting go is one.
 *
 * UNDO IS A LINE TOO. Applying a line yields the line that puts back what it
 * replaced; undo appends that one, and what THAT replaced is the redo. Nothing
 * is taken out of a record, so a session's members undo by the same path they
 * edit by.
 *
 * ONE JUDGES. Alone, this is its own judge and keeps no record. Hosting, each
 * line it takes is appended to the record (a Record_, by family). A guest's
 * lines go out as proposals (`model.Emit() -> proposals.Take()`), the host
 * judges them (`proposals.Follow(0) -> model.Judge()`), and a guest's table
 * moves only by the record (`record.Follow(0) -> model.Absorb()`).
 *
 * WHAT IS ONE RUNTIME'S ALONE is not a line: the eye, the tool, the selection
 * and the axes drawn on it.
 */
class PaintModeler : public DeletableBase<PaintModeler>
{
public:
    WIRE_TYPE_IDENTITY(PaintModeler);

    PaintModeler() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    bool Create() { this->addTag("active"); return true; }

    // ── what the script hands over ───────────────────────────────────────

    // The container the parts are children of. Its centre is the model's
    // origin: a part's position is stated from it (Scene3D's own rule).
    void BindWorld(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_world = r; }
    void BindCamera(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_camera = r; placeCameraLocked(); }
    // Where add and paint take their colour: the page's one brush.
    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        std::lock_guard<std::mutex> g(m_mu);
        m_tool_src = raw ? tool : 0;
    }
    // The page's 2D view, drawn again when the model closes: the camera sits
    // in the page's own pane (paint_model.etcs), whose raster is retained.
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        std::lock_guard<std::mutex> g(m_mu);
        m_surface = raw ? static_cast<PaintSurface*>(raw->getTrueType()) : nullptr;
    }
    void BindReadout(ETCS::RID label) { std::lock_guard<std::mutex> g(m_mu); m_readout = label; m_readout_text.clear(); readoutLocked(); }
    // The layout the page's panes are in: solved again when the mode flips, so
    // the bar that is shown takes the row and the hidden one takes nothing.
    void BindLayout(ETCS::RID layout) { std::lock_guard<std::mutex> g(m_mu); m_layout = layout; }
    // The page's share: while it is in a session, a guest's line is taken only
    // from a writer (PaintShare::Writers), as a page's strokes are.
    void BindShare(ETCS::RID share) { std::lock_guard<std::mutex> g(m_mu); m_share = share; }
    /*
     * A CELL OF THE BAR, AND WHAT IT SAYS. The bar is script (paint_model_bar.
     * etcs) and every press on it is a verb; what the bar SHOWS is this type's,
     * because only it knows what is in hand. `role` is a tool's name -- that
     * cell is lit while it is the tool -- or one of axis, snap (lit while one
     * is set), shape, export, mode. A caption is the word under a cell that
     * says its current value: the shape, the axis, on/off, the format, and on
     * the mode cell the page the press goes TO.
     */
    void BindCell(const std::string& role, ETCS::RID node)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_cells.emplace_back(role, node);
        m_cell_lit.push_back(-1);
        cellsLocked();
    }
    void BindCaption(const std::string& role, ETCS::RID label)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_captions.emplace_back(role, label);
        m_caption_text.emplace_back();
        cellsLocked();
    }
    // A node of the pool: a Scene3D under the world, hidden until a line adds it.
    void AddPart(ETCS::RID node)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_nodes.push_back(node);
        m_parts.emplace_back();
        verb(node, "SetVisible", "0");
    }
    // A shape a part can be drawn as: a RenderProvider Mesh, by name. "box" is
    // every node's own and needs none.
    void AddShape(const std::string& name, ETCS::RID mesh)
    {
        if (name.empty() || name == "box" || name.find(' ') != std::string::npos) return;
        std::lock_guard<std::mutex> g(m_mu);
        for (auto& s : m_shapes) if (s.first == name) { s.second = mesh; return; }
        m_shapes.emplace_back(name, mesh);
    }
    // The three arrows drawn on the selected part: 0 x, 1 y, 2 z.
    void BindAxis(uint32_t axis, ETCS::RID node)
    {
        if (axis > 2) return;
        std::lock_guard<std::mutex> g(m_mu);
        m_axis_node[axis] = node;
        verb(node, "SetVisible", "0");
    }
    // What the page shows in each mode: a node listed under 2 is hidden while
    // the model is open, one under 3 while it is closed.
    void AddPane(uint32_t mode, ETCS::RID node)
    {
        std::lock_guard<std::mutex> g(m_mu);
        (mode == 3 ? m_panes3 : m_panes2).push_back(node);
    }

    // ── the page's mode ──────────────────────────────────────────────────

    bool open() const { return m_open.load(); }
    // A press this pane took and has not yet let go of.
    bool dragging() { std::lock_guard<std::mutex> g(m_mu); return m_drag != Drag_::Idle; }
    void Open()   { show(true); }
    void Close()  { show(false); }
    void Toggle() { show(!m_open.load()); }

    // ── the tool ─────────────────────────────────────────────────────────

    // select | move | turn | size | add | paint | delete
    bool SetTool(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const Tool t = toolFrom(name);
        if (t == Tool::None) return false;
        cancelDragLocked();
        m_tool = t;
        readoutLocked();
        return true;
    }
    // x | y | z | none: the line a move slides along, a turn turns about and a
    // size stretches. None is the view plane, the up axis and all three.
    bool SetAxis(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        int a;
        if      (name == "x") a = 0;
        else if (name == "y") a = 1;
        else if (name == "z") a = 2;
        else if (name == "none") a = -1;
        else return false;
        m_axis = (a == m_axis) ? -1 : a;   // the same one again lets go of it
        axesLocked();
        readoutLocked();
        return true;
    }
    // The shape add makes; outside the add tool, the selected part's too. In
    // it, a part still chosen from before is not what the press is about.
    bool SetShape(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!shapeKnownLocked(name)) return false;
        m_shape = name;
        if (m_tool != Tool::Add && m_sel >= 0 && m_parts[m_sel].shape != name)
            submitLocked("shape " + std::to_string(m_sel) + " " + name, Why::Edit);
        readoutLocked();
        return true;
    }
    // A material a part may be made of, by name (a game's: what it weighs is
    // the game's to say). "-" is none, and every part's to begin with.
    void AddMaterial(const std::string& name)
    {
        if (name.empty() || name == "-" || name.find(' ') != std::string::npos) return;
        std::lock_guard<std::mutex> g(m_mu);
        if (!materialKnownLocked(name)) m_materials.push_back(name);
        cellsLocked();
    }
    // The material add makes a part of; outside the add tool, the selected
    // part's too -- as SetShape.
    bool SetMaterial(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!materialKnownLocked(name)) return false;
        m_material = name;
        if (m_tool != Tool::Add && m_sel >= 0 && m_parts[m_sel].material != name)
            submitLocked("material " + std::to_string(m_sel) + " " + name, Why::Edit);
        readoutLocked();
        cellsLocked();
        return true;
    }
    // A grid to land on: positions and sizes on multiples of `step` (scene
    // units), turns on multiples of `degrees`. 0 is free.
    void SetSnap(float step, float degrees)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_snap = step > 0.0f ? step : 0.0f;
        m_turn_snap = degrees > 0.0f ? degrees : 0.0f;
    }

    // The bar's steppers: each press is the next one round.
    void CycleMaterial()
    {
        std::string next;
        {
            std::lock_guard<std::mutex> g(m_mu);
            std::vector<std::string> names{ "-" };
            names.insert(names.end(), m_materials.begin(), m_materials.end());
            const std::string& now = (m_tool != Tool::Add && m_sel >= 0) ? m_parts[m_sel].material : m_material;
            size_t i = 0;
            while (i < names.size() && names[i] != now) ++i;
            next = names[(i + 1) % names.size()];
        }
        SetMaterial(next);
    }
    void CycleShape()
    {
        std::string next;
        {
            std::lock_guard<std::mutex> g(m_mu);
            std::vector<std::string> names{ "box" };
            for (auto& sh : m_shapes) names.push_back(sh.first);
            const std::string& now = (m_tool != Tool::Add && m_sel >= 0) ? m_parts[m_sel].shape : m_shape;
            size_t i = 0;
            while (i < names.size() && names[i] != now) ++i;
            next = names[(i + 1) % names.size()];
        }
        SetShape(next);
    }
    void CycleAxis()
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_axis = (m_axis >= 2) ? -1 : m_axis + 1;
        axesLocked();
        readoutLocked();
    }
    // The grid on (a quarter unit, fifteen degrees) or off.
    void ToggleSnap()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const bool on = m_snap > 0.0f || m_turn_snap > 0.0f;
        m_snap = on ? 0.0f : 0.25f;
        m_turn_snap = on ? 0.0f : 15.0f;
        readoutLocked();
    }
    void CycleFormat()
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_format = m_format == "glb" ? "obj" : (m_format == "obj" ? "stl" : "glb");
        readoutLocked();
    }
    /*
     * OUT, AS THE BAR'S FORMAT. Beside the runtime on a desktop (model.<fmt>
     * in the working directory, the log says where); in the browser the
     * page's to hand over -- it calls Export into its own filesystem and gives
     * the bytes to the browser, as it does a GIF (index.html, downloadModel).
     */
    std::string ExportHere()
    {
        std::string fmt;
        { std::lock_guard<std::mutex> g(m_mu); fmt = m_format; }
#if defined(__EMSCRIPTEN__)
        const std::string what = "model-" + fmt;
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-menu', { detail: UTF8ToString($0) }));
        }, what.c_str());
        return "page";
#else
        return Export("model." + fmt);
#endif
    }

    // ── the eye ──────────────────────────────────────────────────────────

    void SetOrbit(float distance, float yaw_deg, float pitch_deg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (distance > 0.0f) m_dist = clampf(distance, kMinDist, kMaxDist);
        m_yaw   = yaw_deg * kPi / 180.0f;
        m_pitch = clampf(pitch_deg * kPi / 180.0f, -kMaxPitch, kMaxPitch);
        placeCameraLocked();
    }
    void LookAt(float x, float y, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_target = V3{ x, y, z };
        placeCameraLocked();
    }

    // ── the pointer ──────────────────────────────────────────────────────
    //
    // In the camera frame's own pixels, as PaintInput hands a hit on it over.
    // Press/Drag/Release/Wheel are the same events as verbs, so a script plays
    // a gesture with no window.
    void Press(float x, float y)   { Pointer(event(INPUT_BUTTON_DOWN, x, y)); }
    void Drag(float x, float y)    { Pointer(event(INPUT_MOTION, x, y)); }
    void Release(float x, float y) { Pointer(event(INPUT_BUTTON_UP, x, y)); }
    void Wheel(float notches)      { std::lock_guard<std::mutex> g(m_mu); dollyLocked(notches); }

    void Pointer(const InputEvent& ev)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const float x = ev.x, y = ev.y;
        switch (ev.action)
        {
            case INPUT_DOWN:
            case INPUT_BUTTON_DOWN:
                if (m_drag != Drag_::Idle) break;
                m_button = ev.key;
                m_press_x = m_last_x = x; m_press_y = m_last_y = y;
                m_dragged = false;
                pressLocked(x, y);
                break;
            case INPUT_MOTION:
                // Let go where nobody told us: a drag ended unseen is put down
                // where it started, since where it was let go is not known.
                if (m_drag != Drag_::Idle && input_button_state(ev, m_button) == 0) { cancelDragLocked(); break; }
                if (m_drag == Drag_::Idle) break;
                if (std::fabs(x - m_press_x) + std::fabs(y - m_press_y) > 2.0f) m_dragged = true;
                dragLocked(x, y);
                m_last_x = x; m_last_y = y;
                break;
            case INPUT_UP:
            case INPUT_BUTTON_UP:
                if (m_drag == Drag_::Idle || ev.key != m_button) break;
                releaseLocked();
                break;
            case INPUT_SCROLL:
                dollyLocked(static_cast<float>(ev.y));
                break;
            default: break;
        }
    }

    // A key while the model is open: true when it was this pane's.
    bool KeyDown(uint16_t key)
    {
        constexpr uint16_t KEY_ESCAPE = 256, KEY_BACKSPACE = 259, KEY_DELETE = 261;
        if (paint_modifiers().ctrl())
        {
            if (key == 'Z') { if (paint_modifiers().shift()) Redo(); else Undo(); return true; }
            if (key == 'Y') { Redo(); return true; }
            return false;
        }
        switch (key)
        {
            case KEY_ESCAPE: { std::lock_guard<std::mutex> g(m_mu); cancelDragLocked(); selectLocked(-1); return true; }
            case KEY_DELETE:
            case KEY_BACKSPACE:
            {
                std::lock_guard<std::mutex> g(m_mu);
                if (m_sel >= 0) submitLocked("del " + std::to_string(m_sel), Why::Edit);
                return true;
            }
            case 'Q': return SetTool("select");
            case 'G': return SetTool("move");
            case 'R': return SetTool("turn");
            case 'S': return SetTool("size");
            case 'A': return SetTool("add");
            case 'P': return SetTool("paint");
            case 'X': return SetAxis("x");
            case 'Y': return SetAxis("y");
            case 'Z': return SetAxis("z");
            default:  return false;
        }
    }

    // ── lines ────────────────────────────────────────────────────────────

    // A line, as this runtime's own edit: what a tool would have said. As a
    // verb it carries what fits a Buffer, which every line a tool makes does.
    bool Do(const std::string& line)
    {
        std::lock_guard<std::mutex> g(m_mu);
        return submitLocked(line, Why::Edit);
    }
    bool Undo()
    {
        std::lock_guard<std::mutex> g(m_mu);
        cancelDragLocked();
        while (!m_undo.empty())
        {
            const std::string line = std::move(m_undo.back());
            m_undo.pop_back();
            if (submitLocked(line, Why::Undo)) return true;   // refused: someone else's line got there first
        }
        return false;
    }
    bool Redo()
    {
        std::lock_guard<std::mutex> g(m_mu);
        cancelDragLocked();
        while (!m_redo.empty())
        {
            const std::string line = std::move(m_redo.back());
            m_redo.pop_back();
            if (submitLocked(line, Why::Redo)) return true;
        }
        return false;
    }

    // The model as it stands, as the lines that make it: one add a part.
    std::string Lines() { std::lock_guard<std::mutex> g(m_mu); return linesLocked(); }
    // The same lines to a file, and back -- as edits, so a load can be undone
    // and a session sees it like any other change.
    bool Save(const std::string& path)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { ETCS_LOG("PaintModeler", "Save: cannot open " << path); return false; }
        out << Lines();
        return static_cast<bool>(out);
    }
    bool Load(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { ETCS_LOG("PaintModeler", "Load: cannot open " << path); return false; }
        std::lock_guard<std::mutex> g(m_mu);
        cancelDragLocked();
        for (size_t i = 0; i < m_parts.size(); ++i)
            if (m_parts[i].used) submitLocked("del " + std::to_string(i), Why::Edit);
        std::string line;
        bool all = true;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (!submitLocked(line, Why::Edit)) { ETCS_LOG("PaintModeler", "Load: refused '" << line << "'"); all = false; }
        }
        return all;
    }
    // The world's own export (Scene3D.Export): the format is the path's ending.
    std::string Export(const std::string& path)
    {
        ETCS::RID world;
        { std::lock_guard<std::mutex> g(m_mu); world = m_world; }
        return ask(world, "Export", path);
    }

    // ── readers ──────────────────────────────────────────────────────────

    uint32_t Count() { std::lock_guard<std::mutex> g(m_mu); return countLocked(); }
    int32_t  Selected() { std::lock_guard<std::mutex> g(m_mu); return m_sel; }
    // The table as one number: two runtimes fed the same lines say the same.
    std::string Hash()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const std::string s = linesLocked();
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(XXH3_64bits(s.data(), s.size())));
        return hex;
    }
    // "<alone|host|guest> <tool> <axis> <selected> <parts> <seq> <undo> <redo>"
    std::string Status()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::ostringstream o;
        o << (!m_session ? "alone" : (m_judge ? "host" : "guest")) << ' ' << toolName(m_tool) << ' '
          << (m_axis < 0 ? "none" : (m_axis == 0 ? "x" : (m_axis == 1 ? "y" : "z"))) << ' ' << m_sel << ' '
          << countLocked() << ' ' << m_seq << ' ' << m_undo.size() << ' ' << m_redo.size();
        return o.str();
    }
    // A part, for a reader: "<shape> <x y z> <w h d> <r g b a>" in scene units, or "-".
    std::string Report(uint32_t id)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (id >= m_parts.size() || !m_parts[id].used) return "-";
        const Part& p = m_parts[id];
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f %.3f", p.shape.c_str(),
                      p.x.ToDouble(), p.y.ToDouble(), p.z.ToDouble(),
                      p.hx.ToDouble() * 2.0, p.hy.ToDouble() * 2.0, p.hz.ToDouble() * 2.0,
                      p.col[0].ToDouble(), p.col[1].ToDouble(), p.col[2].ToDouble(), p.col[3].ToDouble());
        return buf;
    }

    // ── sessions ─────────────────────────────────────────────────────────

    // The record is here and this model judges it. The model as it stands goes
    // into the record first, so a follower starts from what the host has.
    bool Host(ETCS::RID record, ETCS::RID proposals, const std::string& name)
    {
        if (name.empty() || !record) return false;
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        m_me = name; m_judge = true; m_session = true;
        m_record = record; m_proposals = proposals;
        std::istringstream in(linesLocked());
        std::string line;
        while (std::getline(in, line)) if (!line.empty()) recordLocked(m_me, line);
        return true;
    }
    // This model follows a host's record from its first line.
    bool Join(const std::string& name)
    {
        if (name.empty()) return false;
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        restartLocked();
        m_me = name; m_judge = false; m_session = true;
        return true;
    }
    // Alone again, the model kept as it stands.
    void Leave()
    {
        std::lock_guard<std::mutex> g(m_mu);
        ++m_gen;   // the streams of the session going end with it
        m_me.clear(); m_judge = true; m_session = false;
        m_record = m_proposals = 0; m_seq = 0;
        m_outbox.clear(); m_pending.clear();
    }

    // EACH STREAM IS ITS SESSION'S (KartBattle.h): it takes the session's
    // number when it starts and ends when the model is in another.
    uint64_t Generation() { std::lock_guard<std::mutex> g(m_mu); return m_gen; }
    bool nextEmit(uint64_t gen, std::string& out, bool& stale)
    {
        std::lock_guard<std::mutex> g(m_mu);
        stale = gen != m_gen;
        if (stale || m_outbox.empty()) return false;
        out = std::move(m_outbox.front());
        m_outbox.pop_front();
        return true;
    }
    // A line of the record, followed: "<seq> <author> <line>".
    bool Absorb(uint64_t gen, const std::string& msg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen) return false;
        if (m_judge || msg.compare(0, 2, "~ ") == 0) return true;
        uint64_t seq = 0; std::string author, line;
        if (!parseLine(msg, seq, author, line)) return true;
        std::string inverse;
        const bool took = applyLocked(line, inverse);
        if (!took) ETCS_LOG("PaintModeler", "line " << seq << " (" << author << " " << line << ") refused here -- this model is not the record's.");
        m_seq = seq + 1;
        if (author == m_me) landedMineLocked(line, took, inverse);
        return true;
    }
    // A proposal, judged: applied and recorded if taken.
    bool JudgeLine(uint64_t gen, const std::string& msg)
    {
        ETCS::RID proposals = 0, share = 0; uint64_t seq = 0;
        std::string author, line;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (gen != m_gen) return false;
            if (!m_judge || !m_session || msg.compare(0, 2, "~ ") == 0) return true;
            if (!parseLine(msg, seq, author, line) || author.empty() || author == m_me) return true;
            share = m_share;
        }
        // A page in a session takes a line only from a writer. Asked with no
        // lock held: the share is another type's, reached by its verb.
        bool may = true;
        if (share)
        {
            std::istringstream in(ask(share, "Writers", ""));
            std::string w;
            may = false;
            while (in >> w) if (w == author) { may = true; break; }
        }
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (gen != m_gen) return false;
            std::string inverse;
            if (!may) ETCS_LOG("PaintModeler", author << " is not a writer here -- '" << line << "' refused.");
            else if (applyLocked(line, inverse)) recordLocked(author, line);
            proposals = m_proposals;
        }
        if (Record_* p = recordOf(proposals)) p->Checkpoint(seq + 1);
        return true;
    }

private:
    enum class Tool  : uint8_t { None, Select, Move, Turn, Size, Add, Paint, Delete };
    enum class Drag_ : uint8_t { Idle, Orbit, Pan, Move, Turn, Size };
    enum class Why   : uint8_t { Edit, Undo, Redo };

    struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
    // A part as the lines left it. The position is from the world's centre.
    struct Part
    {
        bool        used = false;
        std::string shape = "box";
        std::string material = "-";   // what it is made of (AddMaterial); "-" none
        Fixed       x, y, z;
        Fixed       hx = Fixed::From(0.5), hy = Fixed::From(0.5), hz = Fixed::From(0.5);
        Fixed       col[4] = { Fixed::From(0.8), Fixed::From(0.8), Fixed::From(0.85), Fixed::One() };
        Fixed       qw = Fixed::One(), qx, qy, qz;
    };

    static constexpr float  kPi       = 3.14159265358979f;
    static constexpr float  kTurn     = 0.006f;     // radians per pixel: an orbit, and a turn
    static constexpr float  kMaxPitch = 1.45f;
    static constexpr float  kMinDist  = 0.5f, kMaxDist = 400.0f;
    static constexpr float  kGrow     = 0.01f;      // a size drag: e^(pixels * this)
    static constexpr double kMinHalf  = 0.005, kMaxHalf = 2048.0;
    static constexpr size_t kHistory  = 512;

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static V3    add(V3 a, V3 b)       { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
    static V3    sub(V3 a, V3 b)       { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
    static V3    mul(V3 a, float s)    { return V3{ a.x * s, a.y * s, a.z * s }; }
    static float dot(V3 a, V3 b)       { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static V3    cross(V3 a, V3 b)     { return V3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    static V3    axisOf(int a)         { return V3{ a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f }; }
    static V3    posOf(const Part& p)  { return V3{ p.x.ToFloat(), p.y.ToFloat(), p.z.ToFloat() }; }
    static V3    halfOf(const Part& p) { return V3{ p.hx.ToFloat(), p.hy.ToFloat(), p.hz.ToFloat() }; }
    // v by the part's facing, or back out of it.
    static V3 turned(const Part& p, V3 v, bool inverse)
    {
        OrderVector o;
        o.qw = p.qw; o.qx = p.qx; o.qy = p.qy; o.qz = p.qz;
        Fixed fx = Fixed::From(v.x), fy = Fixed::From(v.y), fz = Fixed::From(v.z);
        if (inverse) o.UnrotateVector(fx, fy, fz); else o.RotateVector(fx, fy, fz);
        return V3{ fx.ToFloat(), fy.ToFloat(), fz.ToFloat() };
    }
    static InputEvent event(uint8_t action, float x, float y)
    {
        InputEvent e{};
        e.action = action;
        e.key = 0;
        e.x = static_cast<int16_t>(x); e.y = static_cast<int16_t>(y);
        e.buttons = action == INPUT_BUTTON_UP ? 0 : 1;
        return e;
    }
    static Tool toolFrom(const std::string& n)
    {
        if (n == "select") return Tool::Select;
        if (n == "move")   return Tool::Move;
        if (n == "turn")   return Tool::Turn;
        if (n == "size")   return Tool::Size;
        if (n == "add")    return Tool::Add;
        if (n == "paint")  return Tool::Paint;
        if (n == "delete") return Tool::Delete;
        return Tool::None;
    }
    static const char* toolName(Tool t)
    {
        switch (t)
        {
            case Tool::Select: return "select";
            case Tool::Move:   return "move";
            case Tool::Turn:   return "turn";
            case Tool::Size:   return "size";
            case Tool::Add:    return "add";
            case Tool::Paint:  return "paint";
            case Tool::Delete: return "delete";
            default:           return "none";
        }
    }

    // ── the page's mode ──────────────────────────────────────────────────

    void show(bool on)
    {
        std::vector<ETCS::RID> two, three;
        PaintSurface* surface = nullptr;
        ETCS::RID layout = 0;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (m_open.load() == on) return;
            m_open.store(on);
            cancelDragLocked();
            two = m_panes2; three = m_panes3;
            surface = m_surface;
            layout = m_layout;
            if (on) placeCameraLocked();
            cellsLocked();
        }
        // No lock across these: hidden is a flag, and a flag change is ordered.
        // Hide before show, so the two bars never both hold the row.
        for (ETCS::RID n : (on ? two : three)) paint_node_hidden(n, true);
        for (ETCS::RID n : (on ? three : two)) paint_node_hidden(n, false);
        if (layout) verb(layout, "Solve", "");
        if (!on && surface) surface->Render();
    }

    // ── the eye ──────────────────────────────────────────────────────────

    bool frameLocked(float& w, float& h, ViewFrustum& v)
    {
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return false;
        void* plane = static_cast<ETCS::Entity*>(cam.get())->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        if (!plane) return false;
        const Rect2D f = static_cast<Drawable2D_*>(plane)->Bounds();
        if (f.w == 0 || f.h == 0) return false;
        w = static_cast<float>(f.w); h = static_cast<float>(f.h);
        v = cam->GetView();
        return true;
    }
    // The line through a pointer on the camera's frame (ontology/Camera.h).
    bool rayLocked(float px, float py, V3& o, V3& d)
    {
        float w, h; ViewFrustum v;
        if (!frameLocked(w, h, v)) return false;
        Point3D po, pd;
        if (!ViewRay(v, px / w, py / h, w / h, po, pd)) return false;
        o = V3{ po.x, po.y, po.z }; d = V3{ pd.x, pd.y, pd.z };
        return true;
    }
    V3 eyeLocked() const
    {
        const float cp = std::cos(m_pitch);
        return V3{ m_target.x - m_dist * cp * std::sin(m_yaw), m_target.y + m_dist * std::sin(m_pitch), m_target.z - m_dist * cp * std::cos(m_yaw) };
    }
    void placeCameraLocked()
    {
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        ViewFrustum v = cam->GetView();
        const V3 e = eyeLocked();
        v.position = Point3D{ e.x, e.y, e.z };
        v.look_at  = Point3D{ m_target.x, m_target.y, m_target.z };
        v.up       = Point3D{ 0.0f, 1.0f, 0.0f };
        cam->SetView(v);
    }
    void dollyLocked(float notches)
    {
        m_dist = clampf(m_dist * std::exp(-0.1f * notches), kMinDist, kMaxDist);
        placeCameraLocked();
    }

    // ── picking ──────────────────────────────────────────────────────────

    // Where a ray enters a part's box, in the part's own frame: every shape
    // is picked as the box it fills.
    static bool hitPart(const Part& p, V3 o, V3 d, float& t)
    {
        const V3 lo = turned(p, sub(o, posOf(p)), true), ld = turned(p, d, true), h = halfOf(p);
        const float oo[3] = { lo.x, lo.y, lo.z }, dd[3] = { ld.x, ld.y, ld.z }, hh[3] = { h.x, h.y, h.z };
        float t0 = 0.0f, t1 = 1e30f;
        for (int i = 0; i < 3; ++i)
        {
            if (std::fabs(dd[i]) < 1e-8f) { if (std::fabs(oo[i]) > hh[i]) return false; continue; }
            float a = (-hh[i] - oo[i]) / dd[i], b = (hh[i] - oo[i]) / dd[i];
            if (a > b) std::swap(a, b);
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return false;
        }
        t = t0;
        return true;
    }
    int pickLocked(V3 o, V3 d, float* at = nullptr)
    {
        int best = -1; float bt = 1e30f;
        for (size_t i = 0; i < m_parts.size(); ++i)
        {
            float t;
            if (m_parts[i].used && hitPart(m_parts[i], o, d, t) && t < bt) { bt = t; best = static_cast<int>(i); }
        }
        if (at && best >= 0) *at = bt;
        return best;
    }

    // ── the gesture ──────────────────────────────────────────────────────

    void pressLocked(float x, float y)
    {
        V3 o, d;
        const bool ray = rayLocked(x, y, o, d);
        if (m_button != PAINT_BUTTON_LEFT || !ray)
        {
            m_drag = (m_button == PAINT_BUTTON_MIDDLE) ? Drag_::Pan : Drag_::Orbit;
            return;
        }
        float t = 0.0f;
        const int hit = pickLocked(o, d, &t);
        switch (m_tool)
        {
            case Tool::Add:
                addAtLocked(o, d, hit, t);
                m_drag = Drag_::Orbit; m_dragged = true;   // the press is spent: no deselect on release
                return;
            case Tool::Paint:
                if (hit >= 0)
                {
                    const PaintColor c = colourLocked();
                    submitLocked("color " + std::to_string(hit) + " " + raws({ Fixed::From(c.r), Fixed::From(c.g), Fixed::From(c.b), Fixed::From(c.a) }), Why::Edit);
                    m_dragged = true;
                }
                m_drag = Drag_::Orbit;
                return;
            case Tool::Delete:
                if (hit >= 0) { submitLocked("del " + std::to_string(hit), Why::Edit); m_dragged = true; }
                m_drag = Drag_::Orbit;
                return;
            default: break;
        }
        if (hit < 0) { m_drag = Drag_::Orbit; return; }   // off every part: the eye's, and a click lets go
        selectLocked(hit);
        m_dragged = true;
        m_from = m_parts[hit];
        m_live = m_from;
        if (m_tool == Tool::Move)
        {
            m_drag = Drag_::Move;
            slideLocked(o, d, m_grab);          // where on the plane (or the axis) the press took hold
        }
        else if (m_tool == Tool::Turn) m_drag = Drag_::Turn;
        else if (m_tool == Tool::Size) m_drag = Drag_::Size;
        else m_drag = Drag_::Orbit;             // select: the press chose it, the drag is the eye's
    }

    void dragLocked(float x, float y)
    {
        switch (m_drag)
        {
            case Drag_::Orbit:
                m_yaw  += (x - m_last_x) * kTurn;
                m_pitch = clampf(m_pitch + (y - m_last_y) * kTurn, -kMaxPitch, kMaxPitch);
                placeCameraLocked();
                break;
            case Drag_::Pan:
            {
                float w, h; ViewFrustum v;
                if (!frameLocked(w, h, v)) break;
                const V3 f = sub(m_target, eyeLocked());
                V3 r = cross(V3{ 0.0f, 1.0f, 0.0f }, f);
                const float rl = std::sqrt(dot(r, r)), fl = std::sqrt(dot(f, f));
                if (rl <= 0.0f || fl <= 0.0f) break;
                r = mul(r, 1.0f / rl);
                const V3 u = mul(cross(f, r), 1.0f / fl);
                const float per_px = 2.0f * m_dist * std::tan(v.fov_y_radians * 0.5f) / h;
                m_target = add(m_target, add(mul(r, -(x - m_last_x) * per_px), mul(u, (y - m_last_y) * per_px)));
                placeCameraLocked();
                break;
            }
            case Drag_::Move:
            {
                V3 o, d, at;
                if (!rayLocked(x, y, o, d) || !slideLocked(o, d, at)) break;
                V3 p = add(posOf(m_from), sub(at, m_grab));
                if (m_snap > 0.0f)
                {
                    const V3 from = posOf(m_from);
                    if (p.x != from.x) p.x = std::round(p.x / m_snap) * m_snap;
                    if (p.y != from.y) p.y = std::round(p.y / m_snap) * m_snap;
                    if (p.z != from.z) p.z = std::round(p.z / m_snap) * m_snap;
                }
                m_live.x = Fixed::From(p.x); m_live.y = Fixed::From(p.y); m_live.z = Fixed::From(p.z);
                if (m_axis == 0)      { m_live.y = m_from.y; m_live.z = m_from.z; }
                else if (m_axis == 1) { m_live.x = m_from.x; m_live.z = m_from.z; }
                else if (m_axis == 2) { m_live.x = m_from.x; m_live.y = m_from.y; }
                drawLocked(m_sel, m_live);
                break;
            }
            case Drag_::Turn:
            {
                float deg = (x - m_press_x) * kTurn * 180.0f / kPi;
                if (m_turn_snap > 0.0f) deg = std::round(deg / m_turn_snap) * m_turn_snap;
                const V3 a = axisOf(m_axis < 0 ? 1 : m_axis);
                OrderVector q;
                q.qw = m_from.qw; q.qx = m_from.qx; q.qy = m_from.qy; q.qz = m_from.qz;
                q.RotateBy(Fixed::From(a.x), Fixed::From(a.y), Fixed::From(a.z), Fixed::From(static_cast<double>(deg) * 3.14159265358979 / 180.0));
                m_live.qw = q.qw; m_live.qx = q.qx; m_live.qy = q.qy; m_live.qz = q.qz;
                drawLocked(m_sel, m_live);
                break;
            }
            case Drag_::Size:
            {
                const double k = std::exp(static_cast<double>(m_press_y - y) * kGrow);
                auto grown = [&](Fixed h) {
                    double v = h.ToDouble() * k;
                    if (m_snap > 0.0f) { const double s = m_snap * 0.5; v = std::round(v / s) * s; if (v < s) v = s; }
                    return Fixed::From(v < kMinHalf ? kMinHalf : (v > kMaxHalf ? kMaxHalf : v));
                };
                m_live.hx = (m_axis < 0 || m_axis == 0) ? grown(m_from.hx) : m_from.hx;
                m_live.hy = (m_axis < 0 || m_axis == 1) ? grown(m_from.hy) : m_from.hy;
                m_live.hz = (m_axis < 0 || m_axis == 2) ? grown(m_from.hz) : m_from.hz;
                drawLocked(m_sel, m_live);
                break;
            }
            default: break;
        }
    }

    void releaseLocked()
    {
        const Drag_ was = m_drag;
        m_drag = Drag_::Idle;
        if (was == Drag_::Orbit && !m_dragged && m_button == PAINT_BUTTON_LEFT) { selectLocked(-1); return; }
        if (m_sel < 0 || (was != Drag_::Move && was != Drag_::Turn && was != Drag_::Size)) return;
        const std::string id = std::to_string(m_sel);
        std::string line;
        if (was == Drag_::Move && (m_live.x != m_from.x || m_live.y != m_from.y || m_live.z != m_from.z))
            line = "move " + id + " " + raws({ m_live.x, m_live.y, m_live.z });
        else if (was == Drag_::Turn && (m_live.qw != m_from.qw || m_live.qx != m_from.qx || m_live.qy != m_from.qy || m_live.qz != m_from.qz))
            line = "turn " + id + " " + raws({ m_live.qw, m_live.qx, m_live.qy, m_live.qz });
        else if (was == Drag_::Size && (m_live.hx != m_from.hx || m_live.hy != m_from.hy || m_live.hz != m_from.hz))
            line = "size " + id + " " + raws({ m_live.hx, m_live.hy, m_live.hz });
        // The picture goes back to the table either way: a judge's line draws
        // it again at once, a guest's when the record brings it round.
        drawLocked(m_sel, m_parts[m_sel]);
        if (!line.empty()) submitLocked(line, Why::Edit);
        axesLocked();
    }

    void cancelDragLocked()
    {
        const Drag_ was = m_drag;
        m_drag = Drag_::Idle;
        if (m_sel >= 0 && (was == Drag_::Move || was == Drag_::Turn || was == Drag_::Size))
        {
            drawLocked(m_sel, m_parts[m_sel]);
            axesLocked();
        }
    }

    // Where the pointer's line meets what a move slides on: the locked axis
    // through the part (the point of it nearest the line), or the plane
    // through the part that faces the eye.
    bool slideLocked(V3 o, V3 d, V3& at)
    {
        const V3 c = posOf(m_from);
        if (m_axis >= 0)
        {
            const V3 a = axisOf(m_axis), w = sub(c, o);
            const float b = dot(a, d), den = 1.0f - b * b;
            if (den < 1e-5f) return false;            // looking straight down the axis
            const float s = (b * dot(d, w) - dot(a, w)) / den;
            at = add(c, mul(a, s));
            return true;
        }
        V3 n = sub(m_target, eyeLocked());
        const float nl = std::sqrt(dot(n, n));
        if (nl <= 0.0f) return false;
        n = mul(n, 1.0f / nl);
        const float den = dot(n, d);
        if (std::fabs(den) < 1e-5f) return false;
        const float t = dot(n, sub(c, o)) / den;
        if (t <= 0.0f) return false;
        at = add(o, mul(d, t));
        return true;
    }

    // A new part where the pointer's line lands: on the part it hits, else on
    // the ground (y = 0), else at what the eye is looking at -- standing on it.
    void addAtLocked(V3 o, V3 d, int hit, float t)
    {
        int id = -1;
        for (size_t i = 0; i < m_parts.size(); ++i) if (!m_parts[i].used) { id = static_cast<int>(i); break; }
        if (id < 0) { ETCS_LOG("PaintModeler", "add: every part of the pool (" << m_parts.size() << ") is in use."); return; }
        V3 at = m_target;
        if (hit >= 0) at = add(o, mul(d, t));
        else if (d.y < -1e-5f) at = add(o, mul(d, -o.y / d.y));
        if (m_snap > 0.0f) { at.x = std::round(at.x / m_snap) * m_snap; at.z = std::round(at.z / m_snap) * m_snap; }
        const Fixed half = Fixed::From(0.5);
        const PaintColor c = colourLocked();
        const std::string line = "add " + std::to_string(id) + " " + m_shape + " "
            + raws({ Fixed::From(at.x), Fixed::From(at.y) + half, Fixed::From(at.z), half, half, half,
                     Fixed::From(c.r), Fixed::From(c.g), Fixed::From(c.b), Fixed::From(c.a),
                     Fixed::One(), Fixed::Zero(), Fixed::Zero(), Fixed::Zero() })
            + (m_material == "-" ? std::string() : " " + m_material);
        if (submitLocked(line, Why::Edit)) selectLocked(id);
    }

    PaintColor colourLocked()
    {
        if (ETCS::Entity* raw = paint_resolve_tag("PaintTool", m_tool_src))
            return static_cast<PaintTool*>(raw->getTrueType())->brush().color;
        return PaintColor{ 0.8f, 0.8f, 0.85f, 1.0f };
    }

    // ── the selection, and the axes drawn on it ──────────────────────────

    void selectLocked(int id)
    {
        if (id >= 0 && (static_cast<size_t>(id) >= m_parts.size() || !m_parts[id].used)) id = -1;
        if (id == m_sel) return;
        m_sel = id;
        if (id >= 0) m_shape = m_parts[id].shape;
        axesLocked();
        readoutLocked();
    }
    void axesLocked()
    {
        for (int a = 0; a < 3; ++a)
        {
            const ETCS::RID n = m_axis_node[a];
            if (!n) continue;
            if (m_sel < 0) { verb(n, "SetVisible", "0"); continue; }
            const Part& p = (m_drag == Drag_::Idle || m_drag == Drag_::Orbit || m_drag == Drag_::Pan) ? m_parts[m_sel] : m_live;
            const V3 c = posOf(p), h = halfOf(p), d = axisOf(a);
            const float reach = std::sqrt(dot(h, h)), len = reach + 0.6f + reach * 0.25f;
            const float thick = (m_axis == a ? 0.14f : 0.07f) * (0.5f + reach * 0.5f);
            verb(n, "Create", f3(thick, len, thick));
            verb(n, "SetPosition", f3(c.x + d.x * len * 0.5f, c.y + d.y * len * 0.5f, c.z + d.z * len * 0.5f));
            verb(n, "Aim", f3(d.x, d.y, d.z));
            verb(n, "SetVisible", "1");
        }
    }
    void readoutLocked()
    {
        cellsLocked();
        if (!m_readout) return;
        std::string t = std::string(toolName(m_tool)) + (m_axis < 0 ? "" : (m_axis == 0 ? "  x" : (m_axis == 1 ? "  y" : "  z")));
        if (m_sel >= 0) t += "  #" + std::to_string(m_sel) + " " + m_parts[m_sel].shape;
        else t += "  " + m_shape;
        t += "  " + std::to_string(countLocked()) + "/" + std::to_string(m_parts.size());
        if (t == m_readout_text) return;
        m_readout_text = t;
        paint_node_text(m_readout, t);
    }

    // Each cell lit or not, each caption its value -- written only when it
    // changed, since this runs on every change of what is in hand.
    void cellsLocked()
    {
        static constexpr float kIdle[3] = { 0.16f, 0.16f, 0.20f };
        static constexpr float kLift = 0.30f;   // the 2D bar's tool in hand (PaintPalette, SELECTED_LIFT)
        for (size_t i = 0; i < m_cells.size(); ++i)
        {
            const std::string& role = m_cells[i].first;
            bool lit;
            if (role == "axis")      lit = m_axis >= 0;
            else if (role == "snap") lit = m_snap > 0.0f || m_turn_snap > 0.0f;
            else                     lit = toolFrom(role) != Tool::None && toolFrom(role) == m_tool;
            if (m_cell_lit[i] == static_cast<int>(lit)) continue;
            m_cell_lit[i] = static_cast<int>(lit);
            const float t = lit ? kLift : 0.0f;
            paint_node_fill(m_cells[i].second, kIdle[0] + (1.0f - kIdle[0]) * t, kIdle[1] + (1.0f - kIdle[1]) * t,
                            kIdle[2] + (1.0f - kIdle[2]) * t, 1.0f);
        }
        for (size_t i = 0; i < m_captions.size(); ++i)
        {
            const std::string& role = m_captions[i].first;
            std::string t;
            if (role == "shape")       t = (m_tool != Tool::Add && m_sel >= 0) ? m_parts[m_sel].shape : m_shape;
            else if (role == "material") t = (m_tool != Tool::Add && m_sel >= 0) ? m_parts[m_sel].material : m_material;
            else if (role == "axis")   t = m_axis < 0 ? "free" : (m_axis == 0 ? "x" : (m_axis == 1 ? "y" : "z"));
            else if (role == "snap")   t = (m_snap > 0.0f || m_turn_snap > 0.0f) ? "on" : "off";
            else if (role == "export") t = m_format;
            else if (role == "mode")   t = m_open.load() ? "2d" : "3d";
            else continue;
            if (t == m_caption_text[i]) continue;
            m_caption_text[i] = t;
            paint_node_text(m_captions[i].second, t);
        }
    }

    // ── the table, and its picture ───────────────────────────────────────

    uint32_t countLocked() const
    {
        uint32_t n = 0;
        for (const Part& p : m_parts) if (p.used) ++n;
        return n;
    }
    bool materialKnownLocked(const std::string& name) const
    {
        if (name == "-") return true;
        for (const std::string& m : m_materials) if (m == name) return true;
        return false;
    }
    bool shapeKnownLocked(const std::string& name) const
    {
        if (name == "box") return true;
        for (auto& s : m_shapes) if (s.first == name) return true;
        return false;
    }
    ETCS::RID meshLocked(const std::string& name) const
    {
        for (auto& s : m_shapes) if (s.first == name) return s.second;
        return 0;
    }
    // A part's node, made to look like `p`.
    void drawLocked(int id, const Part& p)
    {
        if (id < 0 || static_cast<size_t>(id) >= m_nodes.size()) return;
        const ETCS::RID n = m_nodes[id];
        if (!p.used) { verb(n, "SetVisible", "0"); return; }
        verb(n, "Create", f3(p.hx.ToFloat() * 2.0f, p.hy.ToFloat() * 2.0f, p.hz.ToFloat() * 2.0f));
        verb(n, "SetPosition", f3(p.x.ToFloat(), p.y.ToFloat(), p.z.ToFloat()));
        // The spinor as an axis and an angle, which is what Orient takes.
        const double w = p.qw.ToDouble(), s = std::sqrt(std::max(0.0, 1.0 - w * w));
        if (s < 1e-9) verb(n, "Orient", "0, 1, 0, 0");
        else verb(n, "Orient", f3(static_cast<float>(p.qx.ToDouble() / s), static_cast<float>(p.qy.ToDouble() / s), static_cast<float>(p.qz.ToDouble() / s))
                               + ", " + f1(static_cast<float>(2.0 * std::atan2(s, w) * 180.0 / 3.14159265358979)));
        verb(n, "SetColor", f3(p.col[0].ToFloat(), p.col[1].ToFloat(), p.col[2].ToFloat()) + ", " + f1(p.col[3].ToFloat()));
        verb(n, "SetMesh", std::to_string(meshLocked(p.shape)));
        verb(n, "SetVisible", "1");
    }
    void restartLocked()
    {
        m_drag = Drag_::Idle;
        m_sel = -1;
        for (size_t i = 0; i < m_parts.size(); ++i)
        {
            if (m_parts[i].used) verb(m_nodes[i], "SetVisible", "0");
            m_parts[i] = Part{};
        }
        m_undo.clear(); m_redo.clear(); m_pending.clear(); m_outbox.clear();
        axesLocked();
        readoutLocked();
    }
    // An add, and its material after the numbers when it has one.
    static std::string addLine(size_t id, const Part& p)
    {
        return "add " + std::to_string(id) + " " + p.shape + " "
             + raws({ p.x, p.y, p.z, p.hx, p.hy, p.hz, p.col[0], p.col[1], p.col[2], p.col[3], p.qw, p.qx, p.qy, p.qz })
             + (p.material == "-" ? std::string() : " " + p.material);
    }
    std::string linesLocked() const
    {
        std::string out;
        for (size_t i = 0; i < m_parts.size(); ++i)
            if (m_parts[i].used) { out += addLine(i, m_parts[i]); out += '\n'; }
        return out;
    }

    // ── a line, applied ──────────────────────────────────────────────────

    // Takes `line` if it can be true of the table as it stands, and says what
    // line puts back what it replaced. Refuses anything else, whole.
    bool applyLocked(const std::string& line, std::string& inverse)
    {
        std::istringstream in(line);
        std::string op; uint64_t id = 0;
        if (!(in >> op >> id) || id >= m_parts.size()) return false;
        Part& p = m_parts[id];
        const std::string sid = std::to_string(id);
        auto fixed = [&](Fixed* out, int n) {
            for (int i = 0; i < n; ++i) { int64_t r; if (!(in >> r)) return false; out[i] = Fixed::FromRaw(r); }
            return true;
        };
        auto halves = [](const Fixed* h) {
            for (int i = 0; i < 3; ++i) if (h[i] < Fixed::From(kMinHalf) || h[i] > Fixed::From(kMaxHalf)) return false;
            return true;
        };
        auto unit = [](Fixed* c) { for (int i = 0; i < 4; ++i) c[i] = Fixed::Max(Fixed::Zero(), Fixed::Min(Fixed::One(), c[i])); };

        if (op == "add")
        {
            std::string shape; Fixed v[14];
            if (p.used || !(in >> shape) || !shapeKnownLocked(shape) || !fixed(v, 14) || !halves(v + 3)) return false;
            if (v[10].IsZero() && v[11].IsZero() && v[12].IsZero() && v[13].IsZero()) return false;
            std::string made;
            if (!(in >> made)) made = "-";
            if (!materialKnownLocked(made)) return false;
            unit(v + 6);
            p.used = true; p.shape = shape; p.material = made;
            p.x = v[0]; p.y = v[1]; p.z = v[2]; p.hx = v[3]; p.hy = v[4]; p.hz = v[5];
            for (int i = 0; i < 4; ++i) p.col[i] = v[6 + i];
            p.qw = v[10]; p.qx = v[11]; p.qy = v[12]; p.qz = v[13];
            inverse = "del " + sid;
        }
        else if (!p.used) return false;
        else if (op == "del")
        {
            inverse = addLine(id, p);
            p = Part{};
            if (m_sel == static_cast<int>(id)) { m_drag = Drag_::Idle; m_sel = -1; }
        }
        else if (op == "move")
        {
            Fixed v[3];
            if (!fixed(v, 3)) return false;
            inverse = "move " + sid + " " + raws({ p.x, p.y, p.z });
            p.x = v[0]; p.y = v[1]; p.z = v[2];
        }
        else if (op == "turn")
        {
            Fixed v[4];
            if (!fixed(v, 4) || (v[0].IsZero() && v[1].IsZero() && v[2].IsZero() && v[3].IsZero())) return false;
            inverse = "turn " + sid + " " + raws({ p.qw, p.qx, p.qy, p.qz });
            p.qw = v[0]; p.qx = v[1]; p.qy = v[2]; p.qz = v[3];
        }
        else if (op == "size")
        {
            Fixed v[3];
            if (!fixed(v, 3) || !halves(v)) return false;
            inverse = "size " + sid + " " + raws({ p.hx, p.hy, p.hz });
            p.hx = v[0]; p.hy = v[1]; p.hz = v[2];
        }
        else if (op == "color")
        {
            Fixed v[4];
            if (!fixed(v, 4)) return false;
            unit(v);
            inverse = "color " + sid + " " + raws({ p.col[0], p.col[1], p.col[2], p.col[3] });
            for (int i = 0; i < 4; ++i) p.col[i] = v[i];
        }
        else if (op == "shape")
        {
            std::string shape;
            if (!(in >> shape) || !shapeKnownLocked(shape)) return false;
            inverse = "shape " + sid + " " + p.shape;
            p.shape = shape;
        }
        else if (op == "material")
        {
            std::string made;
            if (!(in >> made) || !materialKnownLocked(made)) return false;
            inverse = "material " + sid + " " + p.material;
            p.material = made;
        }
        else return false;

        // A drag in hand on the part a line just changed is let go of: the
        // line is what is so.
        if (m_sel == static_cast<int>(id) && (m_drag == Drag_::Move || m_drag == Drag_::Turn || m_drag == Drag_::Size)) m_drag = Drag_::Idle;
        drawLocked(static_cast<int>(id), p);
        if (m_sel == static_cast<int>(id) || m_sel < 0) axesLocked();
        readoutLocked();
        return true;
    }

    // This runtime's own line: taken here if this model judges, proposed if
    // it follows. `why` is where its inverse goes once it has landed.
    bool submitLocked(const std::string& line, Why why)
    {
        if (!m_judge)
        {
            m_outbox.push_back(line);
            m_pending.emplace_back(line, why);
            return true;
        }
        std::string inverse;
        if (!applyLocked(line, inverse)) return false;
        recordLocked(m_me, line);
        landedLocked(why, inverse);
        return true;
    }
    void landedLocked(Why why, const std::string& inverse)
    {
        if (why == Why::Undo) m_redo.push_back(inverse);
        else
        {
            m_undo.push_back(inverse);
            if (why == Why::Edit) m_redo.clear();
        }
        if (m_undo.size() > kHistory) m_undo.pop_front();
        if (m_redo.size() > kHistory) m_redo.pop_front();
    }
    // A guest's own line came round on the record. The proposals ahead of it
    // that never did were refused by the judge, and are forgotten with it.
    void landedMineLocked(const std::string& line, bool took, const std::string& inverse)
    {
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (m_pending[i].first != line) continue;
            const Why why = m_pending[i].second;
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            if (took) landedLocked(why, inverse);
            return;
        }
    }
    void recordLocked(const std::string& author, const std::string& line)
    {
        if (!m_session) return;
        Record_* rec = recordOf(m_record);
        if (!rec) return;
        const uint64_t seq = rec->Append(author, line);
        if (seq != UINT64_MAX) m_seq = seq + 1;
    }

    // ── helpers ──────────────────────────────────────────────────────────

    static std::string raws(std::initializer_list<Fixed> vs)
    {
        std::string out;
        for (Fixed v : vs) { if (!out.empty()) out += ' '; out += std::to_string(v.raw); }
        return out;
    }
    static std::string f1(float a) { char b[32]; std::snprintf(b, sizeof b, "%.6f", static_cast<double>(a)); return b; }
    static std::string f3(float a, float b, float c) { return f1(a) + ", " + f1(b) + ", " + f1(c); }
    static bool parseLine(const std::string& msg, uint64_t& seq, std::string& author, std::string& line)
    {
        const size_t a = msg.find(' ');
        const size_t b = a == std::string::npos ? a : msg.find(' ', a + 1);
        if (b == std::string::npos) return false;
        seq = std::strtoull(msg.c_str(), nullptr, 10);
        author = msg.substr(a + 1, b - a - 1);
        line = msg.substr(b + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return !line.empty();
    }
    static Record_* recordOf(ETCS::RID rid)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        return e ? static_cast<Record_*>(e->getInterfacePointer(ETCS::Buffer("Record"))) : nullptr;
    }
    // A verb on a scene node, by its own tag: what this module knows of the
    // module that made it is the verb's name.
    static std::string ask(ETCS::RID node, const char* name, const std::string& args)
    {
        if (!node) return std::string();
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), node);
        if (!e) return std::string();
        const std::string tag = e->getSourceTag().toString();
        ETCS::Buffer action((tag + "." + name).c_str());
        ETCS::Buffer payload(args.c_str());
        try { e->call(action, payload); } catch (...) { return std::string(); }
        return payload.toString();
    }
    static bool verb(ETCS::RID node, const char* name, const std::string& args)
    {
        if (!node) return false;
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), node);
        if (!e) return false;
        const std::string tag = e->getSourceTag().toString();
        ETCS::Buffer action((tag + "." + name).c_str());
        ETCS::Buffer payload(args.c_str());
        try { e->call(action, payload); } catch (...) { return false; }
        return true;
    }

    std::mutex m_mu;
    ETCS::RID  m_world = 0, m_camera = 0, m_tool_src = 0, m_readout = 0;
    ETCS::RID  m_axis_node[3] = { 0, 0, 0 };
    std::vector<ETCS::RID> m_panes2, m_panes3;
    ETCS::RID m_layout = 0, m_share = 0;
    std::vector<std::pair<std::string, ETCS::RID>> m_cells, m_captions;
    std::vector<int>         m_cell_lit;
    std::vector<std::string> m_caption_text;
    std::string m_format = "glb";
    PaintSurface* m_surface = nullptr;
    std::string m_readout_text;
    std::atomic<bool> m_open{ false };   // read by the input edge with no lock

    // The pool and the table, slot for slot.
    std::vector<ETCS::RID> m_nodes;
    std::vector<Part>      m_parts;
    std::vector<std::pair<std::string, ETCS::RID>> m_shapes;

    // This runtime's own: the tool, the selection, the eye, a drag in hand.
    Tool        m_tool = Tool::Select;
    std::string m_shape = "box";
    std::vector<std::string> m_materials;   // AddMaterial: what a part may be made of
    std::string m_material = "-";            // what add makes a part of
    int         m_axis = -1, m_sel = -1;
    float       m_snap = 0.0f, m_turn_snap = 0.0f;
    float       m_yaw = 0.6f, m_pitch = 0.5f, m_dist = 9.0f;
    V3          m_target;
    Drag_       m_drag = Drag_::Idle;
    uint16_t    m_button = 0;
    bool        m_dragged = false;
    float       m_press_x = 0.0f, m_press_y = 0.0f, m_last_x = 0.0f, m_last_y = 0.0f;
    Part        m_from, m_live;      // the dragged part when the drag began, and as drawn now
    V3          m_grab;

    // The lines that put back what this runtime's own lines replaced.
    std::deque<std::string> m_undo, m_redo;

    // The session.
    std::string m_me;
    bool        m_judge = true, m_session = false;
    ETCS::RID   m_record = 0, m_proposals = 0;
    uint64_t    m_seq = 0, m_gen = 0;
    std::deque<std::string>                 m_outbox;
    std::deque<std::pair<std::string, Why>> m_pending;
};

#endif // PAINTPROVIDER_PAINTMODELER_H__
