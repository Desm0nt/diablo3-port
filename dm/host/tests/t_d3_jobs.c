// [D3] Stress test of the job queue that stands in for the game's job pool (../hle_d3_jobs.c, issue #18); the queue only, no game.
// A "main" thread adds jobs whose descriptors live in its stack frame and waits, as main 0x102cf0 / 0x133030 do; two workers take
// them. Every job checks that its descriptor is alive when it starts and when it ends, every batch that all its jobs are done when
// Wait returns, and the frame is overwritten after each batch. Mixed in: jobs that wait from inside (the loading-screen pump),
// a second thread that waits at random times, kicked jobs with no descriptor and no Wait of their own, batches larger than the queue.
// Build and run (Linux, any pthread platform):  cc -O2 -pthread -o t_d3_jobs t_d3_jobs.c && ./t_d3_jobs [rounds [second thread 0 / 1]]
// (-fsanitize=thread reports the plain read of the pool's quit flag, which the game writes without a lock).
#define DM_GAME_D3 1
#define _GNU_SOURCE
#include "../hle_d3_jobs.c"
#include <stdatomic.h>
#include <unistd.h>
module_t g_mod[MAX_MOD]; int g_nmod; uint64_t g_t0_ns;
uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
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
int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 20000, second = argc > 2 ? atoi(argv[2]) : 1;
    g_t0_ns = now_ns();
    pthread_t w[2], o; for (int i = 0; i < 2; i++) pthread_create(&w[i], NULL, worker, &g_pool);
    if (second) pthread_create(&o, NULL, other, NULL);
    long heap_added = 0;
    for (int r = 0; r < rounds; r++) {
        unsigned k = rnd() % 10;
        if (k < 5) batch(1 + (int)(rnd() % 120), 0);
        else if (k < 7) batch(1 + (int)(rnd() % 60), 1 + (int)(rnd() % 7));        // some jobs wait from inside
        else if (k < 8) batch(4500 + (int)(rnd() % 600), 0);                        // more than the queue holds
        else { int n = 1 + (int)(rnd() % 40); for (int i = 0; i < n; i++) jobs_add(&g_pool, heap_job, NULL); heap_added += n;   // kicked: no Wait yet
               if (rnd() & 1) batch(1 + (int)(rnd() % 30), 0); }
        scribble();
    }
    jobs_wait(&g_pool);
    atomic_store(&g_stop, 1); if (second) pthread_join(o, NULL);
    g_pool.quit = 1; for (int i = 0; i < 2; i++) pthread_join(w[i], NULL);
    int ok = !atomic_load(&g_bad) && atomic_load(&g_heap_done) == heap_added && g_head == g_tail && !g_running && !g_parked;
    printf("%s: %d rounds, %ld jobs run (%llu on the workers, %llu by Add on a full queue), %ld waits from inside a job, bad %ld, heap %d/%ld, longest queue %llu\n",
           ok ? "PASS" : "FAIL", rounds, atomic_load(&g_jobs_run), (unsigned long long)g_by_worker, (unsigned long long)g_direct, atomic_load(&g_nested_waits),
           atomic_load(&g_bad), atomic_load(&g_heap_done), heap_added, (unsigned long long)g_peak);
    return !ok;
}
