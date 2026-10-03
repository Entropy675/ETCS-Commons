#ifndef PAINTPROVIDER_PAINTPAGES_H__
#define PAINTPROVIDER_PAINTPAGES_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintSurface.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintPages: the pages of this session, kept in a database ───────────────
 *
 * A HISTORY OF PAGES, AND ONE OF THEM IS THE PRESENT. The document is the
 * thing being painted on; this is the set of things it has been, each one a
 * row per layer in a database another module owns. "Current" is the slot the
 * present was last loaded from or saved to -- the "last changed" slot -- and
 * it is the one Save writes into. Nothing is current until something has been
 * saved or loaded, which is what m_current == 0 means.
 *
 * THROUGH THE FAMILY, BY RID. The database is reached as a Database_ through
 * the ontology (ontology/Database.h), never through DatabaseProvider's header:
 * this module says what it needs of a database -- a statement with bound
 * values, stepped a row at a time -- and any leaf claiming the family answers.
 * That is also why locality is nobody's business here: the boot script names a
 * file under /persist in the browser and a relative path on a desktop, and this
 * type does not know which.
 *
 * PAM IN THE BLOB, because it is already the pixels (the note above
 * PaintImage): a layer row is the same bytes ExportLayer writes to a file,
 * read back by the same parser, so nothing here has a format of its own and a
 * row pulled out of the database with any sqlite tool is a picture GIMP opens.
 *
 * QUICK SWITCH IS SAVE-THEN-LOAD, and the save is conditional on the document
 * having changed since it was loaded (PaintDocument::revision): switching away
 * from a page you only looked at must not touch its row, and switching away
 * from one you painted on must not lose the paint. The neighbour is the next
 * or previous id, wrapping -- by id and not by recency, because a page that
 * moved to the front every time it was saved would reorder the ring under the
 * keys that walk it.
 *
 * WHAT A PAGE DOES NOT CARRY, and says so when it matters: the text boxes.
 * They are strings, not pixels, and the export has the same gap for the same
 * reason (ExportImage); Save counts them so a page that lost its captions says
 * so in the log rather than in the next session.
 */
class PaintPages : public DeletableBase<PaintPages>
{
public:
    WIRE_TYPE_IDENTITY(PaintPages);

    PaintPages() = default;
    bool DeleteConcrete() override { return true; }

    /*
 * TWO DELETES, AND THE ONE WITH AN ARGUMENT IS THIS TYPE'S OWN: Delete(id)
 * removes a PAGE from the store, while the family's Delete() removes this
 * entity (DeletableBase). Declaring the first hides the second, which the
 * compiler reports on every build as an overloaded virtual going quiet -- so
 * the base's name is pulled back in and the two live as an overload set.
 * Nothing resolves differently: the page verb passes an id, the entity verb
 * passes nothing, and the tag block names them Delete and Destroy so a script
 * never has to know there were ever two.
 */
    using DeletableBase<PaintPages>::Delete;

    /*
     * Bind the document and the database, and make sure the tables exist.
     * The schema is created here rather than by the script because the
     * script cannot know the columns this type reads -- a script that spells
     * them differently would be a page store that saves and never loads.
     */
    bool Create(ETCS::RID document, ETCS::RID database)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        m_document = raw ? static_cast<PaintDocument*>(raw->getTrueType()) : nullptr;
        m_db = database;
        m_current = 0;
        m_loaded_rev = m_document ? m_document->revision() : 0;
        if (!m_document) { ETCS_LOG("PaintPages", "Create: no PaintDocument at RID:" << document); return false; }

        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Create: no Database at RID:" << database << " -- Connect it first."); return false; }

        static const char* const schema[] = {
            "CREATE TABLE IF NOT EXISTS pages("
            " id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
            " width INTEGER NOT NULL, height INTEGER NOT NULL, updated_at INTEGER NOT NULL)",
            // ord is the layer's POSITION in the stack, bottom first, not its
            // order key: two layers may share a key (PaintLayer says so) and a
            // stored page wants a dense, unique stacking back.
            "CREATE TABLE IF NOT EXISTS page_layers("
            " page_id INTEGER NOT NULL, ord INTEGER NOT NULL, name TEXT NOT NULL,"
            " visible INTEGER NOT NULL, opacity REAL NOT NULL, active INTEGER NOT NULL DEFAULT 0,"
            " pam BLOB NOT NULL)",
            "CREATE INDEX IF NOT EXISTS page_layers_by_page ON page_layers(page_id, ord)",
            // THE THUMBNAIL, made at save time and kept beside the page rather
            // than derived from it: a list of pages wants a picture per row,
            // and decoding a page's layers to draw one is 6 MB of PAM per row
            // per refresh. Its own table so an older store gains it on the
            // next save without an ALTER.
            "CREATE TABLE IF NOT EXISTS page_thumbs("
            " page_id INTEGER PRIMARY KEY, width INTEGER NOT NULL, height INTEGER NOT NULL,"
            " rgba BLOB NOT NULL)",
        };
        for (const char* sql : schema)
        {
            Stmt st(*db, sql);
            if (!st || st.step() < 0) { ETCS_LOG("PaintPages", "Create: schema failed -- see the database's log."); return false; }
        }
        m_loaded_rev = m_document->revision();   // what is on screen at boot is not yet a change
        ETCS_LOG("PaintPages", "bound to '" << m_document->name() << "', " << count_pages(*db) << " page(s) stored.");
        return true;
    }

    int64_t current() const { return m_current; }
    bool dirty() const { return m_document && m_document->revision() != m_loaded_rev; }

    /*
     * The present into its slot -- a new one if it has none. One transaction:
     * a page with half its layers is not a page, and the database's own
     * guard rolls back on any exit that is not the commit.
     */
    bool Save()
    {
        if (!m_document) return false;
        Waiting wait(*this);
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Save: the database is gone."); return false; }

        std::vector<PaintLayer*> stack = m_document->layers();
        auto guard = db->Transaction();

        if (m_current == 0)
        {
            Stmt ins(*db, "INSERT INTO pages(name, width, height, updated_at) VALUES(?, ?, ?, ?)");
            if (!ins || !ins.bind(1, text(m_document->name())) || !ins.bind(2, DatabaseValue::integer(m_document->width()))
                || !ins.bind(3, DatabaseValue::integer(m_document->height())) || !ins.bind(4, DatabaseValue::integer(now()))
                || ins.step() < 0)
                return false;
            m_current = last_insert_id(*db);
            if (m_current == 0) return false;
            // A page made with no name is named after its row, the one thing
            // that is certainly unique and reads back as itself in List.
            if (m_document->name().empty()) m_document->SetName("Page " + std::to_string(m_current));
        }
        {
            Stmt up(*db, "UPDATE pages SET name = ?, width = ?, height = ?, updated_at = ? WHERE id = ?");
            if (!up || !up.bind(1, text(m_document->name())) || !up.bind(2, DatabaseValue::integer(m_document->width()))
                || !up.bind(3, DatabaseValue::integer(m_document->height())) || !up.bind(4, DatabaseValue::integer(now()))
                || !up.bind(5, DatabaseValue::integer(m_current)) || up.step() < 0)
                return false;
        }
        {
            Stmt del(*db, "DELETE FROM page_layers WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(m_current)) || del.step() < 0) return false;
        }
        size_t bytes = 0;
        for (size_t i = 0; i < stack.size(); ++i)
        {
            PaintLayer* l = stack[i];
            std::vector<uint8_t> pam;
            std::string why;
            if (!paint_pam_encode(l->PixelData(), l->width(), l->height(), pam, why))
            {
                ETCS_LOG("PaintPages", "Save: layer '" << l->name() << "': " << why << " -- page not saved.");
                return false;
            }
            Stmt ins(*db, "INSERT INTO page_layers(page_id, ord, name, visible, opacity, active, pam)"
                          " VALUES(?, ?, ?, ?, ?, ?, ?)");
            if (!ins || !ins.bind(1, DatabaseValue::integer(m_current)) || !ins.bind(2, DatabaseValue::integer((int64_t)i))
                || !ins.bind(3, text(l->name())) || !ins.bind(4, DatabaseValue::integer(l->visible() ? 1 : 0))
                || !ins.bind(5, DatabaseValue::real(l->opacity()))
                || !ins.bind(6, DatabaseValue::integer(l == m_document->activeLayer() ? 1 : 0))
                || !ins.bind(7, DatabaseValue::blob(pam.data(), pam.size())) || ins.step() < 0)
                return false;
            bytes += pam.size();
        }
        {
            std::vector<uint8_t> thumb;
            page_thumb(stack, THUMB_W, THUMB_H, thumb);
            Stmt th(*db, "INSERT OR REPLACE INTO page_thumbs(page_id, width, height, rgba) VALUES(?, ?, ?, ?)");
            if (!th || !th.bind(1, DatabaseValue::integer(m_current)) || !th.bind(2, DatabaseValue::integer(THUMB_W))
                || !th.bind(3, DatabaseValue::integer(THUMB_H))
                || !th.bind(4, DatabaseValue::blob(thumb.data(), thumb.size())) || th.step() < 0)
                return false;
        }
        if (!guard.commit()) { ETCS_LOG("PaintPages", "Save: commit failed -- page not saved."); return false; }

        m_loaded_rev = m_document->revision();
        ETCS_LOG("PaintPages", "saved page " << m_current << " '" << m_document->name() << "' "
                 << m_document->width() << "x" << m_document->height() << ", " << stack.size()
                 << " layer(s), " << bytes << " bytes of PAM"
                 << (m_document->textBoxCount() ? "; " + std::to_string(m_document->textBoxCount())
                                                  + " text box(es) are not in it" : std::string()));
        persist();
        return true;
    }

    /*
 * THE FILE IS WRITTEN; NOW THE BROWSER HAS TO KEEP IT. sqlite has put the bytes
 * on the filesystem, and on the desktop that is the end of it. In the browser
 * the filesystem under /persist is a MEMFS view of IndexedDB that only reaches
 * the store when the page calls syncfs -- so the page is told, once per save,
 * through the same event bridge the menu uses. Nothing here knows what the page
 * does with it, and on the desktop this is a no-op by construction.
 */
    void persist()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-persist', { detail: 'save' }));
        });
#endif
    }

    /*
     * A stored page onto the document, replacing what is there. Read first,
     * then destroy, then spawn: the read holds the database, and a
     * DestroyEvent must not be fired from inside a hold (PaintDocument::
     * DestroyLayers says why), so the rows are copied out and the hold
     * dropped before a single layer goes.
     */
    bool Load(int64_t id)
    {
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
        struct Row { int64_t ord; std::string name; bool visible; float opacity; bool active; std::vector<uint8_t> pam; };
        std::vector<Row> rows;
        std::string name;
        int64_t w = 0, h = 0;
        {
            ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
            if (!db) { ETCS_LOG("PaintPages", "Load: the database is gone."); return false; }
            Stmt page(*db, "SELECT name, width, height FROM pages WHERE id = ?");
            if (!page || !page.bind(1, DatabaseValue::integer(id)) || page.step() != 1)
            {
                ETCS_LOG("PaintPages", "Load: no page " << id << " -- List says which exist.");
                return false;
            }
            name = str(page.col(0));
            w = page.col(1).i;
            h = page.col(2).i;
            Stmt lay(*db, "SELECT ord, name, visible, opacity, active, pam FROM page_layers WHERE page_id = ? ORDER BY ord");
            if (!lay || !lay.bind(1, DatabaseValue::integer(id))) return false;
            for (int rc = lay.step(); rc == 1; rc = lay.step())
            {
                Row r;
                r.ord     = lay.col(0).i;
                r.name    = str(lay.col(1));
                r.visible = lay.col(2).i != 0;
                r.opacity = static_cast<float>(lay.col(3).d);
                r.active  = lay.col(4).i != 0;
                const DatabaseValue blob = lay.col(5);
                if (blob.p && blob.n)
                    r.pam.assign(static_cast<const uint8_t*>(blob.p), static_cast<const uint8_t*>(blob.p) + blob.n);
                rows.push_back(std::move(r));
            }
        }

        m_document->DestroyLayers();
        m_document->Create(static_cast<uint32_t>(w), static_cast<uint32_t>(h), name);
        size_t spawned = 0;
        bool any_active = false;
        for (const Row& r : rows)
        {
            PaintImage img;
            std::string why;
            if (!paint_pam_parse(r.pam.data(), r.pam.size(), img, why))
            {
                ETCS_LOG("PaintPages", "Load: page " << id << " layer " << r.ord << " '" << r.name
                         << "': " << why << " -- skipped.");
                continue;
            }
            if (m_document->SpawnLayer(img, r.name, static_cast<int32_t>(r.ord), r.visible, r.opacity, r.active))
            {
                ++spawned;
                any_active = any_active || r.active;
            }
        }
        // The top is what is about to be worked on when the row did not say
        // -- the same choice an import makes.
        if (!any_active)
        {
            std::vector<PaintLayer*> stack = m_document->layers();
            if (!stack.empty()) m_document->SetActiveLayer(stack.back()->getRID());
        }
        m_current = id;
        m_loaded_rev = m_document->revision();
        ETCS_LOG("PaintPages", "loaded page " << id << " '" << name << "' " << w << "x" << h << ", "
                 << spawned << " of " << rows.size() << " layer(s).");
        repaint();
        return true;
    }

    /*
     * A fresh page becomes current: the present is kept if it has anything
     * unsaved, then the document is emptied and given the two layers the boot
     * script starts with -- paper under ink, ink active -- because a page
     * with nothing to paint on is not a page anyone can use. Saved at once,
     * so it has an id and Next/Prev can find it.
     */
    // What the store holds, for anything that wants to SHOW the pages rather
    // than step through them -- the gear menu's list (PaintCanvasMenu).
    struct PageInfo { int64_t id = 0; std::string name; int64_t w = 0, h = 0, layers = 0; };

    // The stored thumbnail's size. 4:3 like the default page, and the width a
    // list row can give a picture beside a name.
    static constexpr int32_t THUMB_W = 32;
    static constexpr int32_t THUMB_H = 24;

    // The thumbnail saved with a page, or false for a page saved before there
    // were any (it gains one on its next save).
    bool Thumb(int64_t id, int32_t& w, int32_t& h, std::vector<uint8_t>& rgba)
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt st(*db, "SELECT width, height, rgba FROM page_thumbs WHERE page_id = ?");
        if (!st || !st.bind(1, DatabaseValue::integer(id)) || st.step() != 1) return false;
        w = static_cast<int32_t>(st.col(0).i);
        h = static_cast<int32_t>(st.col(1).i);
        const DatabaseValue v = st.col(2);
        if (v.kind != DatabaseValue::Blob || !v.p || w <= 0 || h <= 0
            || v.n != static_cast<size_t>(w) * h * 4) return false;
        rgba.assign(static_cast<const uint8_t*>(v.p), static_cast<const uint8_t*>(v.p) + v.n);
        return true;
    }

    // A page's name in the store, and on the document too when it is the one
    // on screen -- otherwise the next save would write the old name back.
    bool Rename(int64_t id, const std::string& name)
    {
        if (name.empty()) return false;
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt up(*db, "UPDATE pages SET name = ? WHERE id = ?");
        if (!up || !up.bind(1, text(name)) || !up.bind(2, DatabaseValue::integer(id)) || up.step() < 0) return false;
        if (id == m_current && m_document) m_document->SetName(name);
        persist();
        ETCS_LOG("PaintPages", "page " << id << " renamed '" << name << "'");
        return true;
    }

    bool Pages(std::vector<PageInfo>& out)
    {
        out.clear();
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt st(*db, "SELECT p.id, p.name, p.width, p.height,"
                     " (SELECT COUNT(*) FROM page_layers l WHERE l.page_id = p.id)"
                     " FROM pages p ORDER BY p.id");
        if (!st) return false;
        for (int rc = st.step(); rc == 1; rc = st.step())
        {
            PageInfo info;
            info.id     = st.col(0).i;
            info.name   = str(st.col(1));
            info.w      = st.col(2).i;
            info.h      = st.col(3).i;
            info.layers = st.col(4).i;
            out.push_back(std::move(info));
        }
        return true;
    }

    bool New() { return NewAt(m_document ? m_document->width() : 0,
                              m_document ? m_document->height() : 0); }

    /*
 * A NEW PAGE AT A STATED SIZE. New() keeps whatever the present page is,
 * which is right for ctrl+PageDown off the end of the strip and wrong for the
 * gear menu, where the whole point of the two steppers is to say how big the
 * next canvas should be. Same page otherwise: the present is flushed to its
 * slot first, so "new" adds to the history rather than replacing what was
 * there -- which is what makes more than one page exist to switch between.
 */
    bool NewAt(uint32_t want_w, uint32_t want_h)
    {
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
        flush();
        const uint32_t w = want_w ? want_w : (m_document->width() ? m_document->width() : 1024);
        const uint32_t h = want_h ? want_h : (m_document->height() ? m_document->height() : 768);
        m_document->DestroyLayers();
        m_document->Create(w, h, "");           // named after its row by Save

        PaintImage sheet;
        sheet.w = w; sheet.h = h;
        sheet.rgba.assign(static_cast<size_t>(w) * h * 4, 255);         // white paper
        if (!m_document->SpawnLayer(sheet, "Paper", 0, true, 1.0f, false)) return false;
        std::fill(sheet.rgba.begin(), sheet.rgba.end(), 0);            // transparent ink
        if (!m_document->SpawnLayer(sheet, "Ink", 1, true, 1.0f, true)) return false;

        m_current = 0;
        const bool ok = Save();
        repaint();
        return ok;
    }

    bool Next() { return step(+1); }
    bool Prev() { return step(-1); }

    // The surface to repaint when the document under it is swapped. The
    // document does not know who shows it; a load that leaves the old picture
    // on screen until the next stroke is a load that looks like it failed.
    void BindSurface(ETCS::RID surface) { m_surface = surface; }

    /*
 * BUSY IS A FLAG THIS TYPE RAISES ON ITSELF for as long as a page operation
 * runs. A new page encodes and stores every layer of the one it leaves (6 MB
 * of PAM at 1024x768) and a load decodes as much back, all on the thread the
 * press arrived on -- a second or more in the browser during which the sheet
 * is still and the pointer is ignored, which is indistinguishable from a hang.
 *
 * A state tag rather than a verb at a bound node, because the store's job is
 * to say what state it is in, not to know what shows it. `busy` is a
 * lowercase entry in this entity's own tag store (Entity::addTag), ordered
 * within this module like any tag change; whatever wants to show the wait
 * observes it -- the session's throbber does (Throbber::Watch, bound in
 * boot_paint_panels.etcs), on the frame edge's own thread, and so would
 * anything else that watched. Nothing crosses a module boundary on the input
 * thread to make the indicator appear.
 *
 * Every operation that writes the store raises it, not only the page switch:
 * a Save is the same encode, and a Delete drops a page's megabytes of layer
 * rows -- a second of nothing after pressing an x is the same hang-shaped
 * silence.
 *
 * A depth rather than a bare raise because step() lands in Load() or NewAt(),
 * and the flag should come down when the outermost one does.
 */
    struct Waiting
    {
        PaintPages& p;
        explicit Waiting(PaintPages& pages) : p(pages) { if (p.m_wait_depth++ == 0) p.set_busy(true); }
        ~Waiting() { if (--p.m_wait_depth == 0) p.set_busy(false); }
    };
    void repaint()
    {
        if (m_surface == 0) return;
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", m_surface);
        if (raw) static_cast<PaintSurface*>(raw->getTrueType())->Render();
    }

    void List()
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "List: the database is gone."); return; }
        Stmt st(*db, "SELECT p.id, p.name, p.width, p.height, p.updated_at,"
                     " (SELECT COUNT(*) FROM page_layers l WHERE l.page_id = p.id)"
                     " FROM pages p ORDER BY p.id");
        if (!st) return;
        size_t n = 0;
        for (int rc = st.step(); rc == 1; rc = st.step(), ++n)
        {
            const int64_t id = st.col(0).i;
            ETCS_LOG("PaintPages", "  page " << id << " '" << str(st.col(1)) << "' "
                     << st.col(2).i << "x" << st.col(3).i << ", " << st.col(5).i
                     << " layer(s), updated " << st.col(4).i
                     << (id == m_current ? (dirty() ? "  <- current, changed" : "  <- current") : ""));
        }
        ETCS_LOG("PaintPages", n << " page(s)"
                 << (m_current == 0 ? "; the present is not in a slot yet" : ""));
    }

    /*
 * THE PRESENT PUT AWAY AND LET GO OF: saved to its slot if it changed, and
 * then in no slot at all, so whatever replaces it on screen is a new page
 * rather than an overwrite of this one. What joining a shared canvas does
 * first -- the joiner's page is kept, and the session's page arrives into a
 * document that no longer answers to that slot.
 */
    bool Stash()
    {
        if (!m_document) return false;
        if (dirty() && !Save()) return false;
        if (m_current != 0)
            ETCS_LOG("PaintPages", "page " << m_current << " put away; the present is in no slot now.");
        m_current = 0;
        return true;
    }

    // The slot goes; the picture on screen does not. Deleting the current page
    // leaves the present as an unsaved page, which the next Save gives a new
    // row -- the alternative, emptying the document, would make a wrong id
    // typed into Delete cost the work in front of you.
    bool Delete(int64_t id)
    {
        Waiting wait(*this);
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Delete: the database is gone."); return false; }
        auto guard = db->Transaction();
        {
            Stmt del(*db, "DELETE FROM page_layers WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        {
            Stmt del(*db, "DELETE FROM pages WHERE id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        {
            Stmt del(*db, "DELETE FROM page_thumbs WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        if (!guard.commit()) return false;
        if (id == m_current) m_current = 0;
        ETCS_LOG("PaintPages", "deleted page " << id
                 << (m_current == 0 ? "; the present is no longer in a slot" : ""));
        return true;
    }

private:
    void set_busy(bool on)
    {
        if (on) this->addTag("busy");
        else    this->removeTag(ETCS::Buffer("busy"));
    }

    PaintDocument* m_document = nullptr;
    ETCS::RID      m_db = 0;
    ETCS::RID      m_surface = 0;      // repainted after a load -- see BindSurface
    int            m_wait_depth = 0;   // see Waiting
    int64_t        m_current = 0;       // the slot the present came from or went to; 0 is none
    uint64_t       m_loaded_rev = 0;    // the document's revision at that moment -- see dirty()

    // One statement, finalized when the scope ends whatever the path out --
    // which is what makes every early `return false` above leave nothing open
    // for the transaction guard's rollback to trip over.
    struct Stmt
    {
        Database_& db;
        void*      s;
        Stmt(Database_& d, const char* sql) : db(d), s(d.Prepare(sql)) {}
        ~Stmt() { if (s) db.Finalize(s); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        explicit operator bool() const { return s != nullptr; }
        bool bind(int i, const DatabaseValue& v) { return db.Bind(s, i, v); }
        int  step() { return db.Step(s); }
        DatabaseValue col(int c) { DatabaseValue v; db.Column(s, c, v); return v; }
    };

    /*
 * THE PICTURE OF A PAGE, small. Every visible layer, bottom first, each cell
 * of the thumbnail the average of the source block under it, composited in
 * order at the layer's opacity -- the same arithmetic PaintLayerPanel's
 * paint_thumb does for one layer, done for the stack, over the checker so a
 * transparent page still shows as a page. Written at save time, once.
 */
    static void page_thumb(const std::vector<PaintLayer*>& stack, int32_t tw, int32_t th,
                           std::vector<uint8_t>& out)
    {
        out.assign(static_cast<size_t>(tw) * th * 4, 0);
        std::vector<float> acc(static_cast<size_t>(tw) * th * 3, 0.0f);
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float* c = &acc[(static_cast<size_t>(y) * tw + x) * 3];
                c[0] = c[1] = c[2] = light ? 0.44f : 0.34f;
            }
        for (PaintLayer* l : stack)
        {
            if (!l || !l->visible()) continue;
            const int32_t lw = static_cast<int32_t>(l->width()), lh = static_cast<int32_t>(l->height());
            const uint8_t* src = l->PixelData();
            if (!src || lw <= 0 || lh <= 0) continue;
            const float opacity = std::clamp(l->opacity(), 0.0f, 1.0f);
            for (int32_t y = 0; y < th; ++y)
                for (int32_t x = 0; x < tw; ++x)
                {
                    const int32_t sx0 = x * lw / tw, sx1 = std::max(sx0 + 1, (x + 1) * lw / tw);
                    const int32_t sy0 = y * lh / th, sy1 = std::max(sy0 + 1, (y + 1) * lh / th);
                    const int32_t stepx = std::max(1, (sx1 - sx0) / 8), stepy = std::max(1, (sy1 - sy0) / 8);
                    float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                    for (int32_t sy = sy0; sy < sy1 && sy < lh; sy += stepy)
                        for (int32_t sx = sx0; sx < sx1 && sx < lw; sx += stepx)
                        {
                            const uint8_t* sp = src + (static_cast<size_t>(sy) * lw + sx) * 4;
                            const float a = sp[3] / 255.0f;
                            ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a;
                            ab += (sp[2] / 255.0f) * a; aa += a; ++taps;
                        }
                    if (taps == 0 || aa <= 0.0f) continue;
                    const float cover = (aa / taps) * opacity;
                    float* c = &acc[(static_cast<size_t>(y) * tw + x) * 3];
                    c[0] = c[0] * (1.0f - cover) + (ar / aa) * cover;
                    c[1] = c[1] * (1.0f - cover) + (ag / aa) * cover;
                    c[2] = c[2] * (1.0f - cover) + (ab / aa) * cover;
                }
        }
        for (size_t i = 0; i < static_cast<size_t>(tw) * th; ++i)
        {
            out[i * 4 + 0] = paint_to_byte(acc[i * 3 + 0]);
            out[i * 4 + 1] = paint_to_byte(acc[i * 3 + 1]);
            out[i * 4 + 2] = paint_to_byte(acc[i * 3 + 2]);
            out[i * 4 + 3] = 255;
        }
    }

    static DatabaseValue text(const std::string& s) { return DatabaseValue::text(s.data(), s.size()); }
    static std::string str(const DatabaseValue& v)
    {
        return (v.kind == DatabaseValue::Text && v.p) ? std::string(static_cast<const char*>(v.p), v.n) : std::string();
    }
    static int64_t now()
    {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }
    static int64_t last_insert_id(Database_& db)
    {
        Stmt st(db, "SELECT last_insert_rowid()");
        return (st && st.step() == 1) ? st.col(0).i : 0;
    }
    static int64_t count_pages(Database_& db)
    {
        Stmt st(db, "SELECT COUNT(*) FROM pages");
        return (st && st.step() == 1) ? st.col(0).i : 0;
    }

    // Keep the present if leaving it would lose something: a page that changed
    // since it was loaded -- or, for a present that was never given a slot,
    // since this store was bound to it. Not "has layers": every boot page has
    // layers, and a slot for each untouched boot would be a page a session
    // never asked for.
    void flush()
    {
        if (!m_document) return;
        if (dirty()) Save();
    }

    /*
 * THE PAGES AS A STRIP, NOT A RING. Forward from the last page is a NEW page,
 * which is how a second page comes to exist from the keyboard at all -- a
 * ring would have nowhere to put one and would need a separate verb the hand
 * on ctrl+PageDown does not know about. Back from the first page stays. The
 * present is flushed before either move (a changed page is saved; a page that
 * was never given a slot and has layers in it gets one), and loading the page
 * already on screen is refused: it would destroy and rebuild the same layers
 * for nothing and drop the undo history on the way.
 */
    bool step(int dir)
    {
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
        flush();
        std::vector<int64_t> ids;
        {
            ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
            if (!db) { ETCS_LOG("PaintPages", "switch: the database is gone."); return false; }
            Stmt st(*db, "SELECT id FROM pages ORDER BY id");
            if (!st) return false;
            for (int rc = st.step(); rc == 1; rc = st.step()) ids.push_back(st.col(0).i);
        }
        size_t at = ids.size();
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == m_current) { at = i; break; }
        int64_t target = 0;
        if (at == ids.size())                          // the present has no slot
            target = ids.empty() ? 0 : (dir > 0 ? ids.front() : ids.back());
        else if (dir > 0)
            target = (at + 1 < ids.size()) ? ids[at + 1] : 0;
        else
            target = (at > 0) ? ids[at - 1] : ids[at];
        if (target == 0)
        {
            ETCS_LOG("PaintPages", "switch: past the last page -- opening a new one.");
            return New();
        }
        if (target == m_current) { ETCS_LOG("PaintPages", "switch: page " << m_current << " is the first."); return false; }
        return Load(target);
    }
};

#endif // PAINTPROVIDER_PAINTPAGES_H__
