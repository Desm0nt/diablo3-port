// [D3] VMA watch (kernel memory-map entries, vm.max_map_count = 65530 on Android). Every mprotect of part of a mapping splits it,
// and once the process holds max_map_count VMAs any further split fails with ENOMEM: D3 aborted after ~7 min of play in
// MapPhysicalMemory (FATAL SVC 0x2c mprotect errno=12) with 55k write-protect faults behind it (nvn_dirty.c texture / vertex
// tracking punches page-sized RW holes into read-only pool runs, +2 VMAs each).
// This file: a background thread counts the lines of /proc/self/maps every 5 s, splits the guest address space part by
// protection, logs a [vma] line every DM_VMA_LOG seconds (default 60; 0 = only events; values below 5 are 5) and, when the count
// reaches DM_VMA_RELIEVE_PCT % of max_map_count (default 75; 0 = never), has the dirty tracker give the VMAs back
// (nvn_dirty_relieve: every protected run unprotected in one mprotect, so neighbours merge). The SVCs call dm_vma_relieve on ENOMEM.
#include "dm.h"
#if defined(__linux__) || defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

void dm_guest_as_bounds(uint64_t b[4]);   // hle_svc.c: guest AS [b0, b1), alias region [b2, b3)
uint64_t nvn_dirty_relieve(const char *why) __attribute__((weak));
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) __attribute__((weak));
void nvn_dirty_vma_stats(uint64_t *mp_fail, uint64_t *blk_extra, uint64_t *relieves, uint64_t *relieved_pages, uint64_t *sticky, int *blk_kb) __attribute__((weak));

typedef struct { int total, as_all, as_rw, as_ro, as_none, as_shared, alias_all, alias_rw; } vma_stat_t;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_scan_buf[65536];   // static: the count is also taken on guest threads (SVC ENOMEM path) with small stacks

static void classify(vma_stat_t *st, char *ln, const uint64_t b[4]) {
    char *e; uint64_t lo = strtoull(ln, &e, 16), hi;
    st->total++;
    if (*e != '-') return;
    hi = strtoull(e + 1, &e, 16);
    if (*e != ' ' || strlen(e) < 5) return;
    const char *perm = e + 1;
    if (lo >= b[0] && hi <= b[1] && b[1] > b[0]) {
        st->as_all++;
        if (perm[3] == 's') st->as_shared++;
        else if (perm[1] == 'w') st->as_rw++;
        else if (perm[0] == 'r') st->as_ro++;
        else st->as_none++;
        if (lo >= b[2] && hi <= b[3]) { st->alias_all++; if (perm[1] == 'w') st->alias_rw++; }
    }
}
static int vma_scan(vma_stat_t *st) {
    memset(st, 0, sizeof *st);
    uint64_t b[4] = {0}; dm_guest_as_bounds(b);
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_scan_mu);
    char ln[96]; int ll = 0;
    for (;;) {
        ssize_t n = read(fd, g_scan_buf, sizeof g_scan_buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            char c = g_scan_buf[i];
            if (c == '\n') { ln[ll] = 0; classify(st, ln, b); ll = 0; }
            else if (ll < (int)sizeof ln - 1) ln[ll++] = c;
        }
    }
    if (ll) { ln[ll] = 0; classify(st, ln, b); }
    pthread_mutex_unlock(&g_scan_mu);
    close(fd);
    return st->total;
}
int dm_vma_count_now(void) { vma_stat_t st; return vma_scan(&st); }
static int max_map_count(void) {
    static int v;
    if (!v) {
        v = 65530;   // DEFAULT_MAX_MAP_COUNT; /proc/sys/vm may be closed to apps
        int fd = open("/proc/sys/vm/max_map_count", O_RDONLY | O_CLOEXEC); char t[32] = "";
        if (fd >= 0) { ssize_t n = read(fd, t, sizeof t - 1); close(fd); if (n > 0) { t[n] = 0; int x = atoi(t); if (x > 1000) v = x; } }
    }
    return v;
}
// ENOMEM from an mprotect / mmap of ours: have the dirty tracker hand its VMAs back. Returns the pages it unprotected (0: nothing to
// give, the caller fails the request properly). Normal (non-signal) threads only.
uint64_t dm_vma_relieve(const char *why) {
    vma_stat_t st; vma_scan(&st);
    LOG("【红】[vma] %s: ENOMEM with %d maps (max_map_count %d; guest AS %d: rw %d, r-- %d, --- %d, shared %d; alias %d)", why, st.total, max_map_count(), st.as_all, st.as_rw, st.as_ro, st.as_none, st.as_shared, st.alias_all);
    return nvn_dirty_relieve ? nvn_dirty_relieve(why) : 0;
}
static void vma_log(const vma_stat_t *st, const char *tag) {
    uint64_t fa = 0, pr = 0, mf = 0, be = 0, rl = 0, rp = 0, sk = 0; int bk = 0;
    if (nvn_dirty_stats) nvn_dirty_stats(&fa, &pr);
    if (nvn_dirty_vma_stats) nvn_dirty_vma_stats(&mf, &be, &rl, &rp, &sk, &bk);
    static uint64_t pfa; uint64_t dfa = fa - pfa; pfa = fa;
    LOG("[vma]%s maps %d / max %d (guest AS %d: rw %d, r-- %d, --- %d, shared %d; alias %d, rw %d); write faults %llu (+%llu), host-write pages %llu, block extra pages %llu (unprotect block %d KiB), mprotect fails %llu, sticky pages %llu, relieves %llu (%llu pages)",
        tag, st->total, max_map_count(), st->as_all, st->as_rw, st->as_ro, st->as_none, st->as_shared, st->alias_all, st->alias_rw,
        (unsigned long long)fa, (unsigned long long)dfa, (unsigned long long)pr, (unsigned long long)be, bk, (unsigned long long)mf, (unsigned long long)sk, (unsigned long long)rl, (unsigned long long)rp);
}
static void *vma_watch(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "dm-vma");
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    const char *e = getenv("DM_VMA_LOG"); long period = e && *e ? atol(e) : 60; if (period > 0 && period < 5) period = 5;
    const char *q = getenv("DM_VMA_RELIEVE_PCT"); long pct = q && *q ? atol(q) : 75; if (pct < 0 || pct > 100) pct = 75;
    const int max = max_map_count(), at = (int)(max * pct / 100);
    LOG("[vma] watch: max_map_count %d, log every %ld s (DM_VMA_LOG), relieve at %ld%% = %d maps (DM_VMA_RELIEVE_PCT)", max, period, pct, pct ? at : 0);
    int after_last = 0; long since_log = 0;
    for (;;) {
        sleep(5); since_log += 5;
        vma_stat_t st; if (vma_scan(&st) < 0) { LOG("[vma] /proc/self/maps unreadable: watch stops"); return NULL; }
        // Relieve once the count reaches the threshold, and again only after it grew another 5% of max beyond what the last relief
        // left (a relief that freed little is not repeated every 5 s: each costs one re-check of every tracked texture / range)
        if (pct && st.total >= at && st.total >= after_last + max / 20) {
            vma_log(&st, " near the limit:");
            if (nvn_dirty_relieve) nvn_dirty_relieve("VMA watch");
            vma_scan(&st); after_last = st.total;
            vma_log(&st, st.total >= at ? " 【红】still high after relief:" : " after relief:");
            since_log = 0; continue;
        }
        if (period > 0 && since_log >= period) { vma_log(&st, ""); since_log = 0; }
    }
    return NULL;
}
void dm_vma_watch_start(void) {
    static int started; if (started) return; started = 1;
    const char *e = getenv("DM_VMA_WATCH"); if (e && *e == '0') { LOG("[vma] watch off (DM_VMA_WATCH=0)"); return; }
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 128 << 10);
    if (pthread_create(&t, &a, vma_watch, NULL)) LOG("【红】[vma] watch thread not started");
    pthread_attr_destroy(&a);
}
#else
// No /proc/self/maps (Apple): nothing to count or relieve
int dm_vma_count_now(void) { return -1; }
uint64_t dm_vma_relieve(const char *why) { (void)why; return 0; }
void dm_vma_watch_start(void) {}
#endif
