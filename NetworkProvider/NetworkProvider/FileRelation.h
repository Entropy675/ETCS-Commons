#ifndef NETWORKPROVIDER_FILERELATION_H__
#define NETWORKPROVIDER_FILERELATION_H__

#include "../../../ontology.h"

#include <cstdint>

#include <cctype>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

/*
 * FileRelation -- a Foreign relation (ontology/Foreign.h) to one directory on
 * a medium that does not run ETCS: a removable drive, a folder that gets
 * printed. Its stand-in is whatever Directory rolls what that medium holds
 * (CryptProvider's Custody for a key's custodian). Locked once active, as
 * every relation is: Root and Target refuse after Activate. To move it,
 * delete the relation and make another.
 *
 *   QUESTION  PUT <name>\n<body>      a new file, 0600; never overwrites
 *             GET <name>[\n<fill>]    the file; into <fill> when it is private
 *             LIST                    the names, one a line
 *             DROP <name>             unlink it
 *   ANSWER    "ok" | the file | the names
 *
 * SECRETS ARE FILLED HERE. A body may name {private:<rid>}: the entity must
 * claim Private, and its Reveal is written only into the file. A GET whose
 * second line names one hands the file to that entity's Receive and answers
 * "ok". A name carrying private material ends in ".private", and a GET of
 * such a name without a fill is refused -- whatever the medium keeps of file
 * modes, the name is what says it. Logs carry the question's first line only.
 *
 * Names are [A-Za-z0-9._-], not leading '.', at most 128: nothing that walks
 * out of the root.
 */
class FileRelation : public ForeignBase<FileRelation>, public DeletableBase<FileRelation>
{
public:
    WIRE_TYPE_IDENTITY(FileRelation);

    FileRelation() = default;

    // --- Foreign_ / Relational_ -------------------------------------------------
    std::string SpeaksConcrete() const override { return "file"; }
    std::string TargetsConcrete() const override { std::lock_guard<std::mutex> l(mu_); return target_; }

    bool ExchangeConcrete(const std::string& question, std::string& answer, ETCS::SignalContext ctx) override
    {
        (void)ctx;
        answer.clear();
        if (StandIn() == 0) { ETCS_LOG("FileRelation", "refused: not active (no stand-in bound)."); return false; }
        const size_t nl = question.find('\n');
        const std::string head = question.substr(0, nl);
        const std::string body = nl == std::string::npos ? std::string() : question.substr(nl + 1);
        const size_t sp = head.find(' ');
        const std::string verb = head.substr(0, sp);
        const std::string name = sp == std::string::npos ? std::string() : head.substr(sp + 1);
        ETCS_LOG("FileRelation", head << "  @ " << root());

        if (verb == "LIST" && name.empty()) return list(answer);
        if (!validName(name)) { ETCS_LOG("FileRelation", "refused: '" << name << "' is not a custody name."); return false; }
        if (verb == "PUT")  return put(name, body, answer);
        if (verb == "GET")  return get(name, body, answer);
        if (verb == "DROP") return drop(name, answer);
        ETCS_LOG("FileRelation", "refused: '" << verb << "' (PUT, GET, LIST, DROP).");
        return false;
    }

    bool ApplyConcrete(ETCS::Entity& target, ETCS::SignalContext ctx) override
    {
        (void)target; (void)ctx;
        ETCS_LOG("FileRelation", "Apply: pulled, not pushed -- the stand-in asks through Exchange.");
        return false;
    }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- configuration: refused once active --------------------------------------
    bool Root(const std::string& dir)
    {
        struct stat st{};
        if (refuseActive("Root")) return false;
        if (dir.empty() || ::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        { ETCS_LOG("FileRelation", "Root: '" << dir << "' is not a directory."); return false; }
        std::lock_guard<std::mutex> l(mu_);
        root_ = dir.back() == '/' ? dir.substr(0, dir.size() - 1) : dir;
        return true;
    }
    bool Target(const std::string& tags)
    {
        if (refuseActive("Target") || tags.empty()) return false;
        std::lock_guard<std::mutex> l(mu_);
        target_ = tags;
        return true;
    }
    bool Activate(ETCS::RID stand_in)
    {
        if (refuseActive("Activate")) return false;
        { std::lock_guard<std::mutex> l(mu_); if (root_.empty() || target_.empty()) { ETCS_LOG("FileRelation", "Activate: set Root and Target first."); return false; } }
        if (!BindStandIn(stand_in)) { ETCS_LOG("FileRelation", "Activate: RID " << stand_in << " doesn't carry '" << target_ << "'."); return false; }
        ETCS_LOG("FileRelation", "active: " << root() << " for stand-in RID:" << stand_in << " (configuration locked).");
        return true;
    }

private:
    static ETCS::Entity* resolve(ETCS::RID rid)
    {
        return rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
    }
    // Material passed through here is zeroed before its storage goes back.
    static void wipe(void* p, size_t n)
    {
        volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
        while (n--) *v++ = 0;
    }
    static bool validName(const std::string& n)
    {
        if (n.empty() || n.size() > 128 || n[0] == '.') return false;
        for (char c : n)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
        return true;
    }
    static bool isPrivateName(const std::string& n)
    {
        static const std::string s = ".private";
        return n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0;
    }
    // {private:<rid>} -> that entity's Reveal. False on any fill that doesn't resolve.
    static bool fill(std::string& body, bool& had_private)
    {
        had_private = false;
        body.reserve(body.size() + 4096);   // filled in place: no unwiped copy left behind by a grow
        size_t at = 0;
        while ((at = body.find("{private:", at)) != std::string::npos)
        {
            const size_t end = body.find('}', at);
            if (end == std::string::npos) return false;
            ETCS::RID rid = 0;
            try { rid = static_cast<ETCS::RID>(std::stoull(body.substr(at + 9, end - at - 9))); } catch (...) { return false; }
            ETCS::Entity* e = resolve(rid);
            Private_* p = e ? static_cast<Private_*>(e->getInterfacePointer(ETCS::Buffer("Private"))) : nullptr;
            std::string secret;
            if (!p || !p->Reveal(secret)) { ETCS_LOG("FileRelation", "refused: RID " << rid << " reveals nothing."); return false; }
            body.replace(at, end - at + 1, secret);
            at += secret.size();
            wipe(&secret[0], secret.size());
            had_private = true;
        }
        return true;
    }
    static Private_* privateAt(const std::string& line)
    {
        if (line.compare(0, 9, "{private:") != 0 || line.back() != '}') return nullptr;
        ETCS::RID rid = 0;
        try { rid = static_cast<ETCS::RID>(std::stoull(line.substr(9, line.size() - 10))); } catch (...) { return nullptr; }
        ETCS::Entity* e = resolve(rid);
        return e ? static_cast<Private_*>(e->getInterfacePointer(ETCS::Buffer("Private"))) : nullptr;
    }

    bool put(const std::string& name, std::string body, std::string& answer) const
    {
        bool had_private = false;
        const bool filled = fill(body, had_private);
        if (filled && had_private != isPrivateName(name))
        {
            ETCS_LOG("FileRelation", "refused: private material goes only to a '.private' name, and only it.");
            wipe(&body[0], body.size());
            return false;
        }
        bool ok = filled;
        if (ok)
        {
            const std::string path = root() + "/" + name;
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) { ETCS_LOG("FileRelation", "PUT " << name << ": " << (errno == EEXIST ? "already held -- custody never overwrites." : "cannot create.")); ok = false; }
            else
            {
                size_t at = 0;
                while (ok && at < body.size())
                {
                    const ssize_t w = ::write(fd, body.data() + at, body.size() - at);
                    if (w < 0 && errno == EINTR) continue;
                    if (w <= 0) ok = false; else at += static_cast<size_t>(w);
                }
                ok = ok && ::fsync(fd) == 0;
                ::close(fd);
                if (!ok) { ::unlink(path.c_str()); ETCS_LOG("FileRelation", "PUT " << name << ": write failed, removed."); }
            }
        }
        if (!body.empty()) wipe(&body[0], body.size());
        if (ok) answer = "ok";
        return ok;
    }

    bool get(const std::string& name, const std::string& body, std::string& answer) const
    {
        Private_* into = body.empty() ? nullptr : privateAt(body.substr(0, body.find('\n')));
        if (!body.empty() && !into) { ETCS_LOG("FileRelation", "GET " << name << ": the fill is not a Private entity."); return false; }
        if (isPrivateName(name) != (into != nullptr))
        {
            ETCS_LOG("FileRelation", "GET " << name << ": a '.private' name is read only into a Private entity, and only it.");
            return false;
        }
        std::string content;
        if (!read(name, content)) return false;
        if (!into) { answer = std::move(content); return true; }
        const bool ok = into->Receive(content);
        if (!content.empty()) wipe(&content[0], content.size());
        if (ok) answer = "ok";
        return ok;
    }

    bool list(std::string& answer) const
    {
        DIR* d = ::opendir(root().c_str());
        if (!d) return false;
        while (dirent* e = ::readdir(d))
        {
            const std::string n = e->d_name;
            struct stat st{};
            if (validName(n) && ::stat((root() + "/" + n).c_str(), &st) == 0 && S_ISREG(st.st_mode)) answer += n + "\n";
        }
        ::closedir(d);
        return true;
    }

    bool drop(const std::string& name, std::string& answer) const
    {
        if (::unlink((root() + "/" + name).c_str()) != 0) { ETCS_LOG("FileRelation", "DROP " << name << ": not held."); return false; }
        answer = "ok";
        return true;
    }

    bool read(const std::string& name, std::string& out) const
    {
        const int fd = ::open((root() + "/" + name).c_str(), O_RDONLY);
        if (fd < 0) { ETCS_LOG("FileRelation", "GET " << name << ": not held."); return false; }
        struct stat st{};
        if (::fstat(fd, &st) == 0 && st.st_size > 0) out.reserve(static_cast<size_t>(st.st_size) + 1);
        char buf[4096];
        ssize_t r;
        while ((r = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(r));
        wipe(buf, sizeof buf);
        ::close(fd);
        return r == 0;
    }

    std::string root() const { std::lock_guard<std::mutex> l(mu_); return root_; }

    bool refuseActive(const char* what) const
    {
        if (StandIn() == 0) return false;
        ETCS_LOG("FileRelation", what << ": refused, the relation is active. Delete it and make another.");
        return true;
    }

    mutable std::mutex mu_;
    std::string        root_;
    std::string        target_;
};

#endif // NETWORKPROVIDER_FILERELATION_H__
