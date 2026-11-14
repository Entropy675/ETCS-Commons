#ifndef NETWORKPROVIDER_LOCALPIPEPAIR_H__
#define NETWORKPROVIDER_LOCALPIPEPAIR_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

// ---------------------------------------------------------------------------
// Internal helper — allocates a SharedPage from the module-local arena and
// creates a Pipe pair. Used for in-module producer/consumer chains that drive
// ParseConcrete directly, not through the entity call() interface.
// Lifetime of the page is bounded to the calling function's frame.
// ---------------------------------------------------------------------------
struct LocalPipePair
{
    ETCS::MirrorBuffer  producer;
    ETCS::MirrorBuffer  consumer;
    ETCS::SharedPage*   page = nullptr;

    explicit LocalPipePair(const ETCS::SignalContext& ctx)
    {
        producer.bindContext(ctx);
        consumer.bindContext(ctx);
        page = ETCS::SharedPage::allocate(ETCS::MemoryArena::getInstance(), 0);
        ETCS::MirrorBuffer::makePair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page, 0, 0);
    }

    ~LocalPipePair()
    {
        ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page);
    }

    LocalPipePair(const LocalPipePair&)            = delete;
    LocalPipePair& operator=(const LocalPipePair&) = delete;
};

#endif // NETWORKPROVIDER_LOCALPIPEPAIR_H__
