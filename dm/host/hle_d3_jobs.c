// [D3] The game's job pool on a host queue (issue #18: random crash in a JobWorker, more often after a town portal / at the blacksmith).
// What crashed (update 2.7.7 main, build 2607a74f; three reports, Adreno 840 / SM8850, v0.2.9):
//   CRASH pc=main+0x13839c / +0x10388c / +0x103888, bt main+0x515bd0 main+0x515b50 main+0x9a54 = a JobWorker (0x515b60) at the first
//   reads of a job it had just taken: jobs 0x1382f0 (scene items, queued by 0x133030) and 0x103830 (queued by 0x102cf0) get a 16-byte
//   descriptor {list, first, count} that lives in the caller's stack frame (0x102cf0: local_878, 0x133030: a 0x5000-byte array), and
//   the caller is the game's main thread, which runs on the host stack. The descriptors read were other functions' frames
//   ({1.0f, 1, heap pointer}; {pointer, saved x29}; {3, return address}): the main thread had left 0x102cf0 / 0x133030, and a worker
//   still started one of their jobs.
// The pool (one object, *(main+0x114aa18), 2 workers "JobWorker%d", created by 0x515e90):
//   +0 state (0 idle, 2 filling, 1 kicked, 3 waiting), +4 head, +8 workers, +0xc quit, +0x18 awake workers (LL/SC), +0x1c LightEvent
//   (auto clear), +0x28 Semaphore, +0x50 ring of 1024 {fn, arg}, +0x4050 tail (LL/SC).
//   0x515f70 Add(pool, fn, arg): slot[head] = {fn, arg}, head++; from state 0: state = 2, awake += 2, 2 x ReleaseSemaphore; ring full
//     or no workers: fn(arg) at once.
//   0x516060 Kick: state 2 -> 1.   0x5160d0 Wait: state 0 -> return; state = 3; take and run slots until tail == head; WaitLightEvent;
//     state = 0.   0x515b60 worker: AcquireSemaphore; take and run slots; ring empty: state 2 -> SleepThread(0) and look again, else
//     awake--, at 0 SignalLightEvent, AcquireSemaphore.
//   Callers: only the main loop 0x40810 and the frame 0x3ee30 (0x51a10, 0x102cf0, 0x10e7d0, 0x10f2b0, 0x133030, 0x15e010, 0x2f6270
//   and the waits 0x51fa0, 0x10e890, 0x10f360); nothing else loads the pool pointer.
// What was checked: the protocol itself holds for one thread that adds / kicks / waits and two workers under every interleaving
// (exhaustive model of the steps above with ideal events, some 60 add / kick / wait programs; the SDK semaphore over our SVC
// 0x1a / 0x1b / 0x1c / 0x1d the same way; the SDK and the pool code run unpatched). What it does not survive:
//   - the lock-free ring: a worker that has read {tail, slot} and stops before its store-exclusive takes a stale slot if tail has
//     come round to the same index meanwhile (1024 takes; a batch of 0x133030 can be 1280 jobs). On the Switch only a thread of
//     higher priority stops a worker in that 13-instruction window (no time slices among equals); an Android thread is descheduled
//     anywhere, for milliseconds;
//   - a second thread in Wait: Wait ends with "state = 0", and the next Wait returns at once on state 0 with slots still in the ring;
//   - a Wait from inside a job that the main thread runs from Wait: the inner one takes the event and ends the cycle, the outer one
//     then sleeps on an event nobody signals.
//   The frame 0x3ee30 calls Wait (0x10e890 at 0x3ff74) on every path, also as the loading-screen pump 0x3d4d0, which blocking loads
//   on "kind 0" threads (the main thread; the UISpriteThread 0x333cd0 sets kind 0 too) run when they take over 16 ms within about
//   2 s of a load (flags main+0x1767ba4). A pump under a job on the main thread is the third case; one on the UISpriteThread would
//   be the second (with the UI kicked it blocks on its own "done" event at 0x3fdc0 first). No call path into either was found
//   through direct calls; the jobs' virtual calls were not followed.
// Which of these it was could not be read off the three reports (registers of the crashed worker only), so the fix does not depend
// on it:
//   DM_D3_JOBS=1 (default; 0 = the game's own pool) Add / Wait / the worker loop run on a host queue under one mutex.
//     Add: queue {fn, arg} (no workers: run it; queue full: run it), wake an idle worker and the threads in Wait.
//     Wait: take and run jobs; return when the queue is empty and no job is running, whatever thread calls it and whatever the
//       pool's state word says. A Wait from inside a job (the pump) cannot wait for the job it is in: it returns when nothing else
//       runs, and the Wait around that job goes on until the job itself is done.
//     Worker (the two guest threads, their own stacks, thread setup and exit as before): take and run jobs, sleep on the queue.
//   So a job cannot start after the Wait that follows its Add has returned. Kick stays the game's (it only writes the state word).
//   Log: one line when installed, totals at the first job and every 10 minutes, the first Waits from inside a job, and (red) the
//   first Adds / Waits from a thread other than the one that added the first job - the two cases the game's own pool does not survive.
//   The pool's ring, counters, event and semaphore are not used any more; quit (+0xc, 0x515dc0) is polled every 100 ms.
// Every patched site is checked word for word first; a different build keeps the game's pool.
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    O_WORKER = 0x515b94,       // worker body: ..., mov x0, x20; bl AcquireSemaphore (0x515ba0); ldr w8, [x19, #0xc]; cbnz w8, exit; loop
    O_WORKER_LOOP = 0x515ba0,  // patched: ldr x16, lit; mov x0, x19; blr x16; b O_WORKER_EXIT; lit
    O_WORKER_EXIT = 0x515c80,  // the worker's own exit path (thread teardown)
    O_ADD = 0x515f70, O_KICK = 0x516060, O_WAIT = 0x5160d0,
    P_WORKERS = 8, P_QUIT = 0xc, CAP = 4096,
};
static const struct { uint32_t off, n, w[10]; } k_sites[] = {
    {O_WORKER, 10, {0x321f07e8, 0x9100a274, 0xb900a008, 0xaa1403e0, 0x97ebc35b, 0xb9400e68, 0x350006a8, 0x52880a08, 0x8b080276, 0x91006277}},
    {O_WORKER_EXIT, 4, {0x9414cfac, 0x94054f03, 0xa9437bfd, 0xa9424ff4}},
    {O_ADD, 6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xb9800408, 0x11000509}},
    {O_KICK, 6, {0xb9400008, 0x7100091f, 0x54000061, 0x320003e8, 0xb9000008, 0xd65f03c0}},
    {O_WAIT, 6, {0xb9400008, 0x34000768, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9400008}},
};

typedef void (*job_fn)(void *);
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER, g_done = PTHREAD_COND_INITIALIZER;
static struct { job_fn fn; void *arg; } g_q[CAP];
static unsigned g_head, g_tail;     // under g_mu: jobs added / taken so far (slot = count % CAP)
static int g_running;               // under g_mu: jobs being run (by workers, by threads in Wait, by Add when the queue is full)
static int g_parked;                // under g_mu: of g_running, the jobs whose thread sleeps in a Wait it called from inside them
                                    // (two such Waits must not wait for each other's job)
static int g_idle, g_waiting;       // under g_mu: workers asleep on g_work, threads asleep on g_done
static uint64_t g_added, g_by_worker, g_direct, g_nested, g_peak;   // under g_mu: totals for the log
static __thread int t_depth;        // jobs this thread is inside of
static int g_user, g_others;        // under g_mu: the thread that added the first job (the game's main thread); calls from other threads logged

// g_mu held. The game adds and waits on its main thread only (see the header); a call from another thread, which its own pool does
// not survive, is worth a line
static void note_caller(const char *what) {
    static __thread int tid;
    if (!tid) tid = (int)gettid();
    if (!g_user) g_user = tid;
    else if (tid != g_user && g_others++ < 8)
        LOG("【红】[d3jobs] %s from another thread (tid %d, depth %d; the first user is tid %d): %u queued, %d running", what, tid, t_depth, g_user, g_head - g_tail, g_running);
}

// g_mu held, the queue is not empty: take the oldest job and run it; g_mu held again on return
static void run_one(void) {
    job_fn fn = g_q[g_tail % CAP].fn; void *arg = g_q[g_tail % CAP].arg;
    g_tail++; g_running++;
    pthread_mutex_unlock(&g_mu);
    t_depth++; fn(arg); t_depth--;
    pthread_mutex_lock(&g_mu);
    g_running--;
    if (g_waiting && g_head == g_tail) pthread_cond_broadcast(&g_done);   // each sleeper checks its own condition
}
static void jobs_add(void *pool, job_fn fn, void *arg) {
    if (*(volatile int32_t *)((uint8_t *)pool + P_WORKERS) <= 0) { fn(arg); return; }   // as 0x515f70: no workers = here and now
    pthread_mutex_lock(&g_mu);
    if (!t_depth) note_caller("Add");
    g_added++;
    if (g_head - g_tail >= CAP) {   // as 0x515f70 with its ring full: the caller runs it (counted as running: a Wait elsewhere waits for it)
        g_direct++; g_running++;
        pthread_mutex_unlock(&g_mu);
        t_depth++; fn(arg); t_depth--;
        pthread_mutex_lock(&g_mu);
        g_running--;
        if (g_waiting && g_head == g_tail) pthread_cond_broadcast(&g_done);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_q[g_head % CAP].fn = fn; g_q[g_head % CAP].arg = arg; g_head++;
    if (g_head - g_tail > g_peak) g_peak = g_head - g_tail;
    if (g_idle) pthread_cond_signal(&g_work);
    if (g_waiting) pthread_cond_broadcast(&g_done);   // a thread in Wait takes jobs too
    pthread_mutex_unlock(&g_mu);
}
static void jobs_wait(void *pool) {
    (void)pool;
    pthread_mutex_lock(&g_mu);
    if (!t_depth && g_user) note_caller("Wait");
    if (t_depth && g_nested++ < 4)
        LOG("[d3jobs] Wait from inside a job (depth %d, %u queued, %d running): the game's pool would end the outer Wait's cycle here", t_depth, g_head - g_tail, g_running);
    for (;;) {
        if (g_head != g_tail) { run_one(); continue; }
        // nothing queued and nothing running; from inside a job: nothing but the jobs this call is nested in and the jobs of other
        // threads that sleep here from inside theirs
        if (g_running == (t_depth ? g_parked + t_depth : 0)) break;
        g_parked += t_depth; g_waiting++;
        dm_thread_native_wait(1); pthread_cond_wait(&g_done, &g_mu); dm_thread_native_wait(0);
        g_waiting--; g_parked -= t_depth;
    }
    static uint64_t next;
    uint64_t now = now_ns(), added = g_added, byw = g_by_worker, direct = g_direct, nested = g_nested, peak = g_peak;
    int say = !t_depth && now >= next && added;
    if (say) next = now + 600000000000ull;
    pthread_mutex_unlock(&g_mu);
    if (say) LOG("[d3jobs] %.0f s: %llu jobs (%llu on the workers, %llu run by Add on a full queue), longest queue %llu, Waits from inside a job %llu",
                 (now - g_t0_ns) / 1e9, (unsigned long long)added, (unsigned long long)byw, (unsigned long long)direct, (unsigned long long)peak, (unsigned long long)nested);
}
static void jobs_worker(void *pool) {
    pthread_mutex_lock(&g_mu);
    for (;;) {
        if (g_head != g_tail) { g_by_worker++; run_one(); continue; }
        if (*(volatile int32_t *)((uint8_t *)pool + P_QUIT)) break;   // 0x515dc0 (pool shutdown) sets it and joins the workers
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        g_idle++;
        dm_thread_native_wait(1); pthread_cond_timedwait(&g_work, &g_mu, &ts); dm_thread_native_wait(0);
        g_idle--;
    }
    pthread_mutex_unlock(&g_mu);
}

void dm_d3_jobs_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_d3_jobs_install(void) {
    const char *e = getenv("DM_D3_JOBS");
    if (e && *e == '0' && !e[1]) { LOG("[d3jobs] DM_D3_JOBS=0: the game's own job pool"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) { LOG("【红】[d3jobs] no main module: the game's own job pool"); return; }
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (m->text_end < k_sites[i].off + 4 * k_sites[i].n || memcmp(m->base + k_sites[i].off, k_sites[i].w, 4 * k_sites[i].n)) {
            LOG("【红】[d3jobs] main+%#x differs: not D3 update 2.7.7 (build 2607a74f), the game's own job pool", k_sites[i].off); return; }
    // entry -> host function: ldr x16, #8; br x16; .quad fn (the entries are 8-byte aligned). The worker keeps its prologue, thread
    // setup and exit path: its loop becomes one call (x19 = the pool) followed by a branch to the exit path
    uint64_t add = (uint64_t)(uintptr_t)jobs_add, wait = (uint64_t)(uintptr_t)jobs_wait, worker = (uint64_t)(uintptr_t)jobs_worker;
    const uint32_t w_add[4] = {0x58000050, 0xd61f0200, (uint32_t)add, (uint32_t)(add >> 32)};
    const uint32_t w_wait[4] = {0x58000050, 0xd61f0200, (uint32_t)wait, (uint32_t)(wait >> 32)};
    const uint32_t w_worker[6] = {0x58000090 /* ldr x16, #16 */, 0xaa1303e0 /* mov x0, x19 */, 0xd63f0200 /* blr x16 */,
                                  0x14000000u | ((O_WORKER_EXIT - (O_WORKER_LOOP + 12)) >> 2) /* b exit */, (uint32_t)worker, (uint32_t)(worker >> 32)};
    // all three or none (the game's Add with our worker would queue jobs nobody takes): one writable window over the three sites
    uint8_t *lo = (uint8_t *)((uintptr_t)(m->base + O_WORKER_LOOP) & ~(uintptr_t)0x3FFF), *end = m->base + O_WAIT + sizeof w_wait;
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)lo + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(lo, len, PROT_READ | PROT_WRITE)) { LOG("【红】[d3jobs] mprotect RW failed: the game's own job pool"); return; }
    memcpy(m->base + O_WORKER_LOOP, w_worker, sizeof w_worker);
    memcpy(m->base + O_ADD, w_add, sizeof w_add);
    memcpy(m->base + O_WAIT, w_wait, sizeof w_wait);
    if (mprotect(lo, len, PROT_READ | PROT_EXEC)) die("[d3jobs] mprotect RX");
    __builtin___clear_cache((char *)lo, (char *)lo + len);
    LOG("[d3jobs] job pool on a host queue: Add main+%#x, Wait main+%#x, worker loop main+%#x (queue %d; DM_D3_JOBS=0 = the game's own pool)",
        O_ADD, O_WAIT, O_WORKER_LOOP, CAP);
}
#endif
