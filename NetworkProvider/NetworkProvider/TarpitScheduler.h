#ifndef TARPITSCHEDULER_H__
#define TARPITSCHEDULER_H__
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// TarpitScheduler — a tiny background clock for "run this closure once real
// time has passed", and nothing else. Deliberately NOT built on the
// private core's IOSubmission/ThreadPool: every other deferred callback in
// this module (do_recv/do_send in NetworkProvider.h, TLSIO's own
// completions) is driven by a genuine I/O event eventually firing on its
// own, and this is the one case where nothing is EVER going to complete by
// itself -- the only "event" is a clock reaching a value. That is also
// exactly the job ConnectionManager's own maintenance thread already does
// for a different pair of conditions (reaping idle connections, re-arming
// a shrunk accept window -- see ConnectionManager.h's startMaintenance and
// its own comment on why a watchdog needs a clock of its own rather than
// borrowing one that exists for something else). Same idiom, applied to a
// new condition.
//
// WHY NOT JUST SLEEP ON A THREADPOOL WORKER: every callback elsewhere in
// this module runs on an io_completion or pool worker thread, and every one
// of them is documented as staying non-blocking for exactly that reason --
// a blocking sleep there would tie up a worker slot that real connections'
// own recv/send completions need. This gets its own thread precisely so a
// tarpit's delay never costs the pool anything.
//
// CONCURRENCY CAP: ScheduleDelayed refuses once kMaxPending jobs are
// already waiting, returning false rather than queuing indefinitely. This
// is the one genuinely finite resource a tarpit consumes across every
// connection it holds open -- a caller's own connection-pool slot -- and
// the cap exists so a flood of scan traffic cannot turn the tarpit itself
// into the thing that exhausts that pool for real requests. Shared across
// every HttpServer in the process on purpose: this is one Meyers singleton,
// so multiple servers/ports in the same process share one budget rather
// than each getting kMaxPending and multiplying the exposure.
//
// TEARDOWN: a scheduled job can still be pending when the process starts
// shutting down (its whole point is to run LATE). The closures this module
// hands in guard themselves with an isTearingDown() check of their own
// before touching any entity (see NetworkProvider.h's Serve, the tarpit
// branch) -- this class has no visibility into that on its own, since it
// knows nothing about entities or arenas by design. That guard is
// best-effort, the same category of unavoidable edge case
// ConnectionManager::CloseConcrete already documents for its own
// teardown-time IOSubmission skip; a tarpit delay is deliberately kept
// short (see HttpServer::GetTarpitDelayMs's own default) to keep that
// window small.
class TarpitScheduler
{
public:
    static TarpitScheduler& getInstance()
    {
        static TarpitScheduler instance;
        return instance;
    }

    // Runs `fn` once, no earlier than `delay_ms` from now, on this
    // scheduler's own thread -- never synchronously, and never on the
    // caller's thread. Safe to call from any thread. Returns false (and
    // schedules nothing) if the pending count is already at kMaxPending --
    // the caller's job in that case is to fall back to running its work
    // immediately instead of dropping it.
    bool ScheduleDelayed(int delay_ms, std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_.size() >= kMaxPending) return false;
            const auto fire_at = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(delay_ms);
            jobs_.push_back(Job{fire_at, std::move(fn)});
        }
        ensureRunning();
        return true;
    }

    size_t PendingCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }

    static constexpr size_t kMaxPending = 256;

private:
    struct Job
    {
        std::chrono::steady_clock::time_point fire_at;
        std::function<void()> fn;
    };

    TarpitScheduler() = default;
    ~TarpitScheduler()
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
    TarpitScheduler(const TarpitScheduler&)            = delete;
    TarpitScheduler& operator=(const TarpitScheduler&) = delete;

    void ensureRunning()
    {
        if (running_.exchange(true, std::memory_order_acq_rel)) return;
        thread_ = std::thread([this]
        {
            while (running_.load(std::memory_order_acquire))
            {
                std::vector<std::function<void()>> due;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto now = std::chrono::steady_clock::now();
                    for (auto it = jobs_.begin(); it != jobs_.end(); )
                    {
                        if (it->fire_at <= now)
                        {
                            due.push_back(std::move(it->fn));
                            it = jobs_.erase(it);
                        }
                        else ++it;
                    }
                }
                // Fired OUTSIDE the lock -- a fired job ultimately submits a
                // real Send, and must never run while some other thread's
                // own ScheduleDelayed is blocked waiting on this same mutex.
                for (auto& fn : due) fn();

                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
            }
        });
    }

    // Coarse enough to cost nothing while idle, fine enough that a
    // few-second delay does not visibly round to the next tick.
    static constexpr int kTickMs = 200;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::mutex        mutex_;
    std::vector<Job>  jobs_;
};

#endif // TARPITSCHEDULER_H__
