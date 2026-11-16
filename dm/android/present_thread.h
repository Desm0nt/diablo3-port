// [present-thread] PERF-D07 (DOOM 2016 DM_GFX_PRESENT_THREAD, vulkan_present.cpp pt::loop; port-common/modules/present-thread).
// The hand-off between the translation thread (the producer: one job per present) and the present thread (the worker: fence, acquire,
// blit, queue present). Pure C++ (no Vulkan, no Android), so the host test (port-common/modules/present-thread/test) runs the very same
// code under TSan / ASan.
//
// Tickets, all under mu (numbers start at 1; a ticket = the n-th present handed over):
//   enq  - handed over by the producer;
//   sub  - every job up to here has its blit submitted to the queue (or has finished: a failed job counts as submitted);
//   done - every job up to here has finished;
//   cur  - the job the worker runs now (0 = none).
// The rule that keeps the queue order of presenting inline: the producer waits for sub >= enq before it submits anything else
// (wait_submitted), so the blit of present N always lies between the slot of frame N and the slot of frame N+1 on the one VkQueue.
// No busy waiting: every wait is a condition variable; the worker sleeps while the ring is empty.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>

namespace dmpt {

inline int64_t now_ns() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec; }

template<class P, unsigned D = 4>
struct Handoff {
    static_assert(D >= 2, "a ring of at least two jobs");
    std::mutex mu; std::condition_variable cv;
    P ring[D]; uint64_t enq = 0, sub = 0, done = 0, cur = 0; bool started = false, stop = false;
    static constexpr unsigned T = 1024; struct Stamp { uint64_t tk; int64_t ns; } stamps[T] = {};   // when each present was queued (vkQueuePresentKHR returned)
    std::atomic<int> err{0};   // the first failed job's result; the producer reports it at its next hand-off / submit
    // totals since start: jobs run, worker time in them, producer waits (ring full, blit not yet submitted, drain), worker idle
    std::atomic<uint64_t> jobs{0}, busy_ns{0}, full_ns{0}, fulls{0}, wait_ns{0}, waits{0}, drain_ns{0}, drains{0};

    // Producer. start() runs once, under mu, before the first job is visible (it creates the worker thread). Returns 0, or the result of an
    // earlier failed job (nothing is handed over then). *ticket = this job's ticket.
    template<class Start> int push(const P &p, Start &&start, uint64_t *ticket) {
        std::unique_lock<std::mutex> l(mu);
        if (!started) { started = true; start(); }
        if (enq - done >= D) { const int64_t t0 = now_ns(); cv.wait(l, [this] { return enq - done < D; }); full_ns += uint64_t(now_ns() - t0); ++fulls; }
        if (int e = err.load()) return e;
        ring[(enq + 1) % D] = p; const uint64_t tk = ++enq; if (ticket) *ticket = tk;
        l.unlock(); cv.notify_all(); return 0;
    }
    // Worker: the next job, in order; false only after request_stop() with nothing left (host tests).
    bool take(P &out, uint64_t &ticket) {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [this] { return done < enq || stop; });
        if (done >= enq) return false;
        ticket = done + 1; out = ring[ticket % D]; cur = ticket; return true;
    }
    // Worker, right after the job's queue submission returned: the producer may submit behind it.
    void mark_submitted() {
        { std::lock_guard<std::mutex> l(mu); if (!cur || sub >= cur) return; sub = cur; }
        cv.notify_all();
    }
    // Worker, when vkQueuePresentKHR of the current job returned.
    void stamp(int64_t ns) { std::lock_guard<std::mutex> l(mu); if (cur) stamps[cur % T] = Stamp{cur, ns}; }
    // Worker: the job is over (r = its result, 0 = presented).
    void finish(uint64_t ticket, int r, uint64_t spent_ns) {
        busy_ns += spent_ns; ++jobs;
        if (r) { int z = 0; err.compare_exchange_strong(z, r); }
        { std::lock_guard<std::mutex> l(mu); if (sub < ticket) sub = ticket; done = ticket; cur = 0; }
        cv.notify_all();
    }
    // Producer: wait until every handed job has its blit submitted / ticket tk has / every job has finished. Return the nanoseconds waited.
    uint64_t wait_submitted() {
        std::unique_lock<std::mutex> l(mu); if (sub >= enq) return 0;
        const int64_t t0 = now_ns(); cv.wait(l, [this] { return sub >= enq; }); const uint64_t w = uint64_t(now_ns() - t0); wait_ns += w; ++waits; return w;
    }
    uint64_t wait_ticket(uint64_t tk) {   // any thread (the game's at AcquireTexture): the present with this ticket is on the queue
        std::unique_lock<std::mutex> l(mu); if (sub >= tk || tk > enq) return 0;
        const int64_t t0 = now_ns(); cv.wait(l, [this, tk] { return sub >= tk; }); return uint64_t(now_ns() - t0);
    }
    uint64_t drain() {
        std::unique_lock<std::mutex> l(mu); if (done >= enq) return 0;
        const int64_t t0 = now_ns(); cv.wait(l, [this] { return done >= enq; }); const uint64_t w = uint64_t(now_ns() - t0); drain_ns += w; ++drains; return w;
    }
    uint64_t handed() { std::lock_guard<std::mutex> l(mu); return enq; }
    int64_t presented_ns(uint64_t tk) { std::lock_guard<std::mutex> l(mu); const Stamp &s = stamps[tk % T]; return tk && s.tk == tk ? s.ns : 0; }
    void request_stop() { { std::lock_guard<std::mutex> l(mu); stop = true; } cv.notify_all(); }   // host tests only (Android: the thread lives with the process)
};

// The present thread's cores. avoid = the translation thread's cores (its pin or its current mask), all = every core the kernel knows,
// khz[c] = cpuinfo_max_freq of core c (0 = unknown). Every core but the translator's; when that leaves more than one frequency class, the
// slowest class is dropped too (a thread that sleeps between frames lands on a cold little core: NB-20b-ctx finding 3). No translator
// mask, or one that covers every core (the translator is not pinned): all.
inline unsigned place_mask(unsigned all, unsigned avoid, const unsigned *khz, int ncpu) {
    if (!avoid || (all & ~avoid) == 0) return all;
    unsigned m = all & ~avoid, lo = ~0u, hi = 0;
    for (int c = 0; c < ncpu && c < 32; c++) if (m >> c & 1 && khz && khz[c]) { if (khz[c] < lo) lo = khz[c]; if (khz[c] > hi) hi = khz[c]; }
    if (hi && lo < hi) { unsigned keep = 0; for (int c = 0; c < ncpu && c < 32; c++) if (m >> c & 1 && !(khz[c] && khz[c] == lo)) keep |= 1u << c; if (keep) m = keep; }
    return m;
}

}   // namespace dmpt
