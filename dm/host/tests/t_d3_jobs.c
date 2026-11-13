// [D3] Stress test and throughput benchmark of the job pool that stands in for the game's (../hle_d3_jobs.c, issue #18); the pool
// only, no game.
// Stress: a "main" thread adds jobs whose descriptors live in its stack frame and waits, as main 0x102cf0 / 0x133030 do; two workers
// take them. Every job checks that its descriptor is alive when it starts and when it ends, every batch that all its jobs are done
// when Wait returns, and the frame is overwritten after each batch. Mixed in: jobs that wait from inside (the loading-screen pump),
// a second thread that waits at random times, kicked jobs with no descriptor and no Wait of their own, batches larger than the ring.
// Benchmark ("bench"): batches of N jobs of W busy-loop turns each (0 = empty jobs: the pool's own cost), Add x N then Wait, as a frame
// does; prints jobs/s and the time per batch.
// Build and run (Linux):  cc -O2 -pthread -o t_d3_jobs t_d3_jobs.c && ./t_d3_jobs [rounds [second thread 0 / 1]] && ./t_d3_jobs bench
//   -DJOBS_SRC='"file.c"' builds against another version of the pool (the first one, a queue under one mutex, for the comparison).
// (-fsanitize=thread reports the plain read of the pool's quit flag, which the game writes without a lock.)
#define DM_GAME_D3 1
#define _GNU_SOURCE
#ifndef JOBS_SRC
#define JOBS_SRC "../hle_d3_jobs.c"
#define NEW_POOL 1
#endif
#include JOBS_SRC
#include <stdatomic.h>
#include <unistd.h>
module_t g_mod[MAX_MOD]; int g_nmod; uint64_t g_t0_ns;
uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
void dm_thread_native_wait(int e) { (void)e; }
static struct { int32_t state, head, workers, quit; } g_pool = {0, 0, 2, 0};
typedef struct { uint64_t magic; int first, count; _Atomic int *done; int nested; } desc;
static _Atomic long g_bad, g_jobs_run, g_nested_waits;
static _Atomic unsigned g_seed = 12345;
static unsigned rnd(void) { unsigned s = atomic_load(&g_seed); s = s * 1103515245u + 12345u; atomic_store(&g_seed, s); return s >> 8; }
static void job(void *p) {
    desc *d = p;
    if (d->magic != 0x600DF00D5AFE5AFEull) { atomic_fetch_add(&g_bad, 1); return; }   // the descriptor's frame is gone
    volatile unsigned x = 0; for (int i = 0; i < d->count; i++) x += (unsigned)i * 2654435761u;
    if (d->nested) { atomic_fetch_add(&g_nested_waits, 1); jobs_wait(&g_pool); }       // the pump: a Wait from inside a job
    if (d->magic != 0x600DF00D5AFE5AFEull) atomic_fetch_add(&g_bad, 1);
    atomic_fetch_add(d->done, 1); atomic_fetch_add(&g_jobs_run, 1);
}
static _Atomic int g_heap_done;
static void heap_job(void *p) { volatile unsigned x = 0; for (int i = 0; i < 300; i++) x += (unsigned)i; (void)p; atomic_fetch_add(&g_heap_done, 1); atomic_fetch_add(&g_jobs_run, 1); }
static __attribute__((noinline)) void batch(int n, int nested_every) {   // like main 0x102cf0 / 0x133030: descriptors in this frame
    desc d[n]; _Atomic int done = 0;
    for (int i = 0; i < n; i++) { d[i] = (desc){0x600DF00D5AFE5AFEull, i, 50 + (int)(rnd() % 400), &done, nested_every && i % nested_every == 0}; jobs_add(&g_pool, job, &d[i]); }
    jobs_wait(&g_pool);
    if (atomic_load(&done) != n) { atomic_fetch_add(&g_bad, 1000000); fprintf(stderr, "batch of %d: only %d done at Wait's return\n", n, atomic_load(&done)); }
    for (int i = 0; i < n; i++) d[i].magic = 0xDEADDEADDEADDEADull;   // the frame dies: a job that starts now sees it
}
static __attribute__((noinline)) void scribble(void) { volatile uint64_t junk[9000]; for (int i = 0; i < 9000; i++) junk[i] = 0xBADBADBADull + (uint64_t)i; (void)junk[0]; }
static void *worker(void *p) { jobs_worker(p); return NULL; }
static _Atomic int g_stop;
static void *other(void *p) { (void)p; while (!atomic_load(&g_stop)) { jobs_wait(&g_pool); usleep(rnd() % 300); } return NULL; }

// ---- benchmark
typedef struct { int work; _Atomic long *sum; } bdesc;
static void bjob(void *p) { bdesc *d = p; volatile unsigned x = 0; for (int i = 0; i < d->work; i++) x += (unsigned)i * 2654435761u; atomic_fetch_add_explicit(d->sum, 1, memory_order_relaxed); }
static __attribute__((noinline)) void bbatch(int n, int work, _Atomic long *sum) {
    bdesc d[n];
    for (int i = 0; i < n; i++) { d[i].work = work; d[i].sum = sum; jobs_add(&g_pool, bjob, &d[i]); }
    jobs_wait(&g_pool);
}
static void bench(void) {
    static const int sizes[] = {8, 32, 128, 512, 1200}, works[] = {0, 200, 2000};   // 200 turns ~ 0.1-0.2 us, 2000 ~ 1-2 us
    printf("%-9s %-6s %12s %12s\n", "jobs", "work", "jobs/s", "us/batch");
    for (unsigned w = 0; w < sizeof works / sizeof *works; w++)
        for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
            _Atomic long sum = 0; long batches = 0; uint64_t t0 = now_ns(), t1;
            do { for (int i = 0; i < 50; i++) bbatch(sizes[k], works[w], &sum); batches += 50; t1 = now_ns(); } while (t1 - t0 < 700000000ull);
            if (atomic_load(&sum) != batches * sizes[k]) { printf("FAIL: %ld of %ld jobs done\n", atomic_load(&sum), batches * sizes[k]); exit(1); }
            printf("%-9d %-6d %12.0f %12.2f\n", sizes[k], works[w], (double)batches * sizes[k] * 1e9 / (double)(t1 - t0), (double)(t1 - t0) / 1e3 / (double)batches);
        }
}
int main(int argc, char **argv) {
    int is_bench = argc > 1 && !strcmp(argv[1], "bench");
    int rounds = argc > 1 && !is_bench ? atoi(argv[1]) : 20000, second = argc > 2 ? atoi(argv[2]) : 1;
    g_t0_ns = now_ns();
#ifdef NEW_POOL
    ring_init(); g_spin_wait = argc > 2 && is_bench ? (uint64_t)atoi(argv[2]) * 1000 : 30000; g_spin_worker = argc > 3 && is_bench ? (uint64_t)atoi(argv[3]) * 1000 : 0;
#endif
    pthread_t w[2], o; for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, worker, &g_pool);
    if (is_bench) { bench(); g_pool.quit = 1; for (int i = 0; i < 2; i++) pthread_join(w[i], NULL); return 0; }
    if (second) pthread_create(&o, NULL, other, NULL);
    long heap_added = 0;
    for (int r = 0; r < rounds; r++) {
        unsigned k = rnd() % 10;
        if (k < 5) batch(1 + (int)(rnd() % 120), 0);
        else if (k < 7) batch(1 + (int)(rnd() % 60), 1 + (int)(rnd() % 7));        // some jobs wait from inside
        else if (k < 8) batch(4500 + (int)(rnd() % 600), 0);                        // more than the ring holds
        else { int n = 1 + (int)(rnd() % 40); for (int i = 0; i < n; i++) jobs_add(&g_pool, heap_job, NULL); heap_added += n;   // kicked: no Wait yet
               if (rnd() & 1) batch(1 + (int)(rnd() % 30), 0); }
        scribble();
    }
    jobs_wait(&g_pool);
    atomic_store(&g_stop, 1); if (second) pthread_join(o, NULL);
    g_pool.quit = 1; for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
#ifdef NEW_POOL
    int rest = atomic_load(&g_out) || atomic_load(&g_parked) || atomic_load(&g_waiters) || atomic_load(&g_put) != atomic_load(&g_take);
#else
    int rest = g_head != g_tail || g_running || g_parked;
#endif
    int ok = !atomic_load(&g_bad) && atomic_load(&g_heap_done) == heap_added && !rest;
    printf("%s: %d rounds, %ld jobs run, %ld waits from inside a job, bad %ld, heap %d/%ld, left over %d\n",
           ok ? "PASS" : "FAIL", rounds, atomic_load(&g_jobs_run), atomic_load(&g_nested_waits), atomic_load(&g_bad), atomic_load(&g_heap_done), heap_added, rest);
    return !ok;
}
