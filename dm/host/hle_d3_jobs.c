// [D3] The game's job pool on the host (issue #18: random crash in a JobWorker, more often after a town portal / at the blacksmith).
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
//   DM_D3_JOBS=1 (default) Add / Wait / the worker loop run on a host pool with no lock on the job path:
//     - a bounded multi-producer / multi-consumer ring (4096 cells, a sequence number per cell: one compare-and-swap to add, one to
//       take, no ABA: a cell is taken only at the sequence its producer published);
//     - g_out = jobs added and not finished yet (+1 before a job goes in, -1 after it has returned);
//     - Add: g_out++, put {fn, arg} in the ring (ring full or no workers: run it here), wake one sleeping worker if there is one
//       (a flag per worker: two loads when both are awake, no system call);
//     - Wait: take and run jobs; return when g_out is 0, whatever thread calls it and whatever the pool's state word says. From
//       inside a job (the pump) it cannot wait for the jobs it is nested in: it returns when nothing else is outstanding, and the
//       Wait around that job goes on until the job itself is done. With the ring empty and jobs still running elsewhere it polls
//       for DM_D3_JOBS_SPIN_US (30) and then sleeps on a futex that the finishing job wakes (only when somebody sleeps there);
//     - worker (the two guest threads, their own stacks, thread setup and exit as before): take and run jobs; ring empty: sleep on
//       its futex (DM_D3_JOBS_WORKER_SPIN_US, default 0, polls first), woken by Add; quit (+0xc, 0x515dc0) is looked at every 100 ms.
//   So a job cannot start after the Wait that follows its Add has returned. Kick stays the game's (it only writes the state word);
//   the pool's ring, counters, event and semaphore are not used.
//   DM_D3_JOBS=0 the game's own pool. With DM_D3_JOBS_STATS=1 (default) its Add and Wait entries pass through a probe (the job is
//     wrapped to time its start; the pool's code runs as it is, from a copy of the four words the probe's jump replaced), so the two
//     pools print the same line; DM_D3_JOBS_STATS=0 leaves the game's code untouched (and stops the timing in the host pool).
//   Log: one line when installed; once a minute "[d3jobs] ... pool host|game: frames, jobs per frame (share run by the workers),
//   push -> start average and p99, the main thread's time in Wait per frame, contention (ring retries, sleeps, wakes, jobs run by Add
//   on a full ring)"; the first Waits from inside a job and (red) the first Adds / Waits from a thread other than the one that
//   added the first job - the two cases the game's own pool does not survive. A frame = a call of Wait from the frame 0x3ee30.
// Every patched site is checked word for word first; a different build keeps the game's pool untouched.
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

enum {
    O_WORKER = 0x515b94,       // worker body: ..., mov x0, x20; bl AcquireSemaphore (0x515ba0); ldr w8, [x19, #0xc]; cbnz w8, exit; loop
    O_WORKER_LOOP = 0x515ba0,  // patched: ldr x16, lit; mov x0, x19; blr x16; b O_WORKER_EXIT; lit
    O_WORKER_EXIT = 0x515c80,  // the worker's own exit path (thread teardown)
    O_ADD = 0x515f70, O_KICK = 0x516060, O_WAIT = 0x5160d0,
    O_FRAME_RET = 0x3ff78,     // the return address of the frame's Wait (0x3ee30: bl 0x10e890, which tail-calls Wait)
    P_STATE = 0, P_WORKERS = 8, P_QUIT = 0xc, CAP = 4096, MAX_WORKERS = 8, MAX_ST = 16,
};
static const struct { uint32_t off, n, w[10]; } k_sites[] = {
    {O_WORKER, 10, {0x321f07e8, 0x9100a274, 0xb900a008, 0xaa1403e0, 0x97ebc35b, 0xb9400e68, 0x350006a8, 0x52880a08, 0x8b080276, 0x91006277}},
    {O_WORKER_EXIT, 4, {0x9414cfac, 0x94054f03, 0xa9437bfd, 0xa9424ff4}},
    {O_ADD, 6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xb9800408, 0x11000509}},
    {O_KICK, 6, {0xb9400008, 0x7100091f, 0x54000061, 0x320003e8, 0xb9000008, 0xd65f03c0}},
    {O_WAIT, 6, {0xb9400008, 0x34000768, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9400008}},
};
typedef void (*job_fn)(void *);

// ---- time and sleeping
static double g_tick_ns = 1.0;   // nanoseconds per tick()
static inline uint64_t tick(void) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;   // a few cycles, no call
#else
    return now_ns();
#endif
}
static inline void relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
// Sleep while *w == v (at most ms), wake n sleepers of w. Linux: a futex; elsewhere one mutex and condition (only sleepers take it)
#ifdef __linux__
static void park(_Atomic uint32_t *w, uint32_t v, int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    dm_thread_native_wait(1); syscall(SYS_futex, (uint32_t *)w, FUTEX_WAIT_PRIVATE, v, &ts, NULL, 0); dm_thread_native_wait(0);
}
static void unpark(_Atomic uint32_t *w, int n) { syscall(SYS_futex, (uint32_t *)w, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0); }
#else
static pthread_mutex_t g_pk_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pk_cv = PTHREAD_COND_INITIALIZER;
static void park(_Atomic uint32_t *w, uint32_t v, int ms) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)ms * 1000000; ts.tv_sec += ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
    pthread_mutex_lock(&g_pk_mu);
    if (atomic_load(w) == v) { dm_thread_native_wait(1); pthread_cond_timedwait(&g_pk_cv, &g_pk_mu, &ts); dm_thread_native_wait(0); }
    pthread_mutex_unlock(&g_pk_mu);
}
static void unpark(_Atomic uint32_t *w, int n) { (void)w; (void)n; pthread_mutex_lock(&g_pk_mu); pthread_cond_broadcast(&g_pk_cv); pthread_mutex_unlock(&g_pk_mu); }
#endif

// ---- measurements: a block per thread, written by its thread only, summed once a minute by the thread that reports
typedef struct {
    uint64_t run, lat_sum, hist[64];          // jobs this thread ran; push -> start: sum of ticks, counts by log2(ticks)
    uint64_t added, direct;                   // jobs added; of them run by Add itself (ring full)
    uint64_t waits, wait_ticks, frames;       // depth-0 Waits and the time in them; Waits from the frame 0x3ee30
    uint64_t retries, sleeps, wakes;          // lost ring compare-and-swaps; futex sleeps; futex wakes sent
    int worker;
} __attribute__((aligned(64))) st_t;
#define ST_WORDS (offsetof(st_t, worker) / 8)
static st_t g_st[MAX_ST]; static _Atomic int g_nst;
static __thread st_t *t_st;
static int g_stats = 1, g_host_pool;
static uint8_t *g_main;
static st_t *st(void) {
    if (!t_st) { int i = atomic_fetch_add(&g_nst, 1); t_st = &g_st[i < MAX_ST ? i : MAX_ST - 1]; }
    return t_st;
}
static inline void note_start(st_t *s, uint64_t pushed) {
    uint64_t d = tick() - pushed; s->lat_sum += d; s->hist[63 - __builtin_clzll(d | 1)]++;
}
static __thread int t_depth;           // jobs this thread is inside of
static _Atomic int g_user;             // the thread that added the first job (the game's main thread)
static _Atomic int g_others, g_nested;
static int my_tid(void) { static __thread int tid; if (!tid) tid = (int)gettid(); return tid; }
// The game adds and waits on its main thread only (see the header); a call from another thread, which its own pool does not
// survive, is worth a line
static _Atomic long g_out __attribute__((aligned(64)));   // jobs added and not finished (the host pool)
static void note_caller(const char *what) {
    int tid = my_tid(), u = atomic_load_explicit(&g_user, memory_order_relaxed);
    if (!u) { int z = 0; atomic_compare_exchange_strong(&g_user, &z, tid); return; }
    if (tid != u && atomic_fetch_add(&g_others, 1) < 8)
        LOG("【红】[d3jobs] %s from another thread (tid %d, depth %d; the first user is tid %d): %ld outstanding", what, tid, t_depth, u, atomic_load(&g_out));
}
// Once a minute, from a depth-0 Wait of the first user: what the last minute looked like (differences of the summed blocks)
static void report(uint64_t now) {
    static uint64_t next, last_t, last_by_workers; static st_t last;
    if (!next) { next = now + (uint64_t)(60e9 / g_tick_ns); last_t = now; return; }
    if (now < next) return;
    st_t sum, d; memset(&sum, 0, sizeof sum); uint64_t by_workers = 0;
    int n = atomic_load(&g_nst); if (n > MAX_ST) n = MAX_ST;
    for (int i = 0; i < n; i++) { const uint64_t *a = (const uint64_t *)&g_st[i]; uint64_t *b = (uint64_t *)&sum;
        for (unsigned k = 0; k < ST_WORDS; k++) b[k] += a[k];
        if (g_st[i].worker) by_workers += g_st[i].run; }
    { const uint64_t *a = (const uint64_t *)&sum, *l = (const uint64_t *)&last; uint64_t *b = (uint64_t *)&d;
      for (unsigned k = 0; k < ST_WORDS; k++) b[k] = a[k] - l[k]; }
    uint64_t dw = by_workers - last_by_workers, c = 0, p99 = 0; double secs = (now - last_t) * g_tick_ns / 1e9, fr = d.frames ? (double)d.frames : 1.0;
    for (int k = 0; k < 64 && d.run; k++) { c += d.hist[k]; if (c * 100 >= d.run * 99) { p99 = k < 63 ? 2ull << k : ~0ull; break; } }
    LOG("[d3jobs] %.0f s, pool %s: %llu frames, %.1f jobs/frame (%.0f%% on the workers), push->start avg %.1f us p99 <= %.1f us, main in Wait %.3f ms/frame "
        "(%.1f waits/frame), contention: ring retries %llu, sleeps %llu, wakes %llu, run by Add on a full ring %llu; Waits from inside a job %d, calls from other threads %d",
        secs, g_host_pool ? "host" : "game", (unsigned long long)d.frames, d.added / fr, d.run ? 100.0 * dw / d.run : 0.0,
        d.run ? d.lat_sum * g_tick_ns / 1e3 / d.run : 0.0, p99 * g_tick_ns / 1e3, d.wait_ticks * g_tick_ns / 1e6 / fr, d.waits / fr,
        (unsigned long long)d.retries, (unsigned long long)d.sleeps, (unsigned long long)d.wakes, (unsigned long long)d.direct, atomic_load(&g_nested), atomic_load(&g_others));
    last = sum; last_by_workers = by_workers; last_t = now; next = now + (uint64_t)(60e9 / g_tick_ns);
}
// The end of a depth-0 Wait: its time, the frame count, the report
static inline void wait_over(st_t *s, uint64_t t0, const void *ret) {
    uint64_t now = tick(); s->waits++; s->wait_ticks += now - t0;
    if (ret == (const void *)(g_main + O_FRAME_RET)) s->frames++;
    if (my_tid() == atomic_load_explicit(&g_user, memory_order_relaxed)) report(now);
}

// ---- the host pool
// The ring (bounded MPMC, a sequence per cell: seq == index -> free for the producer at that index, seq == index + 1 -> holds its job)
typedef struct { _Atomic uint64_t seq; job_fn fn; void *arg; uint64_t pushed; } cell_t;
typedef struct { job_fn fn; void *arg; uint64_t pushed; } job_t;
static cell_t g_cell[CAP];
static _Atomic uint64_t g_put __attribute__((aligned(64))), g_take __attribute__((aligned(64)));
static _Atomic long g_parked;                             // of g_out, the jobs whose thread sleeps in a Wait it called from inside them
static _Atomic int g_waiters;                             // threads asleep (or about to be) in Wait
static _Atomic uint32_t g_done_seq;                       // their futex: bumped when a job finishes or arrives while g_waiters != 0
static struct { _Atomic uint32_t asleep; char pad[60]; } g_wk[MAX_WORKERS];   // a worker's futex: 1 while it sleeps (Add clears it and wakes)
static _Atomic int g_nwk;
static uint64_t g_spin_wait, g_spin_worker;               // ticks a Wait / a worker polls an empty ring before it sleeps
static void ring_init(void) { for (uint64_t i = 0; i < CAP; i++) atomic_store(&g_cell[i].seq, i); }
static int ring_put(job_fn fn, void *arg, st_t *s) {
    uint64_t pos = atomic_load_explicit(&g_put, memory_order_relaxed);
    for (;;) {
        cell_t *c = &g_cell[pos % CAP]; uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        if (seq == pos) {
            if (atomic_compare_exchange_weak_explicit(&g_put, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                c->fn = fn; c->arg = arg; c->pushed = g_stats ? tick() : 0;
                atomic_store(&c->seq, pos + 1); return 1; }   // sequentially consistent: the looks at the sleepers come after it
            s->retries++;
        } else if (seq < pos) return 0;   // the cell a lap back is not taken yet: full
        else pos = atomic_load_explicit(&g_put, memory_order_relaxed);
    }
}
static int ring_take(job_t *j, st_t *s) {
    uint64_t pos = atomic_load_explicit(&g_take, memory_order_relaxed);
    for (;;) {
        cell_t *c = &g_cell[pos % CAP]; uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        if (seq == pos + 1) {
            if (atomic_compare_exchange_weak_explicit(&g_take, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
                j->fn = c->fn; j->arg = c->arg; j->pushed = c->pushed;
                atomic_store_explicit(&c->seq, pos + CAP, memory_order_release); return 1; }
            s->retries++;
        } else if (seq < pos + 1) return 0;   // nothing published at the head
        else pos = atomic_load_explicit(&g_take, memory_order_relaxed);
    }
}
// The sleepers in Wait look again (a load when nobody sleeps there)
static inline void wake_waiters(st_t *s) {
    if (atomic_load(&g_waiters)) { atomic_fetch_add(&g_done_seq, 1); unpark(&g_done_seq, 64); s->wakes++; }
}
static inline void run_job(const job_t *j, st_t *s) {
    if (g_stats) note_start(s, j->pushed);
    s->run++;
    t_depth++; j->fn(j->arg); t_depth--;
    atomic_fetch_sub(&g_out, 1);   // the job is over: one less outstanding
    wake_waiters(s);
}
static void jobs_add(void *pool, job_fn fn, void *arg) {
    if (*(volatile int32_t *)((uint8_t *)pool + P_WORKERS) <= 0) { fn(arg); return; }   // as 0x515f70: no workers = here and now
    st_t *s = st();
    if (!t_depth) note_caller("Add");
    s->added++;
    atomic_fetch_add(&g_out, 1);   // before it can be taken: g_out == 0 means nothing queued and nothing running
    if (!ring_put(fn, arg, s)) {   // as 0x515f70 with its ring full: the caller runs it
        job_t j = {fn, arg, g_stats ? tick() : 0}; s->direct++; run_job(&j, s); return; }
    for (int i = 0, n = atomic_load_explicit(&g_nwk, memory_order_relaxed); i < n && i < MAX_WORKERS; i++)   // one sleeping worker, if any
        if (atomic_load(&g_wk[i].asleep)) { uint32_t one = 1;
            if (atomic_compare_exchange_strong(&g_wk[i].asleep, &one, 0)) { unpark(&g_wk[i].asleep, 1); s->wakes++; break; } }
    wake_waiters(s);   // a thread asleep in Wait takes jobs too
}
// Nothing outstanding; from inside a job: nothing but the jobs this Wait is nested in (depth) and the jobs of other threads that
// sleep in a Wait from inside theirs (g_parked; mine = this thread's own are counted in it already)
static inline int quiet(int depth, int mine) {
    return atomic_load(&g_out) == (depth ? atomic_load(&g_parked) + (mine ? 0 : depth) : 0);
}
static void jobs_wait(void *pool) {
    (void)pool;
    st_t *s = st(); int depth = t_depth; job_t j;
    uint64_t t0 = depth ? 0 : tick();
    if (!depth) { if (atomic_load_explicit(&g_user, memory_order_relaxed)) note_caller("Wait"); }
    else if (atomic_fetch_add(&g_nested, 1) < 4)
        LOG("[d3jobs] Wait from inside a job (depth %d, %ld outstanding): the game's pool would end the outer Wait's cycle here", depth, atomic_load(&g_out));
    for (;;) {
        if (ring_take(&j, s)) { run_job(&j, s); continue; }
        if (quiet(depth, 0)) break;
        if (g_spin_wait) {   // the last jobs are on the workers and short: a sleep costs more than they take
            int got = 0, over = 0;
            for (uint64_t end = tick() + g_spin_wait; tick() < end; relax()) if ((got = ring_take(&j, s)) || (over = quiet(depth, 0))) break;
            if (got) { run_job(&j, s); continue; }
            if (over) break;
        }
        if (depth) { atomic_fetch_add(&g_parked, depth); wake_waiters(s); }   // another Wait from inside a job may be waiting for these only
        atomic_fetch_add(&g_waiters, 1);
        uint32_t seq = atomic_load(&g_done_seq);
        int got = ring_take(&j, s);
        if (!got && !quiet(depth, 1)) { s->sleeps++; park(&g_done_seq, seq, 50); }
        atomic_fetch_sub(&g_waiters, 1);
        if (depth) atomic_fetch_sub(&g_parked, depth);
        if (got) run_job(&j, s);
    }
    if (!depth) wait_over(s, t0, __builtin_return_address(0));
}
static void jobs_worker(void *pool) {
    st_t *s = st(); s->worker = 1; job_t j;
    int me = atomic_fetch_add(&g_nwk, 1); if (me >= MAX_WORKERS) die("[d3jobs] more than %d workers", MAX_WORKERS);
    for (;;) {
        if (ring_take(&j, s)) { run_job(&j, s); continue; }
        if (*(volatile int32_t *)((uint8_t *)pool + P_QUIT)) break;   // 0x515dc0 (pool shutdown) sets it and joins the workers
        if (g_spin_worker) { int got = 0;
            for (uint64_t end = tick() + g_spin_worker; tick() < end; relax()) if ((got = ring_take(&j, s))) break;
            if (got) { run_job(&j, s); continue; } }
        atomic_store(&g_wk[me].asleep, 1);
        if (ring_take(&j, s)) { atomic_store(&g_wk[me].asleep, 0); run_job(&j, s); continue; }   // added between the look and the flag
        s->sleeps++; park(&g_wk[me].asleep, 1, 100);
        atomic_store(&g_wk[me].asleep, 0);
    }
}

// ---- probes around the game's own pool (DM_D3_JOBS=0): the same measurements, the pool's code as it is
typedef struct { job_fn fn; void *arg; uint64_t pushed; } rec_t;
static rec_t g_rec[CAP]; static _Atomic uint32_t g_nrec;   // the game's ring holds 1023 jobs: a record is not reused while its job waits
static void (*g_game_add)(void *, job_fn, void *); static void (*g_game_wait)(void *);   // the replaced words, then a jump back
static void probe_job(void *p) {
    rec_t r = *(rec_t *)p; st_t *s = st();
    if (!s->worker && my_tid() != atomic_load_explicit(&g_user, memory_order_relaxed)) s->worker = 1;
    note_start(s, r.pushed); s->run++;
    t_depth++; r.fn(r.arg); t_depth--;
}
static void probe_add(void *pool, job_fn fn, void *arg) {
    st_t *s = st();
    if (!t_depth) note_caller("Add");
    s->added++;
    rec_t *r = &g_rec[atomic_fetch_add(&g_nrec, 1) % CAP]; r->fn = fn; r->arg = arg; r->pushed = tick();
    g_game_add(pool, probe_job, r);
}
static void probe_wait(void *pool) {
    st_t *s = st(); int depth = t_depth;
    if (depth && atomic_fetch_add(&g_nested, 1) < 4) LOG("[d3jobs] Wait from inside a job (depth %d): the game's pool ends the outer Wait's cycle here", depth);
    if (!depth && atomic_load_explicit(&g_user, memory_order_relaxed)) note_caller("Wait");
    uint64_t t0 = depth ? 0 : tick();
    if (*(volatile int32_t *)((uint8_t *)pool + P_STATE)) g_game_wait(pool);   // 0x5160d0: ldr w8, [x0]; cbz w8, ret (the cbz is not in the copy)
    if (!depth) wait_over(s, t0, __builtin_return_address(0));
}

static void jump(uint32_t *at, const void *to) {   // ldr x16, #8; br x16; .quad to (at 8-byte aligned)
    uint64_t a = (uint64_t)(uintptr_t)to; at[0] = 0x58000050; at[1] = 0xd61f0200; at[2] = (uint32_t)a; at[3] = (uint32_t)(a >> 32);
}
void dm_d3_jobs_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_d3_jobs_install(void) {
    const char *e = getenv("DM_D3_JOBS"), *q = getenv("DM_D3_JOBS_STATS");
    int host = !(e && *e == '0' && !e[1]);
    g_stats = !(q && *q == '0' && !q[1]);
#if defined(__aarch64__)
    { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (f) g_tick_ns = 1e9 / (double)f; }
#endif
    if (!host && !g_stats) { LOG("[d3jobs] DM_D3_JOBS=0 DM_D3_JOBS_STATS=0: the game's own job pool, untouched"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) { LOG("【红】[d3jobs] no main module: the game's own job pool"); return; }
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (m->text_end < k_sites[i].off + 4 * k_sites[i].n || memcmp(m->base + k_sites[i].off, k_sites[i].w, 4 * k_sites[i].n)) {
            LOG("【红】[d3jobs] main+%#x differs: not D3 update 2.7.7 (build 2607a74f), the game's own job pool", k_sites[i].off); return; }
    uint32_t w_add[4], w_wait[4];
    if (host) {
        { const char *v = getenv("DM_D3_JOBS_SPIN_US"); g_spin_wait = (uint64_t)((v && *v ? atof(v) : 30.0) * 1e3 / g_tick_ns); }
        { const char *v = getenv("DM_D3_JOBS_WORKER_SPIN_US"); g_spin_worker = (uint64_t)((v && *v ? atof(v) : 0.0) * 1e3 / g_tick_ns); }
        ring_init();
        jump(w_add, jobs_add); jump(w_wait, jobs_wait);
    } else {
        // the words the two jumps replace, each followed by a jump back behind them (they do not depend on where they run; Wait's
        // second word, cbz w8, ret, is left out: probe_wait tests the state itself)
        uint32_t *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (t == MAP_FAILED) { LOG("【红】[d3jobs] no page for the probes: the game's own job pool, no measurements"); return; }
        memcpy(t, m->base + O_ADD, 16); jump(t + 4, m->base + O_ADD + 16);
        t[8] = k_sites[4].w[0]; t[9] = k_sites[4].w[2]; t[10] = k_sites[4].w[3]; t[11] = 0xd503201f /* nop */; jump(t + 12, m->base + O_WAIT + 16);
        if (mprotect(t, 4096, PROT_READ | PROT_EXEC)) { LOG("【红】[d3jobs] the probes' page cannot be made executable: the game's own job pool, no measurements"); return; }
        __builtin___clear_cache((char *)t, (char *)t + 4096);
        g_game_add = (void (*)(void *, job_fn, void *))(void *)t; g_game_wait = (void (*)(void *))(void *)(t + 8);
        jump(w_add, probe_add); jump(w_wait, probe_wait);
    }
    // entry -> host function. The worker keeps its prologue, thread setup and exit path: its loop becomes one call (x19 = the pool)
    // followed by a branch to the exit path
    uint64_t worker = (uint64_t)(uintptr_t)jobs_worker;
    const uint32_t w_worker[6] = {0x58000090 /* ldr x16, #16 */, 0xaa1303e0 /* mov x0, x19 */, 0xd63f0200 /* blr x16 */,
                                  0x14000000u | ((O_WORKER_EXIT - (O_WORKER_LOOP + 12)) >> 2) /* b exit */, (uint32_t)worker, (uint32_t)(worker >> 32)};
    // all sites or none (the game's Add with our worker would queue jobs nobody takes): one writable window over them
    uint8_t *lo = (uint8_t *)((uintptr_t)(m->base + O_WORKER_LOOP) & ~(uintptr_t)0x3FFF), *end = m->base + O_WAIT + sizeof w_wait;
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)lo + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(lo, len, PROT_READ | PROT_WRITE)) { LOG("【红】[d3jobs] mprotect RW failed: the game's own job pool"); return; }
    g_main = m->base; g_host_pool = host;
    if (host) memcpy(m->base + O_WORKER_LOOP, w_worker, sizeof w_worker);
    memcpy(m->base + O_ADD, w_add, sizeof w_add);
    memcpy(m->base + O_WAIT, w_wait, sizeof w_wait);
    if (mprotect(lo, len, PROT_READ | PROT_EXEC)) die("[d3jobs] mprotect RX");
    __builtin___clear_cache((char *)lo, (char *)lo + len);
    if (host) LOG("[d3jobs] job pool on the host: lock-free ring of %d, Add main+%#x, Wait main+%#x, worker loop main+%#x; Wait polls %.0f us before it sleeps, "
                  "a worker %.0f us (DM_D3_JOBS_SPIN_US / DM_D3_JOBS_WORKER_SPIN_US); measurements %s (DM_D3_JOBS_STATS); DM_D3_JOBS=0 = the game's own pool",
                  CAP, O_ADD, O_WAIT, O_WORKER_LOOP, g_spin_wait * g_tick_ns / 1e3, g_spin_worker * g_tick_ns / 1e3, g_stats ? "on" : "off");
    else LOG("[d3jobs] DM_D3_JOBS=0: the game's own job pool, measured through probes at Add main+%#x and Wait main+%#x (DM_D3_JOBS_STATS=0 = untouched)", O_ADD, O_WAIT);
}
#endif
