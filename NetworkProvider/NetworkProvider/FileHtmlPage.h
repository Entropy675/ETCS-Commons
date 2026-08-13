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
    bool ResetConcrete()
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
    
    bool DeleteConcrete() 
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
    
    bool IsActiveConcrete()     const { return true; }
    bool IsFileBackedConcrete() const { return kind_ == Kind::File; }

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
    void LoadFromDisk(const std::string& disk_path)
    {
        kind_ = Kind::Directory;

        namespace fs = std::filesystem;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(disk_path, ec))
        {
            if (ec)
            {
                ETCS_LOG("FileHtmlPage", "LoadFromDisk: directory_iterator error on '"
                         << disk_path << "': " << ec.message());
                break;
            }

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

            children_by_name_[name] = child;
        }
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
    HtmlPage_::ResolvedAsset ResolveConcrete(const std::string& request_path) const
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

        // Directory: try a real index.html child first, then a local
        // synthesized fallback, else genuinely not found.
        auto idx_it = node->children_by_name_.find(kIndexFile);
        if (idx_it != node->children_by_name_.end() && idx_it->second->kind_ == Kind::File)
        {
            result.matched   = true;
            result.data      = idx_it->second->content_.data();
            result.length    = idx_it->second->content_.size();
            result.mime_type = idx_it->second->mime_type_;
            return result;
        }
        if (node->fallback_page_)
        {
            const ETCS::NBuffer& nb = node->fallback_page_->GetHtmlContent();
            result.matched   = true;
            result.data      = nb.buf;
            result.length    = nb.written;
            result.mime_type = "text/html";
            return result;
        }
        return result; // not found
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
