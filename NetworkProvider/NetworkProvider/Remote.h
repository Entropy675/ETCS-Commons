#ifndef NETWORKPROVIDER_REMOTE_H__
#define NETWORKPROVIDER_REMOTE_H__
#include "../../../ontology.h"
#include "Peer.h"

#include <string>

/*
 * Remote -- the child that makes its parent a SURFACE of a node in another
 * runtime (ontology/Remote.h says what that means and why the parent is the
 * real type rather than a stand-in).
 *
 *   spawn NetworkProvider::Peer peer
 *   peer.Connect(wss://example.org/link/studio)
 *   spawn PaintProvider::PaintNode canvas          <- the same type as the host's
 *   canvas.spawn(NetworkProvider::Remote remote)
 *   remote.Bind(canvas @peer)                       <- the host's export name
 *
 * From Bind until Unbind (or this child going), every verb addressed to
 * `canvas` runs on the host's node, and either half of a stream pair naming
 * `canvas` runs there too -- all of it on the Peer's one edge (Edge.h).
 *
 * Bind is refused unless `canvas` carries the host node's authority layer:
 * the same Wrapper children on the network scope, which the script attaches
 * like any child (`canvas.spawn(NetworkProvider::Seal seal)`, `seal.Key(...)`).
 * A wrapper whose type this runtime cannot construct cannot be attached, and
 * then the node cannot be bound at all. What stays here is only what is about the link --
 * these verbs, on this child -- so nothing the far node answers to is
 * shadowed by something only this side has.
 *
 * The peer may be omitted when the surface's own parent is the Peer
 * (`peer.spawn(PaintProvider::PaintNode canvas)`).
 */
class Remote : public RemoteBase<Remote>, public DeletableBase<Remote>
{
public:
    WIRE_TYPE_IDENTITY(Remote);

    Remote()  = default;
    ~Remote() { Unbind(); }

    bool Bind(const std::string& name, ETCS::RID peer_rid)
    {
        ETCS::Entity* surface = getParent();
        if (!surface) { ETCS_LOG("Remote", "Bind: no parent -- spawn this under the surface."); return false; }
        if (!peer_rid && surface->getParent() && isPeer(surface->getParent()))
            peer_rid = surface->getParent()->getRID();
        Peer* peer = resolvePeer(peer_rid);
        auto edge = peer ? peer->edge() : nullptr;
        if (!edge || !edge->isOpen())
        { ETCS_LOG("Remote", "Bind: no linked Peer (RID:" << peer_rid << ")."); return false; }

        Unbind();
        const std::string module = surface->getSourceModule().toString();
        const std::string tag    = surface->getSourceTag().toString();
        const uint64_t    hash   = surface->tagHash(ETCS::Buffer(tag.c_str()));
        // The surface's authority layer, which must be the far node's own.
        const std::string guard  = ETCS::MirrorBuffer::manifestOf(surface, ETCS::WireScope::Socket);
        std::string why;
        ETCS::RID far = 0;
        if (!edge->bind(name, module, tag, hash, guard, far, why))
        {
            ETCS_LOG("Remote", "Bind '" << name << "' refused: " << why);
            return false;
        }
        name_     = name;
        peer_rid_ = peer_rid;
        far_rid_  = far;
        surface_  = surface;
        surface->setRemoteWire(static_cast<ETCS::IWireRemote*>(this));
        ETCS_LOG("Remote", module << "::" << tag << " RID:" << surface->getRID()
                 << " is now the surface of '" << name << "' (far RID:" << far << ").");
        return true;
    }

    void Unbind()
    {
        if (!surface_) return;
        surface_->setRemoteWire(nullptr);
        surface_ = nullptr;
    }

    void Info() const
    {
        ETCS_LOG("Remote", "RID:" << getRID() << (surface_ ? " bound to '" + name_ + "' far RID:"
                 + std::to_string(far_rid_) + " via Peer RID:" + std::to_string(peer_rid_) : " unbound"));
    }

    // --- IWireRemote --------------------------------------------------------
    bool RemoteWorkConcrete(const ETCS::Buffer& action, ETCS::Buffer& data, const ETCS::SignalContext&) override
    {
        auto edge = link();
        if (!edge || !surface_) { data.reset(); return false; }
        const std::string conj = surface_->getSourceTag().toString() + "." + action.toString();
        std::string why;
        if (edge->work(far_rid_, surface_->actionHash(ETCS::Buffer(conj.c_str())), action.toString(),
                       data, surface_, why)) return true;
        ETCS_LOG("Remote", name_ << "." << action.toString() << " refused: " << why);
        return false;
    }
    int RemoteStreamConcrete(const ETCS::Buffer& action, const ETCS::Buffer& config, bool produces,
                             const ETCS::SignalContext&) override
    {
        auto edge = link();
        if (!edge || !surface_) return -1;
        const std::string conj = surface_->getSourceTag().toString() + "." + action.toString();
        std::string why;
        const int fd = edge->openStream(name_, action.toString(), config.toString(), produces,
                                        surface_->actionHash(ETCS::Buffer(conj.c_str())),
                                        ETCS::MirrorBuffer::manifestOf(surface_, ETCS::WireScope::Socket), why);
        if (fd < 0) ETCS_LOG("Remote", name_ << "." << action.toString() << " stream refused: " << why);
        return fd;
    }

    bool DeleteConcrete() override
    {
        Unbind();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    static bool isPeer(ETCS::Entity* e)
    {
        return e && e->getSourceModule().toString() == "NetworkProvider"
                 && e->getSourceTag().toString() == "Peer";
    }
    std::shared_ptr<etcs_link::Edge> link() const
    {
        Peer* peer = resolvePeer(peer_rid_);
        return peer ? peer->edge() : nullptr;
    }
    static Peer* resolvePeer(ETCS::RID rid)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        return isPeer(e) ? static_cast<Peer*>(e->getTrueType()) : nullptr;
    }

    std::string    name_;
    ETCS::RID      peer_rid_ = 0;
    ETCS::RID      far_rid_  = 0;
    ETCS::Entity*  surface_  = nullptr;
};

#endif // NETWORKPROVIDER_REMOTE_H__
