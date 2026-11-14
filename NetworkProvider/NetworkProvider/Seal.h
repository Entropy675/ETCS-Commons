#ifndef NETWORKPROVIDER_SEAL_H__
#define NETWORKPROVIDER_SEAL_H__
#include "../../../ontology.h"

#include <cstring>
#include <string>

/*
 * Seal -- the smallest authority a Wrapper can be: every frame that leaves
 * carries a mark only a holder of the same key makes, and a frame that
 * arrives without it is refused (ETCS::wire_refuse), which ends the stream
 * or refuses the verb.
 *
 * A child of what it guards. On a host's published node, it is the layer a
 * guest must fulfil to reach it: a guest's surface (ontology/Remote.h) must
 * carry a Seal too to be bound at all, and one keyed the same for anything to
 * pass. Keys are given out of band -- the role a link hands someone is the key
 * they were told.
 *
 * NOT ENCRYPTION. The payload is readable; what the seal says is that it was
 * written by a key holder and not altered. Network scope only: a frame that
 * never leaves the process has nobody to prove anything to.
 */
class Seal : public WrapperBase<Seal>, public DeletableBase<Seal>
{
public:
    WIRE_TYPE_IDENTITY(Seal);

    Seal()  = default;
    ~Seal() = default;

    void Key(const std::string& secret)
    {
        seed_ = XXH3_64bits(secret.data(), secret.size());
    }

    void WrapConcrete(ETCS::MBuffer& io, ETCS::SignalContext) override
    {
        const uint64_t mac = XXH3_64bits_withSeed(io.buf, io.written, seed_);
        if (io.written + sizeof mac > ETCS::MBuffer::bufsize) { ETCS::wire_refuse(io); return; }
        std::memcpy(io.buf + io.written, &mac, sizeof mac);
        io.written += sizeof mac;
    }
    void UnwrapConcrete(ETCS::MBuffer& io, ETCS::SignalContext) override
    {
        if (io.written < sizeof(uint64_t)) { ETCS::wire_refuse(io); return; }
        const size_t n = io.written - sizeof(uint64_t);
        uint64_t mac = 0;
        std::memcpy(&mac, io.buf + n, sizeof mac);
        if (mac != XXH3_64bits_withSeed(io.buf, n, seed_)) { ETCS::wire_refuse(io); return; }
        io.written = n;
        io.buf[n] = '\0';
    }
    void CloseConcrete(ETCS::MBuffer&, ETCS::SignalContext) override {}
    ETCS::WireScope ScopeConcrete() const override { return ETCS::WireScope::Socket; }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    uint64_t seed_ = 0x5EA15EA15EA15EA1ull;   // the unkeyed seal: proves only the type
};

#endif // NETWORKPROVIDER_SEAL_H__
