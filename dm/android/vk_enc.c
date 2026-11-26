// [vk-enc-thread] DM_GFX_VENC: the vkCmd* calls the translation thread makes while it translates a guest submission are copied into a
// ring and made in the driver by a thread of their own, dm-venc (port-common/modules/vk-enc-thread, DESIGN.md; KB PNH-A-01, PERF-X10).
// Adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: dm/gfx/vk_android/vka_enc.c (the records, the batched hand-over,
// the WFE + futex waits, the encoder loop). What differs from it:
//   - it sits behind the trampoline table (vk_driver.c dm_vk_resolve -> dm_venc_wrap), not behind a device dispatch table;
//   - deferred are the calls of the thread inside a window (dm_venc_window_begin / _end, opened by the engine with its lock held, around
//     xbg_submit_now), into any command buffer; peanuts defers the calls into one command buffer between vka_enc_start / _stop;
//   - every direct call that could meet a deferred one is gated instead of the engine calling stop: End / Begin / Reset of a command
//     buffer, a reset / allocation of its pool or a queue submit of it wait until the encoder has made the calls already in the ring for
//     that pool; destroys, frees and queue / device idles wait for the whole ring. Inside the window that is the producer's own wait
//     (everything, published or not); outside it (another thread, or the translation thread after the window) the wait is for what was
//     published, and window_end publishes everything before the engine lock is released, so a thread that takes the lock and ends or
//     submits the frame's command buffer (xbg_legacy_wait, the idle poll, a flush) finds every call made;
//   - DM_GFX_VENC=2 waits for the encoder at every window end as well (the encoder still records, but never across the engine lock).
// Order: per command buffer the driver sees the calls in program order (one ring, one consumer); a command buffer is ended, reset or
// submitted only after all of its calls; nothing else of the engine changes order (submits, fences, events, reports, present stay on
// their threads, synchronous). The copies: every array / structure a call points to is copied into the record, so the caller may reuse
// its memory at once; a pNext chain the encoder does not know makes the call wait for the ring and go directly (counted: fallback).
//
// Switches (read once, at the first resolved Vulkan call):
//   DM_GFX_VENC          unset = on, except on a PowerVR GPU (/dev/pvrsrvkm: its compiler runs inside vkCmd*, KB PVR-01); 1 = on;
//                        2 = on + wait at every window end; 0 = off: every slot holds the driver's pointer, as before this module
//   DM_GFX_VENC_RING_KB  ring size (default 4096; rounded up to a power of two, 64..262144)
//   DM_GFX_VENC_BATCH    bytes of calls handed over at once (default 4096; 0 = every call); also at every wait and window end
//   DM_GFX_VENC_TAIL     bytes the encoder makes between two tail stores (default 16384; 0 = every call); always when it catches up
//   DM_GFX_VENC_SPIN_US  how long a waiting thread spins (WFE, the clock read at every wake) before the futex (default 200)
//   DM_GFX_VENC_CPUS     hex CPU mask of dm-venc (default: the cpu-place plan's record mask = DOOM 2016's dm-record core, else every
//                        CPU but the translation thread's)
// Log: "NVN venc ..." at start, "NVN PERF venc ..." every 300 presents (from the translation thread, at a window end).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vk_enc.h"
#include <vulkan/vulkan.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifdef DM_VENC_HOST_TEST
#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#include "../host/dm.h"
#if defined(__has_include)
#if __has_include("../host/cpu_topo.h")
#include "../host/cpu_topo.h"
#define VENC_HAVE_CPU_PLAN 1
extern const dm_cpu_plan_t *dm_cpu_plan(void) __attribute__((weak));
#endif
#endif
#endif
void pc_note(const char *id, int applied, const char *fmt, ...) __attribute__((weak));

// ---------- configuration ----------
static _Atomic int g_mode = -1;            // -1 not read yet, 0 off, 1 on, 2 on + wait at every window end
static char g_mode_why[160];
static uint64_t g_rsz = 4u << 20, g_rmask = (4u << 20) - 1;
static uint32_t g_batch = 4096, g_tail_every = 16384;
static uint64_t g_spin_ns = 200000;
static uint32_t g_cpus_env;                // DM_GFX_VENC_CPUS (0 = unset)
static long env_long(const char *n, long def, long lo, long hi) {
    const char *e = getenv(n); if (!e || !*e) return def;
    char *end; long v = strtol(e, &end, 0); if (end == e) return def;
    return v < lo ? lo : v > hi ? hi : v;
}
static void cfg_once(void) {
    if (atomic_load_explicit(&g_mode, memory_order_relaxed) >= 0) return;
    const char *e = getenv("DM_GFX_VENC"); int m;
    if (e && *e) { m = atoi(e); if (m < 0 || m > 2) m = 1; snprintf(g_mode_why, sizeof g_mode_why, "DM_GFX_VENC=%d", m); }
    else if (!access("/dev/pvrsrvkm", F_OK)) { m = 0; snprintf(g_mode_why, sizeof g_mode_why, "default off on PowerVR (/dev/pvrsrvkm: the driver's compiler runs inside vkCmd*, PVR-01); DM_GFX_VENC=1 forces it"); }
    else { m = 1; snprintf(g_mode_why, sizeof g_mode_why, "default (DM_GFX_VENC unset)"); }
    long kb = env_long("DM_GFX_VENC_RING_KB", 4096, 64, 262144); uint64_t sz = 1; while (sz < (uint64_t)kb * 1024) sz <<= 1;
    g_rsz = sz; g_rmask = sz - 1;
    g_batch = (uint32_t)env_long("DM_GFX_VENC_BATCH", 4096, 0, 1 << 20);
    g_tail_every = (uint32_t)env_long("DM_GFX_VENC_TAIL", 16384, 0, 1 << 20);
    g_spin_ns = (uint64_t)env_long("DM_GFX_VENC_SPIN_US", 200, 0, 100000) * 1000;
    { const char *c = getenv("DM_GFX_VENC_CPUS"); g_cpus_env = c && *c ? (uint32_t)strtoul(c, NULL, 16) : 0; }
    atomic_store(&g_mode, m);
    LOG("NVN venc mode=%d (%s): %s", m, g_mode_why, m ? "vkCmd* of the translation thread recorded on dm-venc" : "off, every Vulkan call goes to the driver directly");
}
int dm_venc_on(void) { return atomic_load_explicit(&g_mode, memory_order_relaxed) > 0; }

// ---------- the driver's entry points (set by dm_venc_wrap before the slot gets the wrapper) ----------
#define VENC_CMDS(X) \
    X(vkCmdBeginQuery) X(vkCmdBeginRenderPass) X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyQueryPoolResults) \
    X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirect) \
    X(vkCmdEndQuery) X(vkCmdEndRenderPass) X(vkCmdFillBuffer) X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdResetQueryPool) \
    X(vkCmdResolveImage) X(vkCmdSetScissor) X(vkCmdSetViewport) X(vkCmdWriteTimestamp) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)
#define VENC_GATED(X) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkQueuePresentKHR) X(vkDeviceWaitIdle) X(vkGetDeviceProcAddr) \
    X(vkDestroyBuffer) X(vkDestroyBufferView) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDescriptorUpdateTemplate) X(vkDestroyDevice) X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) \
    X(vkDestroyPipeline) X(vkDestroyPipelineLayout) X(vkDestroyQueryPool) X(vkDestroyRenderPass) X(vkDestroySampler) X(vkFreeMemory)
#define VENC_REAL(f) PFN_##f f;
static struct { VENC_CMDS(VENC_REAL) VENC_GATED(VENC_REAL) } R;
// A thread that loaded a wrapper from a slot filled by another thread may in theory read its R field before that store lands.
static void *real_wait(void *const volatile *p) { void *v; while (!(v = __atomic_load_n(p, __ATOMIC_ACQUIRE))) sched_yield(); return v; }
#define REAL(f) (__builtin_expect(R.f != NULL, 1) ? R.f : (PFN_##f)real_wait((void *const volatile *)&R.f))

// ---------- time, waits ----------
static inline uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static long fwait(_Atomic uint32_t *a, uint32_t v) { struct timespec t = {1, 0}; return syscall(SYS_futex, (uint32_t *)a, FUTEX_WAIT_PRIVATE, v, &t, NULL, 0); }   // bounded: callers check again
static void fwake(_Atomic uint32_t *a) { syscall(SYS_futex, (uint32_t *)a, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0); }
// Low-power wait until *a may have changed from old: WFE wakes when another core writes the monitored line (or at the event stream tick).
static inline uint64_t wait_change(_Atomic uint64_t *a, uint64_t old) {
#if defined(__aarch64__)
    uint64_t v; __asm__ volatile("ldaxr %0, [%1]" : "=r"(v) : "r"(a) : "memory");
    if (v == old) __asm__ volatile("wfe" ::: "memory");
    return v;
#else
    (void)old; sched_yield(); return atomic_load_explicit(a, memory_order_acquire);
#endif
}
static inline uintptr_t tp(void) {
#if defined(__aarch64__)
    return (uintptr_t)__builtin_thread_pointer();
#else
    return (uintptr_t)pthread_self();
#endif
}

// ---------- the ring ----------
typedef struct { uint32_t id, size; } hdr;   // size: the whole record, a multiple of 8
#define A8(n) (((size_t)(n) + 7) & ~(size_t)7)
#define VENC_ID(f) E_##f,
enum { E_WRAP = 0, VENC_CMDS(VENC_ID) E_N };
static uint8_t *g_ring;
static _Alignas(64) _Atomic uint64_t g_head;   // bytes handed over (positions mod g_rsz)
static _Alignas(64) _Atomic uint64_t g_tail;   // bytes the encoder has made
static _Alignas(64) uint64_t g_ph, g_pub, g_tc; // producer only (inside a window, under the engine lock): head, last published, cached tail
static _Alignas(64) _Atomic uint64_t g_ph_shadow;   // g_ph for the checks of other threads (a plain store per call)
// Each variable written often by one thread and read by another sits on a cache line of its own (_Alignas(64) applies to every declarator):
// a producer store per call next to a flag the encoder reads per call cost ~100 ns a call in the host test (false sharing).
static _Alignas(64) _Atomic int g_csleep, g_pwait, g_fwait;   // encoder asleep on g_cseq / producer waiting on g_pseq / other threads waiting for the tail
static _Alignas(64) _Atomic uint32_t g_cseq, g_pseq;
static _Alignas(64) _Atomic uint64_t g_pwant;
static _Alignas(64) _Atomic int g_live;          // the encoder thread runs
static _Alignas(64) _Atomic uintptr_t g_win_tp;  // the thread inside a window (0 = none)
static int g_win_depth; static uint64_t g_win_t0;

// Statistics. Producer counters are plain (one thread at a time, under the engine lock); the others are atomics.
enum { W_END, W_BEGIN, W_RESET, W_POOL, W_SUBMIT, W_IDLE, W_DESTROY, W_FALLBACK, W_WINEND, W_DIRECT, W_N };
static const char *const w_name[W_N] = {"end", "begin", "reset", "pool", "submit", "idle", "destroy", "fallback", "winend", "direct"};
static _Alignas(64) uint64_t s_calls, s_bytes;   // (the producer's per-call counters: one line each, next to nothing the encoder reads)
static uint64_t s_pubs, s_wakes, s_wins, s_win_ns, s_full, s_full_ns, s_sync_n[W_N], s_sync_ns[W_N], s_fallbacks;
static _Alignas(64) _Atomic uint64_t s_busy_ns, s_recs;   // the encoder's, per batch
static _Alignas(64) _Atomic uint64_t s_sleeps, s_fn, s_fns, s_unpub, s_frames;
static _Atomic uint64_t s_fn_why[W_N];

static inline int spin_over(uint64_t t0) { return mono_ns() - t0 >= g_spin_ns; }
static inline void publish_now(void) {
    g_pub = g_ph; s_pubs++;
    atomic_store_explicit(&g_head, g_ph, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);   // the head before the encoder's sleep flag is read
    if (atomic_load_explicit(&g_csleep, memory_order_relaxed) && atomic_exchange(&g_csleep, 0)) { atomic_fetch_add(&g_cseq, 1); fwake(&g_cseq); s_wakes++; }
}
// The producer waits until the encoder's tail reaches want (spinning a little, then on the futex; as peanuts' wait_tail).
static void wait_tail(uint64_t want) {
    if (g_pub != g_ph) publish_now();   // what it waits for must be handed over
    for (uint64_t t0 = mono_ns();;) {
        uint64_t t = atomic_load_explicit(&g_tail, memory_order_acquire); if (t >= want) { g_tc = t; return; }
        if (spin_over(t0)) break;
        wait_change(&g_tail, t);
    }
    atomic_store(&g_pwant, want);
    for (uint64_t t0 = mono_ns();;) {   // an event count: the sequence is read before the flag is set (only this thread clears its flag)
        uint32_t s = atomic_load(&g_pseq);
        atomic_store(&g_pwait, 1);
        atomic_thread_fence(memory_order_seq_cst);   // the flag before the tail is read again (the encoder stores the tail, then reads the flag)
        if (atomic_load(&g_tail) >= want) break;
        fwait(&g_pseq, s);
        if (mono_ns() - t0 > 2000000000ull) { static int nr; if (nr++ < 4) LOG("NVN venc: the translation thread has waited %.1f s for dm-venc (tail %llu, want %llu, head %llu, encoder %s)",
            (mono_ns() - t0) / 1e9, (unsigned long long)atomic_load(&g_tail), (unsigned long long)want, (unsigned long long)atomic_load(&g_head), atomic_load(&g_csleep) ? "asleep" : "awake"); t0 = mono_ns(); }
    }
    atomic_store(&g_pwait, 0);
    g_tc = atomic_load_explicit(&g_tail, memory_order_acquire);
}
static inline void room(uint64_t n) {
    if (g_ph + n - g_tc <= g_rsz) return;
    g_tc = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (g_ph + n - g_tc <= g_rsz) return;
    uint64_t t0 = mono_ns(); wait_tail(g_ph + n - g_rsz); s_full++; s_full_ns += mono_ns() - t0;
}
// Room for a record of n bytes (a multiple of 8), contiguous in the ring
static void *rec_alloc(uint32_t id, size_t n) {
    uint64_t pos = g_ph & g_rmask;
    if (pos + n > g_rsz) {   // a wrap record fills the rest of the ring (at least 8 bytes: everything is 8-aligned)
        uint64_t rest = g_rsz - pos; room(rest);
        hdr *w = (hdr *)(g_ring + pos); w->id = E_WRAP; w->size = (uint32_t)rest;
        g_ph += rest; pos = 0;
    }
    room(n);
    hdr *h = (hdr *)(g_ring + pos); h->id = id; h->size = (uint32_t)n;
    return h;
}

// ---------- which command pools have calls in the ring ----------
// cb -> pool, from vkAllocateCommandBuffers (any thread; insert under a mutex, lock-free lookups). Unknown: the cb itself is the key.
enum { PMAP = 512 };
static struct { _Atomic uintptr_t cb, pool; } g_pmap[PMAP];
static pthread_mutex_t g_pmap_mu = PTHREAD_MUTEX_INITIALIZER;
static inline unsigned pmap_h(uintptr_t cb) { return (unsigned)((cb >> 3) ^ (cb >> 13)) & (PMAP - 1); }
static uintptr_t key_of(VkCommandBuffer cb) {
    uintptr_t c = (uintptr_t)cb; unsigned h = pmap_h(c);
    for (unsigned i = 0; i < 16; i++) { uintptr_t e = atomic_load_explicit(&g_pmap[(h + i) & (PMAP - 1)].cb, memory_order_acquire); if (!e) break;
        if (e == c) { uintptr_t p = atomic_load_explicit(&g_pmap[(h + i) & (PMAP - 1)].pool, memory_order_relaxed); return p ? p : c; } }
    return c;
}
static void pmap_put(VkCommandBuffer cb, VkCommandPool pool) {
    uintptr_t c = (uintptr_t)cb; unsigned h = pmap_h(c);
    pthread_mutex_lock(&g_pmap_mu);
    for (unsigned i = 0; i < 16; i++) { unsigned k = (h + i) & (PMAP - 1); uintptr_t e = atomic_load_explicit(&g_pmap[k].cb, memory_order_relaxed);
        if (e == c || !e) { atomic_store_explicit(&g_pmap[k].pool, (uintptr_t)pool, memory_order_relaxed); atomic_store_explicit(&g_pmap[k].cb, c, memory_order_release); break; } }
    pthread_mutex_unlock(&g_pmap_mu);   // (a full probe run: the cb stays unknown and is its own key)
}
static void pmap_forget_pool(VkCommandPool pool) {
    pthread_mutex_lock(&g_pmap_mu);
    for (unsigned k = 0; k < PMAP; k++) if (atomic_load_explicit(&g_pmap[k].pool, memory_order_relaxed) == (uintptr_t)pool) atomic_store_explicit(&g_pmap[k].pool, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_pmap_mu);
}
// Keys (pools) with calls in the ring, each with the ring position at the end of its last call: calls of key k are all in the driver once
// the encoder's tail has passed g_tpos of k. Written by the producer only, before the calls are published; an entry whose calls are all
// made is reused for another key; more than TOUCH keys at once: every key counts as having calls up to the head (g_touch_all).
enum { TOUCH = 8 };
static _Alignas(64) _Atomic uintptr_t g_tkey[TOUCH]; static _Alignas(64) _Atomic uint64_t g_tpos[TOUCH]; static _Alignas(64) _Atomic int g_ntouch, g_touch_all;
static uintptr_t g_last_cb; static int g_tidx = -1;   // producer: cb of the last deferred call and its entry (-1: none / all)
static void touch_slow(VkCommandBuffer cb) {
    g_last_cb = (uintptr_t)cb; g_tidx = -1;
    if (atomic_load_explicit(&g_touch_all, memory_order_relaxed)) return;
    uintptr_t k = key_of(cb); int n = atomic_load_explicit(&g_ntouch, memory_order_relaxed);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_tkey[i], memory_order_relaxed) == k) { g_tidx = i; return; }
    uint64_t t = atomic_load_explicit(&g_tail, memory_order_acquire); int i = 0;
    while (i < n && atomic_load_explicit(&g_tpos[i], memory_order_relaxed) > t) i++;   // an entry whose calls are all made
    if (i == n && n == TOUCH) { atomic_store_explicit(&g_touch_all, 1, memory_order_release); return; }
    atomic_store_explicit(&g_tpos[i], g_ph + 1, memory_order_relaxed);   // (pending until the call's end is stored in rec_done)
    atomic_store_explicit(&g_tkey[i], k, memory_order_release);
    if (i == n) atomic_store_explicit(&g_ntouch, n + 1, memory_order_release);
    g_tidx = i;
}
static inline void touch(VkCommandBuffer cb) { if ((uintptr_t)cb != g_last_cb) touch_slow(cb); }
static inline void touch_clear(void) { atomic_store_explicit(&g_ntouch, 0, memory_order_relaxed); atomic_store_explicit(&g_touch_all, 0, memory_order_relaxed); g_last_cb = 0; g_tidx = -1; }
// The ring position up to which key k has calls (0: none); all_pos when every key counts
static uint64_t key_pos(uintptr_t k, uint64_t all_pos) {
    if (atomic_load_explicit(&g_touch_all, memory_order_acquire)) return all_pos;
    int n = atomic_load_explicit(&g_ntouch, memory_order_acquire);
    for (int i = 0; i < n && i < TOUCH; i++) if (atomic_load_explicit(&g_tkey[i], memory_order_acquire) == k) return atomic_load_explicit(&g_tpos[i], memory_order_relaxed);
    return 0;
}
static inline void rec_done(size_t n) {
    g_ph += n; s_calls++; s_bytes += n;
    if (g_tidx >= 0) atomic_store_explicit(&g_tpos[g_tidx], g_ph, memory_order_relaxed);   // before any publication of this call
    atomic_store_explicit(&g_ph_shadow, g_ph, memory_order_relaxed);
    if (g_ph - g_pub >= g_batch) publish_now();
}

// ---------- syncs ----------
static inline int in_win(void) { return atomic_load_explicit(&g_win_tp, memory_order_relaxed) == tp(); }
static inline int pending(void) { return atomic_load_explicit(&g_tail, memory_order_acquire) != g_ph; }   // producer, inside its window
static inline int busy(void) { uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire); return atomic_load_explicit(&g_tail, memory_order_acquire) < h; }
// The producer, inside its window: every call up to ring position p (published or not) is in the driver.
static void pwait_to(uint64_t p, int why) {
    g_tc = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (g_tc < p) { uint64_t t0 = mono_ns(); wait_tail(p); s_sync_n[why]++; s_sync_ns[why] += mono_ns() - t0; }
    if (g_tc == g_ph) touch_clear();   // the ring is empty
}
static void psync(int why) { if (g_pub != g_ph) publish_now(); pwait_to(g_ph, why); }
// Any other thread (or the producer outside its window): every call up to ring position p is in the driver. cbwhy: the caller is about
// to use a command buffer / pool whose calls must all have been handed over already (window_end does that before the engine lock is
// released): a gap means a thread uses the translation thread's command buffer without the engine lock - counted (unpub), and waited for.
static void fwait_to(uint64_t p, int why, int cbwhy) {
    uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire);
    if (cbwhy && p > h) {
        atomic_fetch_add_explicit(&s_unpub, 1, memory_order_relaxed);
        static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 4) LOG("NVN venc: a %s on a command buffer with calls not yet handed over (head %llu, its calls up to %llu, window %s): waiting for them",
            w_name[why], (unsigned long long)h, (unsigned long long)p, atomic_load(&g_win_tp) ? "open" : "closed");
        for (uint64_t t0 = mono_ns(); atomic_load_explicit(&g_head, memory_order_acquire) < p && mono_ns() - t0 < 50000000ull;) sched_yield();
        h = atomic_load_explicit(&g_head, memory_order_acquire);
    }
    if (p > h) p = h;
    if (atomic_load_explicit(&g_tail, memory_order_acquire) >= p) return;
    uint64_t t0 = mono_ns(); atomic_fetch_add(&g_fwait, 1);   // the encoder stores its tail after each call while this is set
    for (;;) {
        uint64_t t = atomic_load_explicit(&g_tail, memory_order_acquire); if (t >= p) break;
        if (!spin_over(t0)) { wait_change(&g_tail, t); continue; }
        struct timespec d = {0, 20000}; nanosleep(&d, NULL);
    }
    atomic_fetch_sub(&g_fwait, 1);
    atomic_fetch_add_explicit(&s_fn, 1, memory_order_relaxed); atomic_fetch_add_explicit(&s_fns, mono_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_fn_why[why], 1, memory_order_relaxed);
}
// A direct call that uses command buffer cb or pool k: waits until every deferred call of that pool is in the driver
static void gate_key(uintptr_t k, int why) {
    if (!atomic_load_explicit(&g_live, memory_order_relaxed)) return;
    if (in_win()) { if (pending()) { uint64_t p = key_pos(k, g_ph); if (p > g_tc) pwait_to(p, why); } return; }
    uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire), t = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (t >= h && atomic_load_explicit(&g_ph_shadow, memory_order_relaxed) <= h) return;   // nothing outstanding (the fast path)
    uint64_t p = key_pos(k, h); if (p > t) fwait_to(p, why, 1);
}
static inline void gate_cb(VkCommandBuffer cb, int why) {
    if (!atomic_load_explicit(&g_live, memory_order_relaxed)) return;
    if (in_win()) { if (pending()) { uint64_t p = key_pos(key_of(cb), g_ph); if (p > g_tc) pwait_to(p, why); } return; }
    uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire), t = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (t >= h && atomic_load_explicit(&g_ph_shadow, memory_order_relaxed) <= h) return;
    uint64_t p = key_pos(key_of(cb), h); if (p > t) fwait_to(p, why, 1);
}
// A direct call that may end the life of an object a deferred call names, or waits for the queue: every call handed over is made
static inline void gate_all(int why) {
    if (!atomic_load_explicit(&g_live, memory_order_relaxed)) return;
    if (in_win()) { if (pending()) psync(why); return; }
    uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire);
    if (atomic_load_explicit(&g_tail, memory_order_acquire) < h) fwait_to(h, why, 0);
}

// ---------- the records: each holds the call's arguments, pointers aimed at copies inside the record ----------
#define CB_FIELD hdr h; VkCommandBuffer cb;
typedef struct { CB_FIELD VkPipelineStageFlags s, d; VkDependencyFlags f; uint32_t nm, nb, ni; const VkMemoryBarrier *m; const VkBufferMemoryBarrier *b; const VkImageMemoryBarrier *i; } r_bar;
typedef struct { CB_FIELD VkBuffer s, d; uint32_t n; const VkBufferCopy *r; } r_cpb;
typedef struct { CB_FIELD VkBuffer d; VkDeviceSize o, n; uint32_t v; } r_fill;
typedef struct { CB_FIELD VkBuffer b; VkImage im; VkImageLayout l; uint32_t n; const VkBufferImageCopy *r; } r_b2i;
typedef struct { CB_FIELD VkImage s, d; VkImageLayout sl, dl; uint32_t n; const void *r; VkFilter f; } r_i2i;   // copy, blit, resolve
typedef struct { CB_FIELD VkImage im; VkImageLayout l; VkClearValue v; uint32_t n; const VkImageSubresourceRange *r; } r_clr;
typedef struct { CB_FIELD uint32_t na, nr; const VkClearAttachment *a; const VkClearRect *r; } r_clra;
typedef struct { CB_FIELD VkRenderPassBeginInfo rb; VkRenderPassAttachmentBeginInfo ab; VkSubpassContents sc; } r_rpb;
typedef struct { CB_FIELD VkPipelineBindPoint bp; VkPipeline p; } r_pipe;
typedef struct { CB_FIELD VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; const VkDescriptorSet *s; const uint32_t *d; } r_sets;
typedef struct { CB_FIELD uint32_t first, n; const VkBuffer *b; const VkDeviceSize *o; } r_vb;
typedef struct { CB_FIELD VkBuffer b; VkDeviceSize o; VkIndexType t; } r_ib;
typedef struct { CB_FIELD uint32_t a, b, c, d; int32_t e; uint32_t f; } r_draw;   // draw (a count, b instances, c first, d first instance), indexed (e vertex offset, f first instance), dispatch (a b c)
typedef struct { CB_FIELD VkBuffer b; VkDeviceSize o; uint32_t n, st; } r_ind;
typedef struct { CB_FIELD VkBuffer b, cb2; VkDeviceSize o, o2; uint32_t n, st; } r_indc;
typedef struct { CB_FIELD uint32_t first, n; const void *p; } r_vps;   // viewports or scissors
typedef struct { CB_FIELD VkPipelineLayout l; VkShaderStageFlags st; uint32_t o, n; const void *p; } r_pc;
typedef struct { CB_FIELD VkQueryPool qp; uint32_t q, n; VkQueryControlFlags f; VkPipelineStageFlagBits stage; } r_qry;   // begin / end / reset / timestamp
typedef struct { CB_FIELD VkQueryPool qp; uint32_t q, n; VkBuffer b; VkDeviceSize o, st; VkQueryResultFlags f; } r_cqr;
typedef struct { CB_FIELD } r_none;

static inline uint8_t *cpy(uint8_t **at, const void *src, size_t n) { uint8_t *d = *at; if (n) memcpy(d, src, n); *at += A8(n); return d; }
enum { REC_MAX_DIV = 4 };   // a record larger than a quarter of the ring goes directly (after the ring)
#define DEFERRED(f, ...) do { if (!in_win()) { gate_cb(cb, W_DIRECT); REAL(f)(__VA_ARGS__); return; } } while (0)
#define FALLBACK(f, ...) do { psync(W_FALLBACK); s_fallbacks++; REAL(f)(__VA_ARGS__); return; } while (0)
#define BIG(extra, f, ...) do { if ((uint64_t)(extra) + 256 > g_rsz / REC_MAX_DIV) FALLBACK(f, __VA_ARGS__); } while (0)
#define BEGIN(T, id, extra) touch(cb); T *r = (T *)rec_alloc(id, A8(sizeof(T)) + (extra)); uint8_t *at = (uint8_t *)r + A8(sizeof(T)); (void)at; r->cb = cb;
#define END() rec_done(r->h.size)

static void VKAPI_CALL e_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    DEFERRED(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < nm; k++) if (m[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < nb; k++) if (b[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    for (uint32_t k = 0; k < ni; k++) if (i[k].pNext) FALLBACK(vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    size_t extra = A8(nm * sizeof *m) + A8(nb * sizeof *b) + A8(ni * sizeof *i);
    BIG(extra, vkCmdPipelineBarrier, cb, s, d, f, nm, m, nb, b, ni, i);
    BEGIN(r_bar, E_vkCmdPipelineBarrier, extra);
    r->s = s; r->d = d; r->f = f; r->nm = nm; r->nb = nb; r->ni = ni;
    r->m = (void *)cpy(&at, m, nm * sizeof *m); r->b = (void *)cpy(&at, b, nb * sizeof *b); r->i = (void *)cpy(&at, i, ni * sizeof *i);
    END();
}
static void VKAPI_CALL e_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg) {
    DEFERRED(vkCmdCopyBuffer, cb, s, d, n, rg); BIG(n * sizeof *rg, vkCmdCopyBuffer, cb, s, d, n, rg);
    BEGIN(r_cpb, E_vkCmdCopyBuffer, A8(n * sizeof *rg)); r->s = s; r->d = d; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer d, VkDeviceSize o, VkDeviceSize n, uint32_t v) {
    DEFERRED(vkCmdFillBuffer, cb, d, o, n, v);
    BEGIN(r_fill, E_vkCmdFillBuffer, 0); r->d = d; r->o = o; r->n = n; r->v = v; END();
}
static void VKAPI_CALL e_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    DEFERRED(vkCmdCopyBufferToImage, cb, b, im, l, n, rg); BIG(n * sizeof *rg, vkCmdCopyBufferToImage, cb, b, im, l, n, rg);
    BEGIN(r_b2i, E_vkCmdCopyBufferToImage, A8(n * sizeof *rg)); r->b = b; r->im = im; r->l = l; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage im, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *rg) {
    DEFERRED(vkCmdCopyImageToBuffer, cb, im, l, b, n, rg); BIG(n * sizeof *rg, vkCmdCopyImageToBuffer, cb, im, l, b, n, rg);
    BEGIN(r_b2i, E_vkCmdCopyImageToBuffer, A8(n * sizeof *rg)); r->b = b; r->im = im; r->l = l; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    DEFERRED(vkCmdCopyImage, cb, s, sl, d, dl, n, rg); BIG(n * sizeof *rg, vkCmdCopyImage, cb, s, sl, d, dl, n, rg);
    BEGIN(r_i2i, E_vkCmdCopyImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f) {
    DEFERRED(vkCmdBlitImage, cb, s, sl, d, dl, n, rg, f); BIG(n * sizeof *rg, vkCmdBlitImage, cb, s, sl, d, dl, n, rg, f);
    BEGIN(r_i2i, E_vkCmdBlitImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->f = f; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *rg) {
    DEFERRED(vkCmdResolveImage, cb, s, sl, d, dl, n, rg); BIG(n * sizeof *rg, vkCmdResolveImage, cb, s, sl, d, dl, n, rg);
    BEGIN(r_i2i, E_vkCmdResolveImage, A8(n * sizeof *rg)); r->s = s; r->sl = sl; r->d = d; r->dl = dl; r->n = n; r->r = cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    DEFERRED(vkCmdClearColorImage, cb, im, l, v, n, rg); BIG(n * sizeof *rg, vkCmdClearColorImage, cb, im, l, v, n, rg);
    BEGIN(r_clr, E_vkCmdClearColorImage, A8(n * sizeof *rg)); r->im = im; r->l = l; r->v.color = *v; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    DEFERRED(vkCmdClearDepthStencilImage, cb, im, l, v, n, rg); BIG(n * sizeof *rg, vkCmdClearDepthStencilImage, cb, im, l, v, n, rg);
    BEGIN(r_clr, E_vkCmdClearDepthStencilImage, A8(n * sizeof *rg)); r->im = im; r->l = l; r->v.depthStencil = *v; r->n = n; r->r = (void *)cpy(&at, rg, n * sizeof *rg); END();
}
static void VKAPI_CALL e_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rc) {
    DEFERRED(vkCmdClearAttachments, cb, na, a, nr, rc); BIG(na * sizeof *a + nr * sizeof *rc, vkCmdClearAttachments, cb, na, a, nr, rc);
    BEGIN(r_clra, E_vkCmdClearAttachments, A8(na * sizeof *a) + A8(nr * sizeof *rc)); r->na = na; r->nr = nr; r->a = (void *)cpy(&at, a, na * sizeof *a); r->r = (void *)cpy(&at, rc, nr * sizeof *rc); END();
}
static void VKAPI_CALL e_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *rb, VkSubpassContents sc) {   // (with or without one VkRenderPassAttachmentBeginInfo)
    DEFERRED(vkCmdBeginRenderPass, cb, rb, sc);
    const VkRenderPassAttachmentBeginInfo *ab = (const VkRenderPassAttachmentBeginInfo *)rb->pNext;
    if (ab && (ab->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO || ab->pNext)) FALLBACK(vkCmdBeginRenderPass, cb, rb, sc);
    size_t nv = ab ? ab->attachmentCount * sizeof(VkImageView) : 0, nc = rb->clearValueCount * sizeof(VkClearValue);
    BIG(nv + nc, vkCmdBeginRenderPass, cb, rb, sc);
    BEGIN(r_rpb, E_vkCmdBeginRenderPass, A8(nv) + A8(nc));
    r->rb = *rb; r->sc = sc;
    r->rb.pClearValues = nc ? (void *)cpy(&at, rb->pClearValues, nc) : NULL;
    if (ab) { r->ab = *ab; r->ab.pAttachments = (void *)cpy(&at, ab->pAttachments, nv); }
    else r->ab.sType = 0;
    r->rb.pNext = NULL;   // (set when the call is made: the record is not where the copy was made)
    END();
}
static void VKAPI_CALL e_vkCmdEndRenderPass(VkCommandBuffer cb) { DEFERRED(vkCmdEndRenderPass, cb); BEGIN(r_none, E_vkCmdEndRenderPass, 0); END(); }
static void VKAPI_CALL e_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    DEFERRED(vkCmdBindPipeline, cb, bp, p); BEGIN(r_pipe, E_vkCmdBindPipeline, 0); r->bp = bp; r->p = p; END();
}
static void VKAPI_CALL e_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    DEFERRED(vkCmdBindDescriptorSets, cb, bp, l, first, n, s, nd, d); BIG(n * sizeof *s + nd * sizeof *d, vkCmdBindDescriptorSets, cb, bp, l, first, n, s, nd, d);
    BEGIN(r_sets, E_vkCmdBindDescriptorSets, A8(n * sizeof *s) + A8(nd * sizeof *d)); r->bp = bp; r->l = l; r->first = first; r->n = n; r->nd = nd;
    r->s = (void *)cpy(&at, s, n * sizeof *s); r->d = nd ? (void *)cpy(&at, d, nd * sizeof *d) : NULL; END();
}
static void VKAPI_CALL e_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    DEFERRED(vkCmdBindVertexBuffers, cb, first, n, b, o); BIG(n * (sizeof *b + sizeof *o), vkCmdBindVertexBuffers, cb, first, n, b, o);
    BEGIN(r_vb, E_vkCmdBindVertexBuffers, A8(n * sizeof *b) + A8(n * sizeof *o)); r->first = first; r->n = n;
    r->b = (void *)cpy(&at, b, n * sizeof *b); r->o = (void *)cpy(&at, o, n * sizeof *o); END();
}
static void VKAPI_CALL e_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    DEFERRED(vkCmdBindIndexBuffer, cb, b, o, t); BEGIN(r_ib, E_vkCmdBindIndexBuffer, 0); r->b = b; r->o = o; r->t = t; END();
}
static void VKAPI_CALL e_vkCmdDraw(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, uint32_t fi) {
    DEFERRED(vkCmdDraw, cb, n, i, f, fi); BEGIN(r_draw, E_vkCmdDraw, 0); r->a = n; r->b = i; r->c = f; r->d = fi; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t f, int32_t vo, uint32_t fi) {
    DEFERRED(vkCmdDrawIndexed, cb, n, i, f, vo, fi); BEGIN(r_draw, E_vkCmdDrawIndexed, 0); r->a = n; r->b = i; r->c = f; r->e = vo; r->f = fi; END();
}
static void VKAPI_CALL e_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st) {
    DEFERRED(vkCmdDrawIndirect, cb, b, o, n, st); BEGIN(r_ind, E_vkCmdDrawIndirect, 0); r->b = b; r->o = o; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st) {
    DEFERRED(vkCmdDrawIndexedIndirect, cb, b, o, n, st); BEGIN(r_ind, E_vkCmdDrawIndexedIndirect, 0); r->b = b; r->o = o; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t n, uint32_t st) {
    DEFERRED(vkCmdDrawIndirectCount, cb, b, o, c, co, n, st); BEGIN(r_indc, E_vkCmdDrawIndirectCount, 0); r->b = b; r->o = o; r->cb2 = c; r->o2 = co; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t n, uint32_t st) {
    DEFERRED(vkCmdDrawIndexedIndirectCount, cb, b, o, c, co, n, st); BEGIN(r_indc, E_vkCmdDrawIndexedIndirectCount, 0); r->b = b; r->o = o; r->cb2 = c; r->o2 = co; r->n = n; r->st = st; END();
}
static void VKAPI_CALL e_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    DEFERRED(vkCmdDispatch, cb, x, y, z); BEGIN(r_draw, E_vkCmdDispatch, 0); r->a = x; r->b = y; r->c = z; END();
}
static void VKAPI_CALL e_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    DEFERRED(vkCmdDispatchIndirect, cb, b, o); BEGIN(r_ind, E_vkCmdDispatchIndirect, 0); r->b = b; r->o = o; END();
}
static void VKAPI_CALL e_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    DEFERRED(vkCmdSetViewport, cb, first, n, v); BIG(n * sizeof *v, vkCmdSetViewport, cb, first, n, v);
    BEGIN(r_vps, E_vkCmdSetViewport, A8(n * sizeof *v)); r->first = first; r->n = n; r->p = cpy(&at, v, n * sizeof *v); END();
}
static void VKAPI_CALL e_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *v) {
    DEFERRED(vkCmdSetScissor, cb, first, n, v); BIG(n * sizeof *v, vkCmdSetScissor, cb, first, n, v);
    BEGIN(r_vps, E_vkCmdSetScissor, A8(n * sizeof *v)); r->first = first; r->n = n; r->p = cpy(&at, v, n * sizeof *v); END();
}
static void VKAPI_CALL e_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t o, uint32_t n, const void *p) {
    DEFERRED(vkCmdPushConstants, cb, l, st, o, n, p); BIG(n, vkCmdPushConstants, cb, l, st, o, n, p);
    BEGIN(r_pc, E_vkCmdPushConstants, A8(n)); r->l = l; r->st = st; r->o = o; r->n = n; r->p = cpy(&at, p, n); END();
}
static void VKAPI_CALL e_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q, VkQueryControlFlags f) {
    DEFERRED(vkCmdBeginQuery, cb, qp, q, f); BEGIN(r_qry, E_vkCmdBeginQuery, 0); r->qp = qp; r->q = q; r->f = f; END();
}
static void VKAPI_CALL e_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool qp, uint32_t q) {
    DEFERRED(vkCmdEndQuery, cb, qp, q); BEGIN(r_qry, E_vkCmdEndQuery, 0); r->qp = qp; r->q = q; END();
}
static void VKAPI_CALL e_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool qp, uint32_t q, uint32_t n) {
    DEFERRED(vkCmdResetQueryPool, cb, qp, q, n); BEGIN(r_qry, E_vkCmdResetQueryPool, 0); r->qp = qp; r->q = q; r->n = n; END();
}
static void VKAPI_CALL e_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool qp, uint32_t q) {
    DEFERRED(vkCmdWriteTimestamp, cb, stage, qp, q); BEGIN(r_qry, E_vkCmdWriteTimestamp, 0); r->qp = qp; r->q = q; r->stage = stage; END();
}
static void VKAPI_CALL e_vkCmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool qp, uint32_t q, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize st, VkQueryResultFlags f) {
    DEFERRED(vkCmdCopyQueryPoolResults, cb, qp, q, n, b, o, st, f);
    BEGIN(r_cqr, E_vkCmdCopyQueryPoolResults, 0); r->qp = qp; r->q = q; r->n = n; r->b = b; r->o = o; r->st = st; r->f = f; END();
}

// ---------- the gated direct calls ----------
static VkResult VKAPI_CALL g_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { gate_cb(cb, W_BEGIN); return REAL(vkBeginCommandBuffer)(cb, bi); }
static VkResult VKAPI_CALL g_vkEndCommandBuffer(VkCommandBuffer cb) { gate_cb(cb, W_END); return REAL(vkEndCommandBuffer)(cb); }
static VkResult VKAPI_CALL g_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) { gate_cb(cb, W_RESET); return REAL(vkResetCommandBuffer)(cb, f); }
static VkResult VKAPI_CALL g_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { gate_key((uintptr_t)p, W_POOL); return REAL(vkResetCommandPool)(d, p, f); }
static VkResult VKAPI_CALL g_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    gate_key((uintptr_t)ai->commandPool, W_POOL);
    VkResult r = REAL(vkAllocateCommandBuffers)(d, ai, out);
    if (r == VK_SUCCESS) for (uint32_t i = 0; i < ai->commandBufferCount; i++) pmap_put(out[i], ai->commandPool);
    return r;
}
static VkResult VKAPI_CALL g_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    if (atomic_load_explicit(&g_live, memory_order_relaxed))
        for (uint32_t i = 0; i < n; i++) for (uint32_t k = 0; k < si[i].commandBufferCount; k++) gate_cb(si[i].pCommandBuffers[k], W_SUBMIT);
    return REAL(vkQueueSubmit)(q, n, si, f);
}
static VkResult VKAPI_CALL g_vkQueueWaitIdle(VkQueue q) { gate_all(W_IDLE); return REAL(vkQueueWaitIdle)(q); }
static VkResult VKAPI_CALL g_vkDeviceWaitIdle(VkDevice d) { gate_all(W_IDLE); return REAL(vkDeviceWaitIdle)(d); }
static VkResult VKAPI_CALL g_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) { atomic_fetch_add_explicit(&s_frames, 1, memory_order_relaxed); return REAL(vkQueuePresentKHR)(q, pi); }
#define VENC_DESTROY(f, T) static void VKAPI_CALL g_##f(VkDevice d, T o, const VkAllocationCallbacks *a) { gate_all(W_DESTROY); REAL(f)(d, o, a); }
VENC_DESTROY(vkDestroyBuffer, VkBuffer) VENC_DESTROY(vkDestroyBufferView, VkBufferView) VENC_DESTROY(vkDestroyDescriptorPool, VkDescriptorPool)
VENC_DESTROY(vkDestroyDescriptorSetLayout, VkDescriptorSetLayout) VENC_DESTROY(vkDestroyDescriptorUpdateTemplate, VkDescriptorUpdateTemplate)
VENC_DESTROY(vkDestroyFramebuffer, VkFramebuffer) VENC_DESTROY(vkDestroyImage, VkImage) VENC_DESTROY(vkDestroyImageView, VkImageView)
VENC_DESTROY(vkDestroyPipeline, VkPipeline) VENC_DESTROY(vkDestroyPipelineLayout, VkPipelineLayout) VENC_DESTROY(vkDestroyQueryPool, VkQueryPool)
VENC_DESTROY(vkDestroyRenderPass, VkRenderPass) VENC_DESTROY(vkDestroySampler, VkSampler) VENC_DESTROY(vkFreeMemory, VkDeviceMemory)
static void VKAPI_CALL g_vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { gate_all(W_DESTROY); pmap_forget_pool(p); REAL(vkDestroyCommandPool)(d, p, a); }
static void VKAPI_CALL g_vkDestroyDevice(VkDevice d, const VkAllocationCallbacks *a) { gate_all(W_DESTROY); REAL(vkDestroyDevice)(d, a); }
static void *wrap_named(const char *name, void *real);
static pthread_mutex_t g_wrap_mu = PTHREAD_MUTEX_INITIALIZER;
// vkCmd* entry points the engine fetches itself (the indirect-count draws): the wrapper; an unknown one turns the encoder off
static PFN_vkVoidFunction VKAPI_CALL g_vkGetDeviceProcAddr(VkDevice d, const char *n) {
    PFN_vkVoidFunction p = REAL(vkGetDeviceProcAddr)(d, n);
    if (!p || !n || strncmp(n, "vkCmd", 5)) return p;
    pthread_mutex_lock(&g_wrap_mu); void *w = wrap_named(n, (void *)p); pthread_mutex_unlock(&g_wrap_mu);
    return (PFN_vkVoidFunction)w;
}

// ---------- the encoder thread ----------
static void run(const hdr *h) {
#define C(T) const T *r = (const T *)h
    switch (h->id) {
    case E_vkCmdPipelineBarrier: { C(r_bar); R.vkCmdPipelineBarrier(r->cb, r->s, r->d, r->f, r->nm, r->nm ? r->m : NULL, r->nb, r->nb ? r->b : NULL, r->ni, r->ni ? r->i : NULL); break; }
    case E_vkCmdCopyBuffer: { C(r_cpb); R.vkCmdCopyBuffer(r->cb, r->s, r->d, r->n, r->r); break; }
    case E_vkCmdFillBuffer: { C(r_fill); R.vkCmdFillBuffer(r->cb, r->d, r->o, r->n, r->v); break; }
    case E_vkCmdCopyBufferToImage: { C(r_b2i); R.vkCmdCopyBufferToImage(r->cb, r->b, r->im, r->l, r->n, r->r); break; }
    case E_vkCmdCopyImageToBuffer: { C(r_b2i); R.vkCmdCopyImageToBuffer(r->cb, r->im, r->l, r->b, r->n, r->r); break; }
    case E_vkCmdCopyImage: { C(r_i2i); R.vkCmdCopyImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r); break; }
    case E_vkCmdBlitImage: { C(r_i2i); R.vkCmdBlitImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r, r->f); break; }
    case E_vkCmdResolveImage: { C(r_i2i); R.vkCmdResolveImage(r->cb, r->s, r->sl, r->d, r->dl, r->n, r->r); break; }
    case E_vkCmdClearColorImage: { C(r_clr); R.vkCmdClearColorImage(r->cb, r->im, r->l, &r->v.color, r->n, r->r); break; }
    case E_vkCmdClearDepthStencilImage: { C(r_clr); R.vkCmdClearDepthStencilImage(r->cb, r->im, r->l, &r->v.depthStencil, r->n, r->r); break; }
    case E_vkCmdClearAttachments: { C(r_clra); R.vkCmdClearAttachments(r->cb, r->na, r->a, r->nr, r->r); break; }
    case E_vkCmdBeginRenderPass: { C(r_rpb); VkRenderPassBeginInfo rb = r->rb; if (r->ab.sType) rb.pNext = &r->ab; R.vkCmdBeginRenderPass(r->cb, &rb, r->sc); break; }
    case E_vkCmdEndRenderPass: { C(r_none); R.vkCmdEndRenderPass(r->cb); break; }
    case E_vkCmdBindPipeline: { C(r_pipe); R.vkCmdBindPipeline(r->cb, r->bp, r->p); break; }
    case E_vkCmdBindDescriptorSets: { C(r_sets); R.vkCmdBindDescriptorSets(r->cb, r->bp, r->l, r->first, r->n, r->s, r->nd, r->d); break; }
    case E_vkCmdBindVertexBuffers: { C(r_vb); R.vkCmdBindVertexBuffers(r->cb, r->first, r->n, r->b, r->o); break; }
    case E_vkCmdBindIndexBuffer: { C(r_ib); R.vkCmdBindIndexBuffer(r->cb, r->b, r->o, r->t); break; }
    case E_vkCmdDraw: { C(r_draw); R.vkCmdDraw(r->cb, r->a, r->b, r->c, r->d); break; }
    case E_vkCmdDrawIndexed: { C(r_draw); R.vkCmdDrawIndexed(r->cb, r->a, r->b, r->c, r->e, r->f); break; }
    case E_vkCmdDrawIndirect: { C(r_ind); R.vkCmdDrawIndirect(r->cb, r->b, r->o, r->n, r->st); break; }
    case E_vkCmdDrawIndexedIndirect: { C(r_ind); R.vkCmdDrawIndexedIndirect(r->cb, r->b, r->o, r->n, r->st); break; }
    case E_vkCmdDrawIndirectCount: { C(r_indc); R.vkCmdDrawIndirectCount(r->cb, r->b, r->o, r->cb2, r->o2, r->n, r->st); break; }
    case E_vkCmdDrawIndexedIndirectCount: { C(r_indc); R.vkCmdDrawIndexedIndirectCount(r->cb, r->b, r->o, r->cb2, r->o2, r->n, r->st); break; }
    case E_vkCmdDispatch: { C(r_draw); R.vkCmdDispatch(r->cb, r->a, r->b, r->c); break; }
    case E_vkCmdDispatchIndirect: { C(r_ind); R.vkCmdDispatchIndirect(r->cb, r->b, r->o); break; }
    case E_vkCmdSetViewport: { C(r_vps); R.vkCmdSetViewport(r->cb, r->first, r->n, (const VkViewport *)r->p); break; }
    case E_vkCmdSetScissor: { C(r_vps); R.vkCmdSetScissor(r->cb, r->first, r->n, (const VkRect2D *)r->p); break; }
    case E_vkCmdPushConstants: { C(r_pc); R.vkCmdPushConstants(r->cb, r->l, r->st, r->o, r->n, r->p); break; }
    case E_vkCmdBeginQuery: { C(r_qry); R.vkCmdBeginQuery(r->cb, r->qp, r->q, r->f); break; }
    case E_vkCmdEndQuery: { C(r_qry); R.vkCmdEndQuery(r->cb, r->qp, r->q); break; }
    case E_vkCmdResetQueryPool: { C(r_qry); R.vkCmdResetQueryPool(r->cb, r->qp, r->q, r->n); break; }
    case E_vkCmdWriteTimestamp: { C(r_qry); R.vkCmdWriteTimestamp(r->cb, r->stage, r->qp, r->q); break; }
    case E_vkCmdCopyQueryPoolResults: { C(r_cqr); R.vkCmdCopyQueryPoolResults(r->cb, r->qp, r->q, r->n, r->b, r->o, r->st, r->f); break; }
    default: { static int nr; if (nr++ < 4) LOG("NVN venc: unknown record %u (size %u) skipped", h->id, h->size); break; }
    }
#undef C
}
static pthread_t g_enc_thread; static _Atomic int g_enc_tid;
static uint32_t g_enc_want;   // the mask dm-venc should have (0 = none set)
// Raw syscalls (as hle_cpu_place.c): the build force-includes a header that pulls in <sched.h> before _GNU_SOURCE, so no cpu_set_t
static int aff_get(int tid, uint32_t *m) { uint64_t s[16]; memset(s, 0, sizeof s); if (syscall(SYS_sched_getaffinity, tid, sizeof s, s) < 0) return -1; *m = (uint32_t)s[0]; return 0; }
static int aff_set(int tid, uint32_t m) { uint64_t s[16]; memset(s, 0, sizeof s); s[0] = m; return syscall(SYS_sched_setaffinity, tid, sizeof s, s) < 0 ? -1 : 0; }
static int g_prod_tid;   // the translation thread (the first to open a window)
static const char *enc_place(uint32_t *want) {
    uint32_t all = 0; aff_get(getpid(), &all); if (!all) all = 0xffffffffu;
    if (g_cpus_env) { *want = g_cpus_env & all ? g_cpus_env & all : g_cpus_env; return "DM_GFX_VENC_CPUS"; }
#ifdef VENC_HAVE_CPU_PLAN
    if (dm_cpu_plan) { const dm_cpu_plan_t *p = dm_cpu_plan(); if (p && p->record && (p->record & all)) { *want = p->record & all; return "cpu plan (record: DOOM 2016's dm-record core)"; } }
#endif
    uint32_t x = 0; if (g_prod_tid) aff_get(g_prod_tid, &x);
    if (x && (all & ~x)) { *want = all & ~x; return "every CPU but the translation thread's"; }
    *want = all; return "every CPU";
}
static void *enc_main(void *arg) {
    (void)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "dm-venc");
#endif
    int tid = (int)syscall(SYS_gettid); atomic_store(&g_enc_tid, tid);
    int pr = setpriority(PRIO_PROCESS, (id_t)tid, -8);
    uint32_t was = 0, want = 0; aff_get(tid, &was); const char *why = enc_place(&want);
    int ar = want ? aff_set(tid, want) : 0; g_enc_want = want;
    char cw[16], cn[16]; snprintf(cw, sizeof cw, "%#x", was); snprintf(cn, sizeof cn, "%#x", want);
    LOG("NVN venc thread dm-venc tid=%d nice=-8 (setpriority %d) cpus %s -> %s (%s) result=%d; ring %llu KB, batch %u, tail %u, spin %llu us",
        tid, pr, cw, cn, why, ar, (unsigned long long)(g_rsz >> 10), g_batch, g_tail_every, (unsigned long long)(g_spin_ns / 1000));
    uint64_t t = 0, tpub = 0;
    for (;;) {
        uint64_t h = atomic_load_explicit(&g_head, memory_order_acquire);
        if (t == h) {   // empty: spin while the translation thread records (its next batch comes within microseconds), then sleep
            for (uint64_t t0 = mono_ns();;) {
                h = atomic_load_explicit(&g_head, memory_order_acquire); if (h != t) break;
                if (spin_over(t0)) break;
                wait_change(&g_head, t);
            }
            if (h == t) {
                uint32_t s = atomic_load(&g_cseq);   // an event count, as wait_tail's
                atomic_store(&g_csleep, 1);
                atomic_thread_fence(memory_order_seq_cst);
                if (atomic_load(&g_head) == t) { fwait(&g_cseq, s); atomic_fetch_add_explicit(&s_sleeps, 1, memory_order_relaxed); }
                atomic_store(&g_csleep, 0);
                continue;
            }
        }
        uint64_t b0 = mono_ns(), n = 0;
        while (t != h) {
            const hdr *r = (const hdr *)(g_ring + (t & g_rmask));
            if (r->id != E_WRAP) { run(r); n++; }
            t += r->size;
            if (t == h || t - tpub >= g_tail_every || atomic_load_explicit(&g_pwait, memory_order_relaxed) || atomic_load_explicit(&g_fwait, memory_order_relaxed)) {
                tpub = t;
                atomic_store(&g_tail, t);
                atomic_thread_fence(memory_order_seq_cst);   // the tail before the producer's wait flag is read
                if (atomic_load(&g_pwait) && t >= atomic_load(&g_pwant)) { atomic_fetch_add(&g_pseq, 1); fwake(&g_pseq); }
                if (t == h) h = atomic_load_explicit(&g_head, memory_order_acquire);   // more came meanwhile
            }
        }
        atomic_fetch_add_explicit(&s_busy_ns, mono_ns() - b0, memory_order_relaxed); atomic_fetch_add_explicit(&s_recs, n, memory_order_relaxed);
    }
    return NULL;
}
static int g_up;   // 1 running, -1 failed
static int up(void) {
    if (g_up) return g_up > 0;
    g_ring = (uint8_t *)malloc(g_rsz);
    if (g_ring) { pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10);
        if (pthread_create(&g_enc_thread, &at, enc_main, NULL)) { free(g_ring); g_ring = NULL; } else pthread_detach(g_enc_thread);
        pthread_attr_destroy(&at); }
    if (!g_ring) {
        g_up = -1; atomic_store(&g_mode, 0);
        LOG("NVN venc: cannot start dm-venc (ring %llu KB): the translation thread records directly", (unsigned long long)(g_rsz >> 10));
        if (pc_note) pc_note("vk-enc-thread", 0, "dm-venc could not start: every call direct");
        return 0;
    }
    g_up = 1; atomic_store(&g_live, 1);
    if (pc_note) pc_note("vk-enc-thread", 1, "%s, mode %d: the translation thread's vkCmd* go to dm-venc in batches of %u bytes; PNH-A-01", g_mode_why, atomic_load(&g_mode), g_batch);
    return 1;
}

// ---------- the window ----------
static uint64_t g_log_frames, g_log_t0; static int64_t g_log_pcpu, g_log_ecpu;
static int64_t thread_cpu_ns(clockid_t c) { struct timespec t; if (clock_gettime(c, &t)) return -1; return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void log_window(void) {
    uint64_t fr = atomic_load_explicit(&s_frames, memory_order_relaxed);
    if (!g_log_t0) { g_log_t0 = mono_ns(); g_log_frames = fr; g_log_pcpu = thread_cpu_ns(CLOCK_THREAD_CPUTIME_ID); return; }
    if (fr - g_log_frames < 300) return;
    const double f = (double)(fr - g_log_frames);
    int64_t pc = thread_cpu_ns(CLOCK_THREAD_CPUTIME_ID), ec = -1; clockid_t cid;
    if (g_up > 0 && !pthread_getcpuclockid(g_enc_thread, &cid)) ec = thread_cpu_ns(cid);
    double ecpu = ec >= 0 && g_log_ecpu ? (double)(ec - g_log_ecpu) / 1e6 / f : -1;
    uint64_t sn = 0, sns = 0; for (int i = 0; i < W_N; i++) { sn += s_sync_n[i]; sns += s_sync_ns[i]; }
    char why[256]; int at = 0; why[0] = 0;
    for (int i = 0; i < W_N && at < (int)sizeof why - 32; i++) if (s_sync_n[i]) at += snprintf(why + at, sizeof why - at, " %s=%.2f/%.3f", w_name[i], s_sync_n[i] / f, s_sync_ns[i] / 1e6 / f);
    char fwhy[160]; int fat = 0; fwhy[0] = 0;
    for (int i = 0; i < W_N && fat < (int)sizeof fwhy - 24; i++) { uint64_t v = atomic_exchange(&s_fn_why[i], 0); if (v) fat += snprintf(fwhy + fat, sizeof fwhy - fat, " %s=%.2f", w_name[i], v / f); }
    uint64_t busy = atomic_exchange(&s_busy_ns, 0), recs = atomic_exchange(&s_recs, 0), sl = atomic_exchange(&s_sleeps, 0), fn = atomic_exchange(&s_fn, 0), fns = atomic_exchange(&s_fns, 0);
    LOG("NVN PERF venc frames=%.0f mode=%d xlat_ms=%.3f xlat_thread_cpu_ms=%.3f enc_ms=%.3f enc_cpu_ms=%.3f calls=%.1f recs=%.1f kb=%.1f pubs=%.1f wakes=%.2f enc_sleeps=%.2f wins=%.2f "
        "waits=%.2f wait_ms=%.3f [%s ] ring_full=%.2f/%.3f fallback=%.2f foreign=%.2f/%.3f [%s ] unpub=%llu",
        f, atomic_load(&g_mode), s_win_ns / 1e6 / f, pc >= 0 && g_log_pcpu >= 0 ? (double)(pc - g_log_pcpu) / 1e6 / f : -1.0, busy / 1e6 / f, ecpu,
        s_calls / f, recs / f, s_bytes / 1024.0 / f, s_pubs / f, s_wakes / f, sl / f, s_wins / f, sn / f, sns / 1e6 / f, why,
        s_full / f, s_full_ns / 1e6 / f, s_fallbacks / f, fn / f, fns / 1e6 / f, fwhy, (unsigned long long)atomic_load(&s_unpub));
    s_calls = s_bytes = s_pubs = s_wakes = s_wins = s_win_ns = s_full = s_full_ns = s_fallbacks = 0; memset(s_sync_n, 0, sizeof s_sync_n); memset(s_sync_ns, 0, sizeof s_sync_ns);
    g_log_frames = fr; g_log_t0 = mono_ns(); g_log_pcpu = pc; g_log_ecpu = ec;
    // the system / vendor boosts rewrite thread masks: put dm-venc back (first 4 logged)
    int et = atomic_load(&g_enc_tid); uint32_t m = 0;
    if (et && g_enc_want && !aff_get(et, &m) && m != g_enc_want) { int r = aff_set(et, g_enc_want); static int nr; if (nr++ < 4) LOG("NVN venc thread tid=%d cpus found %#x, set %#x again (result %d)", et, m, g_enc_want, r); }
}
void dm_venc_window_begin(void) {
    if (atomic_load_explicit(&g_mode, memory_order_relaxed) <= 0) return;
    uintptr_t me = tp();
    if (atomic_load_explicit(&g_win_tp, memory_order_relaxed) == me) { g_win_depth++; return; }
    if (!g_prod_tid) g_prod_tid = (int)syscall(SYS_gettid);
    if (!atomic_load_explicit(&g_live, memory_order_relaxed) && !up()) return;
    // The producer state moves with the engine lock (any thread may open a window: DM_NVN_SYNC=1 translates on the game's threads).
    g_tc = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (g_tc == g_ph) touch_clear();
    g_win_depth = 1; g_win_t0 = mono_ns(); s_wins++;
    atomic_store_explicit(&g_win_tp, me, memory_order_relaxed);
}
void dm_venc_window_end(void) {
    if (atomic_load_explicit(&g_win_tp, memory_order_relaxed) != tp()) return;
    if (--g_win_depth > 0) return;
    if (atomic_load_explicit(&g_mode, memory_order_relaxed) == 2) psync(W_WINEND);
    else if (g_pub != g_ph) publish_now();
    s_win_ns += mono_ns() - g_win_t0;
    atomic_store_explicit(&g_win_tp, 0, memory_order_release);
    log_window();
}

// ---------- resolving ----------
typedef struct { const char *name; void *wrapper; void **real; } wrap_t;
#define VENC_WCMD(f) {#f, (void *)e_##f, (void **)&R.f},
#define VENC_WGATE(f) {#f, (void *)g_##f, (void **)&R.f},
static const wrap_t g_wraps[] = { VENC_CMDS(VENC_WCMD) VENC_GATED(VENC_WGATE)
    {"vkCmdDrawIndirectCountKHR", (void *)e_vkCmdDrawIndirectCount, (void **)&R.vkCmdDrawIndirectCount},
    {"vkCmdDrawIndexedIndirectCountKHR", (void *)e_vkCmdDrawIndexedIndirectCount, (void **)&R.vkCmdDrawIndexedIndirectCount} };
static void *wrap_named(const char *name, void *real) {
    for (size_t i = 0; i < sizeof g_wraps / sizeof g_wraps[0]; i++)
        if (!strcmp(g_wraps[i].name, name)) { __atomic_store_n(g_wraps[i].real, real, __ATOMIC_RELEASE); return g_wraps[i].wrapper; }
    if (!strncmp(name, "vkCmd", 5) && atomic_load(&g_mode) > 0) {   // a command the encoder cannot make: it would race the encoder on the command buffer
        atomic_store(&g_mode, 0);
        LOG("NVN venc: %s has no record of its own: the encoder stays off (every call direct)%s", name, atomic_load(&g_live) ? " from the next window" : "");
        if (pc_note) pc_note("vk-enc-thread", 0, "%s not handled by the encoder", name);
    }
    return real;
}
void *dm_venc_wrap(unsigned slot, const char *name, void *real) {
    (void)slot;
    cfg_once();
    if (atomic_load_explicit(&g_mode, memory_order_relaxed) <= 0 || !real || !name) {
        if (!atomic_load(&g_live)) { static int told; if (!told++ && pc_note) pc_note("vk-enc-thread", 0, "%s", g_mode_why); }
        return real;
    }
    pthread_mutex_lock(&g_wrap_mu); void *w = wrap_named(name, real); pthread_mutex_unlock(&g_wrap_mu);
    return w;
}
