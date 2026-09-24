#ifndef FILEHTMLPAGE_H__
#define FILEHTMLPAGE_H__
#include "../../../ontology.h"
#include "StaticHtmlPage.h"
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <cctype>

// FileHtmlPage — a disk-backed (or externally-mounted) asset tree, built
// entirely out of instances of itself via the SAME addTag<T> typed-child
// mechanism every other entity in this codebase already uses for
// ownership/lifecycle. A directory becomes a FileHtmlPage whose children
// are FileHtmlPage (one per entry, addTag<FileHtmlPage>()'d onto it); a
// file becomes a FileHtmlPage leaf holding that one file's own bytes. No
// separate "directory node" vs "file node" C++ type exists — Kind below
// is a runtime tag on the SAME type, which is what lets the tree recurse
// through addTag<FileHtmlPage>() uniformly at every level rather than
// needing a distinct type per depth.
//
// StaticHtmlPage plays two DELIBERATELY NARROW roles here, never a third
// (folded into the tree itself):
//
//   1. Local fallback (fallback_page_) — synthesized content THIS node
//      owns directly (e.g. an auto-generated directory listing when no
//      real index.html exists beneath it). addTag<StaticHtmlPage>()'d
//      exactly once, tracked by direct pointer, same "create once, hold
//      the pointer" pattern NetworkProvider.h's own TestPage already uses
//      for its own page. Populated via StaticHtmlPage's own existing
//      SetHtmlRaw/SetCssRaw/SetJsRaw work functions — nothing new needed
//      there.
//
//   2. External mount (Kind::Mount, mount_rid_) — a LIVE reference to a
//      StaticHtmlPage owned by some OTHER entity entirely, resolved fresh
//      by RID on every single request (see resolveMountTarget()) rather
//      than copied in once. This is what lets an entity anywhere in the
//      process generate/update its own page (through StaticHtmlPage's
//      already-live-editable Set*Raw functions) and have it appear at a
//      fixed path in this tree with zero extra plumbing on either side —
//      the same "always re-read current content" property NetworkProvider
//      .h's TestPage already relies on for StaticHtmlPage.
//
// Three ways a File-kind leaf gets into the tree, and the difference is
// only WHO CHOSE THE NAME: LoadFromDisk takes the names a directory
// already has, MountFile takes one name you give it for one file you
// name, and InitAsFile is what both of them call. A mount is therefore
// not a fourth kind of thing — Kind::Mount above is the only genuinely
// different one, because it is the only one that re-reads.
//
// What a FileHtmlPage node is NEVER used for: holding a real file's own
// bytes across the html/css/js triple it inherits from HtmlPageBase. That
// triple is deliberately left UNUSED for Directory/File/Mount-kind nodes
// -- see content_'s own comment for why a file's bytes live in a plain
// std::string instead. FileHtmlPage still derives from
// HtmlPageBase<FileHtmlPage> (not Entity directly) purely to stay in the
// same ontology family StaticHtmlPage is in, matching its own
// ResetConcrete/IsActiveConcrete/IsFileBackedConcrete override contract —
// useful if routing code ever wants to treat "anything HtmlPageBase
// family" uniformly, and costs nothing beyond a few unused inherited
// members.
class FileHtmlPage : 
    public HtmlPageBase<FileHtmlPage>, public DeletableBase<FileHtmlPage>, 
    public EphemeralBase<FileHtmlPage>
{
public:
    WIRE_TYPE_IDENTITY(FileHtmlPage);

    enum class Kind : uint8_t { Directory, File, Mount };

    static constexpr const char* kIndexFile = "index.html";
    // kMountCssArg/kMountJsArg moved to StaticHtmlPage, which is what actually
    // knows which paths it answers to -- see resolveMountAsset's replacement
    // below, now a single forwarding call rather than a hand-copy of that
    // page's own routing table.

    FileHtmlPage() = default;
    virtual ~FileHtmlPage() = default;

    // --- HtmlPageBase concrete surface ---
    bool ResetConcrete() override
    {
        // Deliberately does NOT touch children_by_name_, fallback_page_,
        // or the underlying addTag<FileHtmlPage>()'d subtree -- those are
        // real, separately-owned entities. Tearing down a whole subtree
        // is always an explicit operation (removeTag / EntityUnloadEvent
        // on this node), never an implicit side effect of resetting THIS
        // node's own leaf state. Only meaningful for File/Mount-kind
        // nodes being recycled in place.
        content_.clear();
        mime_type_.clear();
        mount_rid_ = 0;
        return true;
    }
    
    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        // Cascades to the whole subtree -- the default now. Deleting a
        // directory node means deleting what is under it; reparenting a
        // subtree up a generation would silently reshape the served tree into
        // something no script asked for.
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }
    
    bool IsActiveConcrete()     const override { return true; }
    bool IsFileBackedConcrete() const override { return kind_ == Kind::File; }

    // --- Node identity ---
    Kind               GetKind()        const { return kind_; }
    const std::string& GetSegmentName() const { return segment_name_; }

    // --- Construction: directory node (has FileHtmlPage children) ---
    void InitAsDirectory(const std::string& segment_name)
    {
        kind_         = Kind::Directory;
        segment_name_ = segment_name;
    }

    // --- Construction: file leaf (owns its own bytes directly) ---
    void InitAsFile(const std::string& segment_name, std::string bytes, std::string mime)
    {
        kind_         = Kind::File;
        segment_name_ = segment_name;
        content_      = std::move(bytes);
        mime_type_    = std::move(mime);
    }

    // --- Construction: mount leaf (serves an external StaticHtmlPage's
    // three assets at fixed sub-paths beneath this node's own path --
    // see the class comment on why StaticHtmlPage is a leaf here, not a
    // tree node). target_rid must name a StaticHtmlPage living in THIS
    // SAME module -- see resolveMountTarget()'s own comment.
    void InitAsMount(const std::string& segment_name, ETCS::RID target_rid)
    {
        kind_         = Kind::Mount;
        segment_name_ = segment_name;
        mount_rid_    = target_rid;
    }

    // --- Local synthesized content, owned directly by THIS node -- e.g.
    // an auto-generated directory listing when no real index.html exists
    // beneath this directory. Created once, tracked directly (never
    // re-added via addTag<StaticHtmlPage>() a second time). Populate via
    // its own existing SetHtmlRaw/SetCssRaw/SetJsRaw work functions.
    StaticHtmlPage* EnsureFallbackPage()
    {
        if (!fallback_page_) fallback_page_ = addTag<StaticHtmlPage>();
        return fallback_page_;
    }
    StaticHtmlPage* GetFallbackPage() const { return fallback_page_; }

    // Default path-extension fallback for ResolveConcrete: when a request
    // segment does not match any child as written (e.g. "/index"), try the
    // same segment with this extension appended (e.g. "/index.html") before
    // declaring a miss. Empty means no fallback -- exact-name only, which is
    // the historical behaviour. Leading '.' is optional on input; stored form
    // always includes it so concatenation is unambiguous. Applies only on
    // the tree root this is set on (HttpServer walks each page root's
    // Resolve), and only when the segment does not already end with the
    // extension -- so "/index.html" still resolves in one step and never
    // becomes "/index.html.html".
    void SetDefaultExtension(const std::string& ext)
    {
        if (ext.empty()) { default_extension_.clear(); return; }
        default_extension_ = (ext[0] == '.') ? ext : ("." + ext);
    }
    const std::string& GetDefaultExtension() const { return default_extension_; }

    /*
     * WHERE THE NEXT MountFile's GO, said once by the caller rather than in
     * every line. A module keeps the list of files its page needs in its own
     * script and mounts them relative to wherever its page lives; the SITE
     * decides where that is. So the site sets the prefix, runs the module's
     * mount script, and clears it:
     *
     *     tree.SetMountPrefix(paint)
     *     run PaintProvider/scripts/paint_mounts.etcs tree=tree
     *     tree.SetMountPrefix()
     *
     * The script cannot be handed the prefix itself -- `run` carries RIDs, not
     * words -- which is why it is state on the tree for the length of the run.
     * MountFile only: MountTree takes one segment and is the site's own call.
     */
    void SetMountPrefix(const std::string& prefix)
    {
        mount_prefix_ = prefix;
        while (!mount_prefix_.empty() && mount_prefix_.back() == '/') mount_prefix_.pop_back();
        while (!mount_prefix_.empty() && mount_prefix_.front() == '/') mount_prefix_.erase(0, 1);
    }
    const std::string& GetMountPrefix() const { return mount_prefix_; }

    // --- Register a mount child directly, not from disk -- the
    // programmatic counterpart to a directory entry LoadFromDisk would
    // have created, for mounting an externally-owned StaticHtmlPage at a
    // chosen path instead of a real file. Same registration step
    // LoadFromDisk already performs for each disk entry (addTag, then
    // index by segment name), just skipping the filesystem read.
    FileHtmlPage* MountChild(const std::string& segment_name, ETCS::RID target_rid)
    {
        FileHtmlPage* child = addTag<FileHtmlPage>();
        child->InitAsMount(segment_name, target_rid);
        children_by_name_[segment_name] = child;
        return child;
    }

    // --- Serve ONE FILE from disk at ONE url path ---
    //
    // The single-file counterpart to LoadFromDisk. LoadFromDisk exposes a whole
    // directory and takes the names it finds; this takes one name you choose and
    // one file you name, and exposes nothing else -- so a file can be served
    // without the directory it lives in becoming reachable.
    //
    // WHY NOT MountExternal FOR THIS. That one forwards to a StaticHtmlPage,
    // which is a PAGE, not a file: its bytes go through an NBuffer (a hard
    // ETCS_NETWORK_MAX_HEADER_SIZE ceiling -- LoadFileIntoBuffer refuses
    // anything larger), its path is canonicalised against the CURRENT WORKING
    // DIRECTORY and refused if it leaves it, it answers "/", "/index.html",
    // "/style.css" and "/app.js" rather than the one path asked for, and it
    // carries no extension to derive a MIME type from, so everything it serves
    // is text/html. Mounting a .wasm that way gets you the wrong Content-Type
    // on the small ones and nothing at all on the real ones. MountExternal is
    // for a LIVE page some other entity keeps rewriting; that is the job it is
    // good at, and it is not this one.
    //
    // This makes a File-kind node instead -- exactly what LoadFromDisk builds
    // for a real file, with the same unbounded std::string storage and the same
    // MimeForExtension lookup, just chosen by hand instead of found by a walk.
    //
    // url_path may contain '/': intermediate Directory nodes are created as
    // needed, so "assets/etcs.wasm" works with no matching directory on disk.
    // Read once, here, like LoadFromDisk -- a later edit to the file needs a
    // re-mount, which is the trade for not re-reading on every request.
    bool MountFile(const std::string& asked_path, const std::string& disk_path)
    {
        const std::string url_path = mount_prefix_.empty() ? asked_path : (mount_prefix_ + "/" + asked_path);
        std::vector<std::string> segments;
        size_t start = 0;
        while (start <= url_path.size())
        {
            size_t slash = url_path.find('/', start);
            std::string seg = (slash == std::string::npos)
                ? url_path.substr(start)
                : url_path.substr(start, slash - start);
            if (!seg.empty()) segments.push_back(seg);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (segments.empty())
        {
            ETCS_LOG("FileHtmlPage", "MountFile: empty url path for '"
                     << disk_path << "' -- nothing mounted.");
            return false;
        }

        // READ FIRST, mutate second. A failed open must not leave new Directory
        // nodes behind on a path that will never resolve -- ListPaths would then
        // advertise a 404 as a served path, which is the one thing it exists to
        // rule out.
        std::ifstream in(disk_path, std::ios::binary);
        if (!in.is_open())
        {
            if (!warn_if_unexpanded(disk_path, "MountFile"))
                ETCS_LOG("FileHtmlPage", "MountFile: failed to open '" << disk_path
                         << "' -- nothing mounted at '" << url_path << "'.");
            return false;
        }
        std::string bytes((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

        FileHtmlPage* node = this;
        for (size_t i = 0; i + 1 < segments.size(); ++i)
        {
            auto it = node->children_by_name_.find(segments[i]);
            if (it != node->children_by_name_.end())
            {
                // Refused rather than descended into: a File or a Mount has no
                // children Resolve would ever consult on the way past it, so
                // hanging a leaf under one mounts something unreachable and
                // reports success.
                if (it->second->kind_ != Kind::Directory)
                {
                    ETCS_LOG("FileHtmlPage", "MountFile: '" << segments[i]
                             << "' on the way to '" << url_path
                             << "' is not a directory -- nothing mounted.");
                    return false;
                }
                node = it->second;
                continue;
            }
            FileHtmlPage* dir = node->addTag<FileHtmlPage>();
            dir->kind_         = Kind::Directory;
            dir->segment_name_ = segments[i];
            node->children_by_name_[segments[i]] = dir;
            node = dir;
        }

        // MIME from the DISK name, not the url name, because the url name is
        // the part a caller is free to invent: mounting index.html at "shell"
        // still serves text/html.
        const std::string& leaf = segments.back();
        // Said out loud rather than silently won: the displaced node stays
        // attached as a typed child and only leaves the name index, so a mount
        // that shadows a real file looks like it worked and the file looks like
        // it vanished. Same overwrite LoadFromDisk's own index does, made visible.
        if (node->children_by_name_.count(leaf))
            ETCS_LOG("FileHtmlPage", "MountFile: '" << url_path
                     << "' replaces an entry already at that path.");

        FileHtmlPage* child = node->addTag<FileHtmlPage>();
        child->InitAsFile(leaf, std::move(bytes), MimeForExtension(disk_path));
        node->children_by_name_[leaf] = child;

        ETCS_LOG("FileHtmlPage", "MountFile: '" << disk_path << "' -> '/" << url_path
                 << "' (" << child->content_.size() << " bytes, " << child->mime_type_
                 << ") under RID:" << getRID());
        return true;
    }

    // --- Serve a whole DIRECTORY at one url prefix ---
    //
    // LoadFromDisk takes the names it finds and answers them at THIS node's own
    // level; this is "that directory, but under /paint", which is the shape a
    // second SELF-CONTAINED site needs -- one that brings its own index.html.
    // Loaded flat beside the first, the two index.html files land on the same
    // name and which one wins is attach order, the same fragility
    // run_website.etcs's header has to explain about "/".
    //
    // So: one named child, LoadFromDisk'd into. Resolution needs nothing new --
    // a Directory child answers its own index.html at /paint/ and redirects
    // /paint there (ResolveConcrete's Directory branch), so every relative url
    // inside that page resolves under the prefix without the page knowing it
    // was mounted.
    //
    // WHY NOT MountFile PER ASSET. The paint page is ~9 MB across ~20 files in
    // four directories, and its own modules.json is the source of truth for
    // which ones -- a hand-written mount list here would be a second copy of
    // that list, silently stale the first time a provider is added to the page.
    //
    // Returns the entry count LoadFromDisk took, so the caller can compare it
    // against what it expected; 0 means the directory was unreadable or empty
    // and every path under the prefix will 404, which LoadFromDisk itself logs.
    size_t MountTree(const std::string& url_segment, const std::string& disk_path)
    {
        if (url_segment.empty() || url_segment.find('/') != std::string::npos)
        {
            ETCS_LOG("FileHtmlPage", "MountTree: '" << url_segment << "' is not a single "
                     "path segment -- a prefix is one name, and nesting it here would "
                     "duplicate MountFile's segment walk. Nothing mounted.");
            return 0;
        }

        kind_ = Kind::Directory;

        // Replace rather than shadow: two children under one name would leave
        // the loser resident for the process's life, holding its whole tree of
        // file contents, reachable from nothing.
        auto existing = children_by_name_.find(url_segment);
        if (existing != children_by_name_.end())
            ETCS_LOG("FileHtmlPage", "MountTree: '/" << url_segment
                     << "' replaces an entry already at that path.");

        FileHtmlPage* child = addTag<FileHtmlPage>();
        child->segment_name_ = url_segment;
        const size_t taken = child->LoadFromDisk(disk_path);
        children_by_name_[url_segment] = child;

        ETCS_LOG("FileHtmlPage", "MountTree: '" << disk_path << "' -> '/" << url_segment
                 << "/' (" << taken << " entries) under RID:" << getRID());
        return taken;
    }

    /*
 * A PATH THAT STILL SAYS ACE_ROOT IS A VERSION SKEW, NOT A MISSING FILE.
 *
 * ACE_ROOT is expanded by the executor before a statement's arguments ever
 * reach a work function (core/CommandExecutor.h). So a literal one arriving
 * here means the script being run is NEWER than the binary running it, and the
 * open that follows will fail for a reason that has nothing to do with the
 * filesystem -- "failed to open 'ACE_ROOT/modules/...'" reads as a wrong path
 * and sends the reader looking in the wrong place. This is the same class of
 * mistake the loader/module manifest check catches, on the one axis it does not
 * cover: a .etcs script is DATA, so editing one takes effect immediately, while
 * the runtime that interprets it does not change until it is rebuilt.
 *
 * Named here rather than guarded against, because the module cannot fix it --
 * it can only say what happened, once, at the first path that shows it.
 */
    static bool warn_if_unexpanded(const std::string& path, const char* verb)
    {
        if (path.compare(0, 9, "ACE_ROOT/") != 0) return false;
        ETCS_LOG("FileHtmlPage", verb << ": '" << path << "' still contains ACE_ROOT, "
                 "which the executor expands before a work function sees it. This "
                 "runtime does not, so it is older than the script -- rebuild the "
                 "NATIVE binaries with `ace make all`. `ace wasm make ...` builds "
                 "only the browser artifacts and never touches bin/*.so or bin/etcs, "
                 "which is what a server script like this one actually runs on.");
        return true;
    }

    // --- Tree construction from disk ---
    //
    // Walks disk_path non-recursively at each level, addTag<FileHtmlPage>
    // ()-ing one child per directory entry and recursing into it for a
    // subdirectory, or reading the whole file in one shot for a regular
    // file. Every file's ENTIRE content is read into an arbitrary-length
    // std::string -- no fixed-capacity ceiling the way a cross-ABI
    // Buffer/TBuffer<N> has, since none of this ever needs to cross a
    // wire boundary; it's read once at load time and served directly out
    // of process memory for the module's whole lifetime. Symlinks and
    // other special entries are skipped rather than guessed at.
    // Returns how many entries it took. ZERO IS NOT AN ERROR ON ITS OWN -- an
    // empty directory is a legal thing to serve -- but it is the only number a
    // caller can compare against what it expected, so it is handed back rather
    // than swallowed. See the work function, which is what reports it.
    size_t LoadFromDisk(const std::string& disk_path)
    {
        kind_ = Kind::Directory;
        warn_if_unexpanded(disk_path, "LoadFromDisk");

        namespace fs = std::filesystem;
        std::error_code ec;

        /*
 * CHECKED HERE, BEFORE THE LOOP, and that is the whole of this fix.
 *
 * The check used to be the first statement INSIDE the range-for -- which never
 * runs when the construction failed, because a directory_iterator that took an
 * error compares equal to end(). So a path that does not exist walked zero
 * entries, reported nothing, and left a Directory node with no children; every
 * request under it then 404'd with the log insisting the tree had loaded. A
 * mistyped or unresolved path is the most likely thing to be wrong here and it
 * was the one thing that said nothing.
 */
        auto it = fs::directory_iterator(disk_path, ec);
        if (ec)
        {
            ETCS_LOG("FileHtmlPage", "LoadFromDisk: cannot read '" << disk_path
                     << "' -- " << ec.message() << ". Nothing was loaded, so every "
                     "path under this tree will 404.");
            return 0;
        }

        size_t taken = 0;
        for (const auto& entry : it)
        {

            const std::string name = entry.path().filename().string();
            FileHtmlPage* child = addTag<FileHtmlPage>();

            if (entry.is_directory())
            {
                child->segment_name_ = name;
                child->LoadFromDisk(entry.path().string());
            }
            else if (entry.is_regular_file())
            {
                std::ifstream in(entry.path(), std::ios::binary);
                if (!in.is_open())
                {
                    ETCS_LOG("FileHtmlPage", "LoadFromDisk: failed to open '"
                             << entry.path().string() << "' -- skipping.");
                    continue;
                }
                std::string bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                child->InitAsFile(name, std::move(bytes), MimeForExtension(name));
            }
            else
            {
                continue; // symlink / special file -- skip rather than guess intent
            }
            ++taken;

            children_by_name_[name] = child;
        }
        return taken;
    }

    // --- Path resolution ---
    //
    // Flattened to a raw {data, length, mime_type} triple right at the
    // point of resolution, regardless of whether the underlying source is
    // this node's own std::string content_ or a StaticHtmlPage's NBuffer
    // (fallback_page_ / a resolved mount) -- avoids owning or copying
    // anything, and gives the caller one uniform shape to build an HTTP
    // response from either way (matching the existing
    // "%.*s"/Content-Length snprintf pattern NetworkProvider.h's own
    // TestPage/StartWebserver already use).
    HtmlPage_::ResolvedAsset ResolveConcrete(const std::string& request_path) const override
    {
        HtmlPage_::ResolvedAsset result;

        std::vector<std::string> segments;
        size_t start = 0;
        while (start <= request_path.size())
        {
            size_t slash = request_path.find('/', start);
            std::string seg = (slash == std::string::npos)
                ? request_path.substr(start)
                : request_path.substr(start, slash - start);
            if (!seg.empty()) segments.push_back(seg);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }

        const FileHtmlPage* node = this;
        size_t i = 0;
        for (; i < segments.size(); ++i)
        {
            if (node->kind_ == Kind::Mount) break;
            auto it = node->children_by_name_.find(segments[i]);
            if (it == node->children_by_name_.end()
                && !default_extension_.empty())
            {
                // Bare name missed -- try the configured default extension
                // once. Only when the segment does not already end with it,
                // so an explicit "/index.html" is never rewritten.
                const std::string& seg = segments[i];
                const std::string& ext = default_extension_;
                const bool already =
                    seg.size() >= ext.size()
                    && seg.compare(seg.size() - ext.size(), ext.size(), ext) == 0;
                if (!already)
                    it = node->children_by_name_.find(seg + ext);
            }
            if (it == node->children_by_name_.end()) return result; // not found
            node = it->second;
        }

        if (node->kind_ == Kind::Mount)
        {
            // Everything after the mount point is the target's own path.
            // Forwarded verbatim rather than interpreted here: which paths a
            // StaticHtmlPage answers to is its business, and duplicating that
            // table here is exactly the drift this restructure removes.
            std::string remainder;
            for (size_t j = i; j < segments.size(); ++j)
                remainder += "/" + segments[j];
            StaticHtmlPage* target = node->resolveMountTarget();
            if (!target) return result;
            return target->Resolve(remainder);
        }

        if (node->kind_ == Kind::File)
        {
            result.matched   = true;
            result.data      = node->content_.data();
            result.length    = node->content_.size();
            result.mime_type = node->mime_type_;
            return result;
        }

        // Directory: a real index child first (index.html by default, or
        // "index" + default_extension_ when one is set), then a local
        // synthesized fallback, else genuinely not found.
        const std::string index_name = default_extension_.empty()
            ? std::string(kIndexFile)
            : ("index" + default_extension_);
        auto idx_it = node->children_by_name_.find(index_name);
        const bool has_index = idx_it != node->children_by_name_.end()
                            && idx_it->second->kind_ == Kind::File;
        if (!has_index && !node->fallback_page_) return result;   // not found

        // ASKED FOR WITHOUT ITS TRAILING SLASH: send the client to the slashed
        // spelling rather than answer with the index. The page's relative URLs
        // ("modules.json", "shell") resolve against the request URL in the
        // browser, so the bytes at "/paint" are a page whose every fetch goes
        // to "/" -- see ResolvedAsset in ontology/HtmlPage.h. Only for a
        // directory that would answer, so a miss stays one 404 rather than a
        // redirect to one. The root never gets here without a slash (no
        // segments), and a segment that resolved through the default
        // extension is a File, not a Directory.
        if (!segments.empty() && !request_path.empty() && request_path.back() != '/')
        {
            result.matched  = true;
            result.redirect = request_path + "/";
            return result;
        }

        if (has_index)
        {
            result.matched   = true;
            result.data      = idx_it->second->content_.data();
            result.length    = idx_it->second->content_.size();
            result.mime_type = idx_it->second->mime_type_;
            return result;
        }
        const ETCS::NBuffer& nb = node->fallback_page_->GetHtmlContent();
        result.matched   = true;
        result.data      = nb.buf;
        result.length    = nb.written;
        result.mime_type = "text/html";
        return result;
    }

    // --- Enumerate every resolvable path beneath this node ---
    //
    // Purely a discovery aid -- e.g. printed once before a server starts
    // listening, so there's a concrete list of what to actually request.
    // Resolve() never consults this; it walks the tree structurally on
    // every call instead, so this list can never drift out of sync with
    // what Resolve() would actually answer. Sorted for stable output --
    // children_by_name_ is an unordered_map, so raw iteration order isn't
    // meaningful.
    std::vector<std::string> ListAllPaths() const
    {
        std::vector<std::string> out;
        CollectPaths("", out);
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    Kind        kind_ = Kind::Directory;
    std::string segment_name_;

    // File-kind storage -- plain heap string, arbitrary length, read once
    // at LoadFromDisk time. Deliberately NOT NBuffer/Buffer here: those
    // exist for cross-ABI wire transport (Entity::call / MirrorBuffer),
    // which this never needs -- it's read once and served directly out of
    // process memory for the module's whole lifetime. Chunking into
    // Buffer-sized (256-byte) frames only needs to happen at actual SEND
    // time (see NetworkProvider.h's ProduceResponse for the existing
    // chunking pattern this would reuse when a response exceeds
    // SocketConnectionState's own fixed send_buf_ size), not at storage
    // time.
    std::string content_;
    std::string mime_type_;

    // Directory-kind: real sub-paths, keyed by path segment. Entity
    // ownership/lifecycle is still addTag<FileHtmlPage>()'s job (loader-
    // ordered registration, typed_children_ RIDList, parent_/parent_rid_
    // teardown wiring) -- this map is PURELY a same-name lookup index on
    // top of that, never itself responsible for construction/destruction.
    // Heap-backed (default allocator): it holds only keys and pointers,
    // never file content, so there's no reason to route it through this
    // entity's own local_arena_ the way Entity's own tags/typed_children_
    // maps do for their actual payload.
    std::unordered_map<std::string, FileHtmlPage*> children_by_name_;

    // Tree-root only (see SetDefaultExtension): optional suffix tried when
    // a path segment misses its exact-name child. Empty = exact-name only.
    std::string default_extension_;

    // Put in front of every MountFile path while set (SetMountPrefix).
    std::string mount_prefix_;

    // Directory-kind: local, synthesized fallback content -- see
    // EnsureFallbackPage()'s own comment.
    StaticHtmlPage* fallback_page_ = nullptr;

    // Mount-kind: RID of an externally-owned StaticHtmlPage, resolved
    // fresh on every request via THIS module's own EventNode::ridMap --
    // see resolveMountTarget()'s own comment for why this is currently
    // restricted to same-module targets.
    ETCS::RID mount_rid_ = 0;

    // ListAllPaths()'s own recursive walk. `prefix` is the URL path
    // already consumed to reach `this` ("" at the tree root). A
    // Directory contributes its own root path ONLY if something would
    // actually answer a request for it (a real index.html child, or a
    // local fallback_page_) -- matching Resolve()'s own fallback order
    // exactly, so this never lists a path Resolve() would 404 on, or
    // omits one it would actually serve.
    void CollectPaths(const std::string& prefix, std::vector<std::string>& out) const
    {
        switch (kind_)
        {
            case Kind::File:
                out.push_back(prefix.empty() ? "/" : prefix);
                return;

            case Kind::Mount:
            {
                // The three paths a StaticHtmlPage answers to, named from that
                // type rather than restated here. Leading slashes stripped
                // since base already ends in one.
                std::string base = prefix.empty() ? "/" : (prefix + "/");
                out.push_back(base);
                out.push_back(base + (StaticHtmlPage::kCssPath + 1));
                out.push_back(base + (StaticHtmlPage::kJsPath  + 1));
                return;
            }

            case Kind::Directory:
            {
                auto idx_it = children_by_name_.find(kIndexFile);
                bool has_index = idx_it != children_by_name_.end()
                               && idx_it->second->kind_ == Kind::File;
                if (has_index || fallback_page_)
                    out.push_back(prefix.empty() ? "/" : (prefix + "/"));

                for (const auto& [name, child] : children_by_name_)
                {
                    std::string child_prefix = prefix.empty() ? ("/" + name) : (prefix + "/" + name);
                    child->CollectPaths(child_prefix, out);
                }
                return;
            }
        }
    }

    static std::string MimeForExtension(const std::string& name)
    {
        auto dot = name.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : name.substr(dot + 1);
        for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));

        if (ext == "html" || ext == "htm") return "text/html";
        if (ext == "css")                  return "text/css";
        if (ext == "js"   || ext == "mjs") return "application/javascript";
        if (ext == "json")                 return "application/json";
        if (ext == "svg")                  return "image/svg+xml";
        if (ext == "png")                  return "image/png";
        if (ext == "jpg"  || ext == "jpeg") return "image/jpeg";
        if (ext == "gif")                  return "image/gif";
        if (ext == "ico")                  return "image/x-icon";
        if (ext == "webp")                 return "image/webp";
        if (ext == "wasm")                 return "application/wasm";
        if (ext == "woff2")                return "font/woff2";
        if (ext == "woff")                 return "font/woff";
        if (ext == "map")                  return "application/json"; // sourcemaps
        if (ext == "txt")                  return "text/plain";
        if (ext == "etcs")                 return "text/plain"; // raw script source, viewable not downloaded
        return "application/octet-stream";
    }


    // Same-module lookup only, by design: EventNode::getInstance() here
    // resolves to THIS module's own local RID registry (NetworkProvider's
    // own), keyed by bare type tag -- exactly the same lookup
    // SocketConnectionState's own page_rid_ already relies on implicitly.
    // Mounting a StaticHtmlPage hosted in a DIFFERENT module would need a
    // loader-mediated resolve event (not yet built) -- restricting to
    // same-module keeps this on solid, already-demonstrated ground rather
    // than reaching for unverified cross-module machinery.
    StaticHtmlPage* resolveMountTarget() const
    {
        if (mount_rid_ == 0) return nullptr;
        auto& ridMap = ETCS::EventNode::getInstance().ridMap;
        auto it = ridMap.find(ETCS::Buffer("StaticHtmlPage"));
        if (it == ridMap.end()) return nullptr;
        ETCS::Entity* e = it->second.invoke_get(mount_rid_);
        if (!e) return nullptr;
        // dynamic_cast, not static_cast -- StaticHtmlPage may inherit
        // Entity virtually (see Entity.h's own addTagTrampoline comment
        // on why base-to-derived conversions across this ontology can't
        // assume a plain static_cast is even legal).
        return dynamic_cast<StaticHtmlPage*>(e);
    }
};

#endif // FILEHTMLPAGE_H__
