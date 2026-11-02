// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

// 页状态与 mprotect 的次序（改保护的一方先把状态改成 BUSY，mprotect 落地后才改成终态；别的线程见 BUSY 就等）：
//   设只读：NONE → BUSY → mprotect(只读) → PROT；解保护：PROT → BUSY → mprotect(可写) → NONE。
// 不变式：状态是 NONE 的页一定可写、是 PROT 的页一定只读。原先设只读是「先记 PROT、攒一段再 mprotect」，这段时间里另一线程的写前解保护
// 会把状态改回 NONE，随后攒批的 mprotect 落地 ⇒ 页只读而状态 NONE：游戏线程写它时缺页处理只回「重执行」永远空转；
// 宿主用系统调用往里读文件时 prep_write 看状态 NONE 跳过，系统调用不走缺页处理，直接 EFAULT、数据读不全。
// BUSY 最长持续 = 一次 mprotect 的时间（设只读时再加上给这一段逐页改状态的几次原子写）。等待有上限 BUSY_WAIT_MS，超限报红：
// 缺页处理里超限按「不是我们的页」返回，交给崩溃处理如实报崩，不静默空转。等待只读原子量、不拿任何锁。
// Order of page state and mprotect (the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY):
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, and one whose state is PROT is always read-only. Setting read-only used to be "record PROT first, batch up a run, then mprotect"; during that time another thread's unprotect-before-write
// could set the state back to NONE, and then the batched mprotect landed => the page read-only with state NONE: when the game thread wrote to it the page-fault handler only returned "re-execute" and spun forever;
// when the host read a file into it with a system call, prep_write saw state NONE and skipped, the system call did not go through the page-fault handler and failed with EFAULT, leaving the data incomplete.
// BUSY lasts at most one mprotect (plus, when setting read-only, the few atomic writes that change the state of the pages in the run). The wait has an upper limit BUSY_WAIT_MS and reports red beyond it:
// in the page-fault handler, exceeding it returns "not our page" and lets the crash handler report the crash truthfully instead of spinning silently. The wait only reads atomics and takes no lock.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 4096   // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing
typedef struct { uintptr_t base, end; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; } range_t;
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
// [D3] Kernel VMA budget (vm.max_map_count, 65530 on Android). Every RO / RW boundary inside a pool is a VMA: a write fault in the
// middle of a read-only run splits it into three (+2 VMAs), and a page whose texture is never checked again stays a hole for good.
// D3 died after ~7 min (55k write faults) with an mprotect ENOMEM in MapPhysicalMemory. Hence:
//  - g_blk: a write fault unprotects the protected pages of the whole aligned block around the page (DM_DIRTY_UNPROT_KB, D3 default
//    64 KiB, elsewhere one host page): sequential streaming writes fault once per block, and holes are block sized;
//  - mprotect failures are no longer ignored: a failed unprotect retries over the page's whole read-only run (one VMA when it is
//    ours alone, no split needed), a failed protect leaves the pages writable and marks them always dirty (GEN_STICKY) so nothing
//    reads stale; either sets g_pressure;
//  - nvn_dirty_relieve: unprotect every protected run with one mprotect each (neighbours with equal protection merge back into one
//    VMA), marking those pages written; run on g_pressure, by the VMA watch (hle_vma.c) near the limit, and by the SVCs on ENOMEM.
static uintptr_t g_blk = 1;                          // pages unprotected per write fault (aligned block)
static _Atomic int g_pressure;                       // an mprotect of ours failed: relieve from a normal thread
static _Atomic uint64_t g_mp_fail, g_blk_extra, g_relieves, g_relieved_pages, g_sticky;
static _Atomic int g_mp_errno;
#define GEN_STICKY UINT64_MAX                        // page left writable after a failed protect: dirty for every stamp
static int mp(uintptr_t a, size_t len, int prot) {   // 0 or errno; errno itself is left as it was (signal handler)
    int saved = errno;
    uint64_t t = mach_absolute_time(); int rc = mprotect((void *)a, len, prot), err = rc ? errno : 0;
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (err) { atomic_fetch_add(&g_mp_fail, 1); atomic_store(&g_mp_errno, err); atomic_store(&g_pressure, 1); }
    errno = saved; return err;
}

static mach_timebase_info_data_t g_tb;   // 首次登记池时取好（缺页路径里只做乘除） | Obtained when a pool is first registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// 等这一页离开 BUSY。返回 1 = 已离开；0 = 等过了上限（报红一次）。信号处理里可用：只有原子读、mach_absolute_time、sched_yield
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("【红】脏页跟踪：页 %p 停在「忙」超过 %d 毫秒（改保护的线程没走完），不再等", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static range_t *find(uintptr_t a) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (atomic_load_explicit(&g_r[i].live, memory_order_acquire) && a >= g_r[i].base && a < g_r[i].end) return &g_r[i];
    return NULL;
}
static uintptr_t range_pages(const range_t *r) { return (r->end - r->base) >> g_shift; }
// Grow the run [*a, *b) of pages this thread holds BUSY over neighbouring PROT pages within [lo, hi) (CAS PROT -> BUSY; stops at
// the first page that is not PROT, so the run stays contiguous and one mprotect covers it). Signal-safe.
static void take_run(range_t *r, uintptr_t *a, uintptr_t *b, uintptr_t lo, uintptr_t hi) {
    while (*a > lo) { uint8_t st = PG_PROT; if (!atomic_compare_exchange_strong(&r->state[*a - 1], &st, PG_BUSY)) break; --*a; }
    while (*b < hi) { uint8_t st = PG_PROT; if (!atomic_compare_exchange_strong(&r->state[*b], &st, PG_BUSY)) break; ++*b; }
}
// Pages [a, b) are BUSY (taken from PROT by the caller): make them writable, stamp them written, state NONE. 0 or errno (then the
// pages are untouched: still read-only and BUSY, the caller decides). Signal-safe.
static int unprotect_run(range_t *r, uintptr_t a, uintptr_t b) {
    int err = mp(r->base + (a << g_shift), (b - a) << g_shift, PROT_READ | PROT_WRITE);
    if (err) return err;
    uint64_t g = atomic_fetch_add(&g_now, 1) + 1;
    for (uintptr_t k = a; k < b; k++) { atomic_store(&r->gen[k], g); atomic_store(&r->state[k], PG_NONE); }
    return 0;
}
static void restore_prot(range_t *r, uintptr_t a, uintptr_t b) { for (uintptr_t k = a; k < b; k++) atomic_store(&r->state[k], PG_PROT); }   // after a failed unprotect: still read-only
// Unprotect page p (BUSY, taken by the caller) together with the protected pages of its aligned g_blk block. When that mprotect
// fails (ENOMEM: the split needs a VMA beyond vm.max_map_count) the whole protected run around it is unprotected instead: a run
// that is one read-only VMA of its own needs no split, and it merges with its writable neighbours (-2 VMAs). 0 or errno.
static int unprotect_around(range_t *r, uintptr_t p) {
    uintptr_t a = p, b = p + 1, np = range_pages(r);
    if (g_blk > 1) {
        uintptr_t base_pg = r->base >> g_shift, abs = base_pg + p, blo = abs - abs % g_blk;
        uintptr_t lo = blo > base_pg ? blo - base_pg : 0, hi = blo + g_blk - base_pg; if (hi > np) hi = np;
        take_run(r, &a, &b, lo, hi);
    }
    int err = unprotect_run(r, a, b);
    if (err) { take_run(r, &a, &b, 0, np); err = unprotect_run(r, a, b); }
    if (err) { restore_prot(r, a, b); return err; }
    if (b - a > 1) atomic_fetch_add(&g_blk_extra, b - a - 1);
    return 0;
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    // A tracked page is never less than readable, so its faults are writes: is_write is not needed (and is always 0 where
    // the ucontext carries no ESR, e.g. LDPlayer's ARM translation on x86, which left such writes unhandled -> SIGSEGV)
    (void)is_write;
    range_t *r = find((uintptr_t)addr);
    if (!r) return 0;
    uintptr_t p = ((uintptr_t)addr - r->base) >> g_shift;
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) {
        if (unprotect_around(r, p)) return 0;   // not writable even without a split: let the crash path report it (the [vma] lines carry the count)
        atomic_fetch_add(&g_faults, 1); return 1; }
    if (st == PG_BUSY) return busy_wait(r, p);   // 别的线程正在改这页的保护：等它改完重执行（改成了只读就再缺页一次、走上面那一支） | Another thread is changing this page's protection: wait for it to finish and re-execute (if it became read-only, it faults once more and takes the branch above)
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（本次缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (this fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
// Host write ahead: the protected pages of [addr, addr+len) are unprotected in contiguous runs (one mprotect per run instead of one
// per page). A run is flushed before waiting on a page another thread holds BUSY, so this never waits while holding pages.
static void prep_flush(range_t *r, uintptr_t a, uintptr_t b) {
    if (!unprotect_run(r, a, b)) { atomic_fetch_add(&g_preps, b - a); return; }
    uintptr_t a2 = a, b2 = b; take_run(r, &a2, &b2, 0, range_pages(r));   // ENOMEM: the whole protected run, no split (see unprotect_around)
    if (!unprotect_run(r, a2, b2)) { atomic_fetch_add(&g_preps, b2 - a2); return; }
    restore_prot(r, a2, b2);
    static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("【红】[vma] dirty tracker: host-write unprotect of %p+%#lx failed errno=%d (VMA limit?): that host write may get EFAULT", (void *)(r->base + (a << g_shift)), (unsigned long)((b - a) << g_shift), atomic_load(&g_mp_errno));
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    while (len && a < e) {
        range_t *r = find(a);
        if (!r) { a = (a + g_pg) & ~(g_pg - 1); continue; }
        uintptr_t stop = e < r->end ? e : r->end, run = 0; int open = 0;
        for (uintptr_t p = (a - r->base) >> g_shift, q = (stop - 1 - r->base) >> g_shift; p <= q + 1; p++) {
            int took = 0;
            if (p <= q) for (;;) { uint8_t st = PG_PROT;   // 见 BUSY 等完要重看：对方可能是在设只读，等完这页是 PROT，照样得解 | After waiting out BUSY, look again: the other side may have been setting read-only, in which case the page is PROT after the wait and still has to be unprotected
                if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { took = 1; break; }
                if (st != PG_BUSY) break;
                if (open) { prep_flush(r, run, p); open = 0; }   // never wait while holding pages
                if (!busy_wait(r, p)) break; }
            if (took && !open) { run = p; open = 1; }
            if (!took && open) { prep_flush(r, run, p); open = 0; }
        }
        a = stop;
    }
}

// Write-protect tracking needs a resumable SIGSEGV: under an ARM-on-x86 native bridge (LDPlayer's libhoudini) a handled fault
// resumes the guest at a wrong pc, so there pools stay untracked (treated as always dirty). DM_WRITE_PROTECT=1 / 0 forces it.
int nvn_dirty_disabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_WRITE_PROTECT"); int off = 0;
        if (e && *e) off = *e == '0';
#ifdef __ANDROID__
        else { char b[PROP_VALUE_MAX] = ""; __system_property_get("ro.dalvik.vm.native.bridge", b); off = b[0] && strcmp(b, "0") != 0; }
#endif
        if (off) LOG("[nvn] write-protect tracking off (%s): dirty pages, wtrack and the vertex cache copy every use", e && *e ? "DM_WRITE_PROTECT=0" : "native bridge");
        v = off;
    }
    return v;
}
void nvn_dirty_add_range(const void *mem, size_t size) {
    if (nvn_dirty_disabled()) return;
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write);
        const char *e = getenv("DM_DIRTY_UNPROT_KB");
#ifdef DM_GAME_D3
        long kb = e && *e ? atol(e) : 64;   // [D3] block-granular unprotect on by default (VMA budget, see g_blk)
#else
        long kb = e && *e ? atol(e) : 0;
#endif
        g_blk = kb > 0 ? ((uintptr_t)kb << 10) / g_pg : 1; if (g_blk < 1) g_blk = 1;
        if (g_blk & (g_blk - 1)) { uintptr_t b = 1; while (b * 2 <= g_blk) b *= 2; g_blk = b; }
        LOG("[vma] dirty tracker: a write fault unprotects %lu KiB blocks (DM_DIRTY_UNPROT_KB, page %lu KiB)", (unsigned long)((g_blk * g_pg) >> 10), (unsigned long)(g_pg >> 10)); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->state = state; r->gen = gen;
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = find((uintptr_t)mem);
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    range_t *r = find((uintptr_t)addr); return r && (uintptr_t)addr + len <= r->end;
}
// 设只读：先把页标成 BUSY，mprotect 落地后才标 PROT（次序与不变式见文件头的状态说明）
// Set read-only: mark the page BUSY first, and mark it PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    if (!len || !nvn_dirty_tracked(addr, len)) return;
    if (atomic_load(&g_pressure)) nvn_dirty_relieve("mprotect failed");
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在改的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls ran to over a thousand per frame when large textures were first uploaded; character select, startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break a run: they are not protected again, and pages another thread is changing are left alone
    // A failed mprotect (ENOMEM at the VMA limit) leaves the run writable: those pages are stamped GEN_STICKY (dirty for every stamp,
    // so their users re-check instead of trusting stale copies) and g_now moves on so the checks run; the next call relieves first.
    // A sticky page that is protected later gets the current generation back (<= the stamp its caller takes after this call).
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            if (!mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ)) {
                uint64_t now = atomic_load(&g_now);
                for (uintptr_t k = run; k < i; k++) { if (atomic_load(&r->gen[k]) == GEN_STICKY) atomic_store(&r->gen[k], now); atomic_store(&r->state[k], PG_PROT); }
            } else {
                for (uintptr_t k = run; k < i; k++) { atomic_store(&r->gen[k], GEN_STICKY); atomic_store(&r->state[k], PG_NONE); }
                atomic_fetch_add(&g_now, 1); atomic_fetch_add(&g_sticky, i - run);
                static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("【红】[vma] dirty tracker: protect of %p+%#lx failed errno=%d (VMA limit?): pages stay writable and count as always dirty; relieving", (void *)(r->base + (run << g_shift)), (unsigned long)((i - run) << g_shift), atomic_load(&g_mp_errno));
            }
            open = 0; }
    }
}
// Give the kernel its VMAs back: every protected run of every live pool becomes writable with one mprotect (no split: the run is
// its own read-only VMA, so it merges with its writable neighbours), and its pages count as written (the trackers re-check and
// protect again on their next use). Pages another thread holds BUSY are skipped; nothing here waits on a page. Normal threads only.
// Returns the pages unprotected.
static pthread_mutex_t g_relieve_mu = PTHREAD_MUTEX_INITIALIZER;
int dm_vma_count_now(void) __attribute__((weak));   // hle_vma.c: lines of /proc/self/maps (-1 where there is none)
uint64_t nvn_dirty_relieve(const char *why) {
    if (pthread_mutex_trylock(&g_relieve_mu)) { pthread_mutex_lock(&g_relieve_mu); pthread_mutex_unlock(&g_relieve_mu); return 0; }   // another thread is at it: wait for it
    pthread_mutex_lock(&g_slot_mu);   // no pool comes or goes meanwhile (callers never hold g_slot_mu)
    atomic_store(&g_pressure, 0);
    int before = dm_vma_count_now ? dm_vma_count_now() : -1;
    uint64_t runs = 0, pages = 0, fails = 0; uint64_t t0 = mach_absolute_time();
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!atomic_load_explicit(&r->live, memory_order_acquire)) continue;
        uintptr_t np = range_pages(r);
        for (uintptr_t p = 0; p < np; p++) {
            uint8_t st = PG_PROT;
            if (!atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) continue;
            uintptr_t a = p, b = p + 1; take_run(r, &a, &b, p, np);
            if (unprotect_run(r, a, b)) { restore_prot(r, a, b); fails++; } else { runs++; pages += b - a; }
            p = b - 1;
        }
    }
    atomic_store(&g_pressure, 0);   // our own failures above must not re-trigger at once
    pthread_mutex_unlock(&g_slot_mu);
    atomic_fetch_add(&g_relieves, 1); atomic_fetch_add(&g_relieved_pages, pages);
    int after = dm_vma_count_now ? dm_vma_count_now() : -1;
    uint64_t ms = (mach_absolute_time() - t0) * g_tb.numer / (g_tb.denom ? g_tb.denom : 1) / 1000000;
    LOG("%s[vma] dirty tracker relieved (%s): %llu protected runs / %llu pages (%.1f MB) made writable in %llu ms, %llu failed; maps %d -> %d",
        fails ? "【红】" : "", why, (unsigned long long)runs, (unsigned long long)pages, (double)(pages << g_shift) / 1048576.0, (unsigned long long)ms, (unsigned long long)fails, before, after);
    pthread_mutex_unlock(&g_relieve_mu);
    return pages;
}
void nvn_dirty_vma_stats(uint64_t *mp_fail, uint64_t *blk_extra, uint64_t *relieves, uint64_t *relieved_pages, uint64_t *sticky, int *blk_kb) {
    *mp_fail = atomic_load(&g_mp_fail); *blk_extra = atomic_load(&g_blk_extra); *relieves = atomic_load(&g_relieves);
    *relieved_pages = atomic_load(&g_relieved_pages); *sticky = atomic_load(&g_sticky); *blk_kb = (int)((g_blk * (g_pg ? g_pg : 4096)) >> 10);
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    if (!nvn_dirty_tracked(addr, len)) return 1;   // 未跟踪时保守视为脏，宁可多传也不显示旧内容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    range_t *r = find((uintptr_t)addr);
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
// [DOOM] Writer/reader exclusion for CPU texel writes vs. the renderer's partial re-upload (see nvn_dirty.h). Without it a
// re-upload can race a WriteTexels on another thread: it protects and reads while the writer is mid-memcpy (torn page for
// one submission), and a page that was BUSY during protect stays writable with gen <= the new stamp, so the rest of that
// write is never re-uploaded. One plain mutex: WriteTexels calls are short (one VT page) and uncontended almost always.
static pthread_mutex_t g_texel_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_texel_on = -1;
int nvn_texel_sync_on(void) { if (g_texel_on < 0) { const char *e = getenv("DM_GFX_VT_ATOMIC"); g_texel_on = e && atoi(e) ? 1 : 0; LOG("[nvn] DM_GFX_VT_ATOMIC=%d (WriteTexels vs partial re-upload exclusion)", g_texel_on); } return g_texel_on; }
void nvn_texel_lock(void) { pthread_mutex_lock(&g_texel_mu); }
void nvn_texel_unlock(void) { pthread_mutex_unlock(&g_texel_mu); }
// [ETERNAL] crash report: every tracked range (live or not) that holds addr, with the page's state and stamp
void nvn_dirty_describe(const void *addr) {
    uintptr_t a = (uintptr_t)addr; int n = atomic_load(&g_nr), shown = 0;
    for (int i = 0; i < n; i++) if (a >= g_r[i].base && a < g_r[i].end) {
        uintptr_t p = (a - g_r[i].base) >> g_shift; shown++;
        fprintf(stderr, "[dm]   dirty range %d %#lx-%#lx live %d: page state %u gen %llu\n", i, (unsigned long)g_r[i].base, (unsigned long)g_r[i].end,
                (int)atomic_load(&g_r[i].live), (unsigned)atomic_load(&g_r[i].state[p]), (unsigned long long)atomic_load(&g_r[i].gen[p]));
    }
    if (!shown) fprintf(stderr, "[dm]   no dirty-tracker range holds %p (%d ranges)\n", addr, n);
}
