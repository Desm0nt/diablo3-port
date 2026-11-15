// [cpu-place] Thread placement at run time (port-common/modules/cpu-place, KB APP-14; the plan: cpu_topo.h / hle_cpu_topo.c).
// Why (diablo3-port #23, Helio G99 = 2 x A76 + 6 x A55): low fps and slow motion after ~30 s of play, fast again for 30-40 s after
// a trip to the recents screen or when screen recording starts; a scheduler / governor Magisk module fixes it. Before this module
// D3 (and XC2 / TL2) pinned only the thread that presents (nvn-translate) to the cores with the highest cpuinfo_max_freq, once per
// 120 presents; the game's main thread, its job workers and every other guest thread were left to the scheduler, so whatever the
// system's boosts and cpusets did to them after the launch boost ran out was never undone and never visible in the log.
// What it does:
//   - one plan per mode (gear "CPU cores": auto / big / all, DM_CPU_CORES; live through dm_cpu_place_set_mode): masks for the
//     translator, the game's main guest thread, the job workers (guest names "JobWorker*", DM_CPU_JOB_NAMES=<prefix,prefix>) and
//     the other guest threads;
//   - thread "dm-cpu": applies the masks, then checks them every DM_CPU_CHECK_MS (2000) and right after the app is back in front
//     (dm_cpu_place_focus 2 / 3: at once and every 0.5 s for 4 s, since Android moves the process between cpusets after onResume,
//     which rewrites thread masks); paused while the app is in the background (onStop);
//   - a thread whose mask was changed by somebody else is put back and logged ("[cpu] ... found <mask>, set <mask> again");
//     a thread left free whose mask is narrower than the process's own is logged too (the system narrowed it);
//   - every 0.5 s the CPU each key thread last ran on is sampled; "[cpu] where <N>s:" (every 60 s, the first after 30 s) reports per role how often it was found on
//     the little cluster, with the main thread's cpuset cgroup (and its cpus where readable) - the diagnosis #23 needs.
// Log lines (boot.log): "[cpu] topology: ...", "[cpu] plan: ...", "[cpu] placed (<why>): ...", "[cpu] where <N>s: ...",
// "[cpu] main thread cpuset: A -> B", "[cpu] <role> tid N mask found X (set Y): set again (<why>)", "[cpu] resume: ...".
// Switches: DM_CPU_CORES=auto|big|all (the gear writes it), DM_CPU_TOPO=index (the old rule, A/B), DM_CPU_CHECK_MS=2000,
// DM_CPU_JOB_NAMES=JobWorker, DM_CPU_REPORT_S=60 (the first "where" line after half of it). vulkan_present.cpp's pin markers (gfx-pin-cpu, gfx-no-pin, gfx-ab-target=pin) still win for the
// translator (dm_cpu_place_markers).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // pthread_setname_np (glibc, for the host test; bionic has it anyway)
#endif
#include "cpu_topo.h"
#if defined(__ANDROID__) && !defined(__OHOS__)
#include "dm.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// hle_thread.c: live guest threads: host tid, kind (1 = the main thread), the guest's own name (SDK ThreadType); returns the count
__attribute__((weak)) int thr_guest_threads(int32_t *tid, uint8_t *kind, char (*name)[32], int max);

enum { R_TRANSLATE, R_MAIN, R_JOBS, R_GUEST, R_N };
static const char *const k_role[R_N] = {"translate", "main", "jobs", "guest"};
typedef struct { uint64_t b[4]; } cpumask_t;   // 256 CPUs
static int aff_set(int tid, uint32_t m) { cpumask_t s; memset(&s, 0, sizeof s); s.b[0] = m; return syscall(__NR_sched_setaffinity, tid, sizeof s, &s) < 0 ? -1 : 0; }
static int aff_get(int tid, uint32_t *m) { cpumask_t s; memset(&s, 0, sizeof s); if (syscall(__NR_sched_getaffinity, tid, sizeof s, &s) < 0) return -1; *m = (uint32_t)s.b[0]; return 0; }
static int64_t mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static dm_cpu_topo_in_t g_in;
static dm_cpu_plan_t g_plan;           // under g_mu after start (the mode can change)
static int g_legacy, g_markers, g_bg, g_started, g_check_ms = 2000, g_report_s = 60;
static const char *g_why;              // a pending reason to check now (under g_mu)
static int64_t g_burst_until;          // check every tick until then (after a resume)
static _Atomic int g_xtid;             // the presenting thread
static char g_job_names[128] = "JobWorker";

static void plan_init(void) {
    { const char *r = getenv("DM_CPU_SYSFS_ROOT"); dm_cpu_topo_read(r ? r : "", &g_in); }   // a fake tree: the host test only
    const char *e = getenv("DM_CPU_TOPO"); g_legacy = e && (!strcmp(e, "index") || !strcmp(e, "legacy") || !strcmp(e, "old"));
    int mode = dm_cpu_mode_parse(getenv("DM_CPU_CORES")); if (mode < 0) mode = DM_CPU_MODE_AUTO;
    if ((e = getenv("DM_CPU_CHECK_MS")) && atoi(e) >= 200) g_check_ms = atoi(e);
    if ((e = getenv("DM_CPU_REPORT_S")) && atoi(e) >= 2) g_report_s = atoi(e);
    if ((e = getenv("DM_CPU_JOB_NAMES")) && *e) snprintf(g_job_names, sizeof g_job_names, "%s", e);
    dm_cpu_plan_mode(&g_in, mode, g_legacy, &g_plan);
    char b[900];
    dm_cpu_topo_describe(&g_in, &g_plan, b, sizeof b); LOG("[cpu] topology: %s", b);
    dm_cpu_plan_describe(&g_plan, b, sizeof b);
    LOG("[cpu] plan: %s; job workers = guest names %s*; checked every %d ms and after onResume (DM_CPU_CORES=auto|big|all from the gear, DM_CPU_TOPO=index, DM_CPU_CHECK_MS)",
        b, g_job_names, g_check_ms);
}
const dm_cpu_plan_t *dm_cpu_plan(void) { pthread_once(&g_once, plan_init); return &g_plan; }

static void kick(const char *why) { pthread_mutex_lock(&g_mu); g_why = why; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
void dm_cpu_place_translator(int tid) { if (tid > 0 && atomic_load_explicit(&g_xtid, memory_order_relaxed) != tid) { atomic_store(&g_xtid, tid); if (g_started) kick("translator"); } }
void dm_cpu_place_markers(int on) {
    pthread_mutex_lock(&g_mu); g_markers = on; pthread_mutex_unlock(&g_mu);
    if (on) LOG("[cpu] translator pin left to vulkan_present.cpp's markers (gfx-pin-cpu / gfx-no-pin / gfx-ab-target=pin)");
}
void dm_cpu_place_set_mode(int mode) {
    (void)dm_cpu_plan();
    if (mode < DM_CPU_MODE_AUTO || mode > DM_CPU_MODE_ALL) return;
    char b[600];
    pthread_mutex_lock(&g_mu);
    if (mode == g_plan.mode) { pthread_mutex_unlock(&g_mu); return; }
    dm_cpu_plan_mode(&g_in, mode, g_legacy, &g_plan); dm_cpu_plan_describe(&g_plan, b, sizeof b);
    g_why = "mode"; pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    LOG("[cpu] plan (gear, live): %s", b);
}
void dm_cpu_place_focus(int ev) {
    pthread_mutex_lock(&g_mu);
    int was = g_bg;
    if (ev == 1) g_bg = 1;
    else if (ev == 2 || ev == 3) { g_bg = 0; g_burst_until = mono_ms() + 4000; g_why = "resume"; pthread_cond_signal(&g_cv); }
    pthread_mutex_unlock(&g_mu);
    if (ev == 1 && !was) LOG("[cpu] background: placement checks paused");
}

// ---- per-thread bookkeeping
typedef struct { int tid, role, seen; uint32_t set; char name[32]; } track_t;
static track_t g_tr[256]; static int g_ntr;
static unsigned g_resets, g_resets_logged, g_fails, g_narrow, g_narrow_logged;
static uint32_t g_proc_mask;                  // this (untouched) thread's mask = what the process's cpuset allows now
// where the key threads ran: samples per role and CPU over the report window
static unsigned g_where[R_N][DM_CPU_MAX], g_where_n[R_N];

static int task_cpu(int tid) {   // field 39 of /proc/self/task/<tid>/stat: the CPU it last ran on
    char p[64], b[600]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    char *s = strrchr(b, ')'); if (!s) return -1;
    int field = 2; for (s++; *s && field < 39; s++) if (*s == ' ') field++;
    return field == 39 ? atoi(s) : -1;
}
static int is_job(const char *name) {
    char list[128]; snprintf(list, sizeof list, "%s", g_job_names);
    for (char *sv = NULL, *t = strtok_r(list, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if (*t && !strncmp(name, t, strlen(t))) return 1;
    return 0;
}
static uint32_t role_mask(const dm_cpu_plan_t *p, int r) { return r == R_TRANSLATE ? p->translate : r == R_MAIN ? p->main : r == R_JOBS ? p->jobs : p->guest; }

typedef struct { int tid, role; char name[32]; } live_t;
static int collect(live_t *lv, int max, int xtid) {
    int n = 0;
    if (xtid) {   // its host name (nvn-translate; a guest thread's with DM_NVN_SYNC=1)
        lv[n].tid = xtid; lv[n].role = R_TRANSLATE; snprintf(lv[n].name, sizeof lv[n].name, "presenter");
        char p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", xtid); FILE *f = fopen(p, "r");
        if (f) { if (fgets(lv[n].name, sizeof lv[n].name, f)) lv[n].name[strcspn(lv[n].name, "\n")] = 0; fclose(f); }
        n++;
    }
    if (!thr_guest_threads) return n;
    static int32_t tid[256]; static uint8_t kind[256]; static char name[256][32];
    int ng = thr_guest_threads(tid, kind, name, 256);
    for (int i = 0; i < ng && n < max; i++) {
        if (tid[i] <= 0 || tid[i] == xtid) continue;   // DM_NVN_SYNC=1: the guest thread that presents keeps the translator's place
        lv[n].tid = tid[i]; lv[n].role = kind[i] == 1 ? R_MAIN : is_job(name[i]) ? R_JOBS : R_GUEST;
        snprintf(lv[n].name, sizeof lv[n].name, "%.31s", kind[i] == 1 ? "main" : name[i]); n++;
    }
    return n;
}

// One pass: every live thread gets its role's mask (or, when its role is free and it was set before, every CPU back).
static void place_pass(const char *why, int log_placed) {
    pthread_mutex_lock(&g_mu); dm_cpu_plan_t p = g_plan; int markers = g_markers; pthread_mutex_unlock(&g_mu);
    const int xtid = atomic_load(&g_xtid);
    static live_t lv[256]; int n = collect(lv, 256, xtid);
    uint32_t own = 0; if (aff_get(0, &own) || !own) own = p.all;
    if (own != g_proc_mask) { if (g_proc_mask) LOG("[cpu] the process's cpus (cpuset) now %x, were %x (%s)", own, g_proc_mask, why); g_proc_mask = own; }
    unsigned pass_resets = 0, pass_new = 0, pass_free = 0, nguest = 0; char reset_text[200] = ""; size_t rto = 0;
    for (int i = 0; i < g_ntr; i++) g_tr[i].seen = 0;
    for (int i = 0; i < n; i++) {
        track_t *t = NULL;
        for (int j = 0; j < g_ntr && !t; j++) if (g_tr[j].tid == lv[i].tid) t = &g_tr[j];
        if (!t && g_ntr < 256) { t = &g_tr[g_ntr++]; memset(t, 0, sizeof *t); t->tid = lv[i].tid; }
        if (!t) break;
        t->seen = 1; t->role = lv[i].role; memcpy(t->name, lv[i].name, sizeof t->name);
        if (lv[i].role == R_GUEST) nguest++;
        uint32_t want = (lv[i].role == R_TRANSLATE && markers) ? 0 : role_mask(&p, lv[i].role), cur = 0;
        if (aff_get(t->tid, &cur)) { t->seen = 0; continue; }   // gone
        if (lv[i].role == R_TRANSLATE && markers) continue;      // vulkan_present.cpp's markers own it
        if (want) {
            uint32_t expect = want & own ? want & own : want;    // the kernel reports the mask within the active / allowed CPUs
            if (cur == expect && t->set == want) continue;
            int was_set = t->set == want;
            if (aff_set(t->tid, want)) { if (++g_fails <= 4) LOG("[cpu] %s tid %d (%s): setting %x failed, errno %d (%s)", k_role[t->role], t->tid, t->name, want, errno, why); continue; }
            t->set = want;
            if (was_set) {   // somebody else changed a mask we had set
                ++g_resets; ++pass_resets;
                if (rto + 40 < sizeof reset_text) rto += (size_t)snprintf(reset_text + rto, sizeof reset_text - rto, "%s%s %x", rto ? ", " : "", t->role == R_MAIN ? "main" : t->role == R_TRANSLATE ? "translate" : t->name, cur);
                if (++g_resets_logged <= 8) LOG("[cpu] %s tid %d (%s) mask found %x (set %x): set again (%s)", k_role[t->role], t->tid, t->name, cur, want, why);
            } else pass_new++;
        } else {
            if (t->set) { aff_set(t->tid, p.all); t->set = 0; pass_free++; continue; }   // its role went free: every CPU back
            if (t->role != R_GUEST && cur != own && (cur & own) != own) {   // left free, narrower than the process: the system did it
                ++g_narrow;
                if (++g_narrow_logged <= 8) LOG("[cpu] %s tid %d (%s) left free but its mask is %x, the process's %x (%s; not changed: mode %s)", k_role[t->role], t->tid, t->name, cur, own, why, dm_cpu_mode_name(p.mode));
            }
        }
    }
    int k = 0; for (int i = 0; i < g_ntr; i++) if (g_tr[i].seen) g_tr[k++] = g_tr[i]; g_ntr = k;   // drop the exited ones
    if (!strcmp(why, "resume")) {
        static int64_t told; int64_t now = mono_ms();
        if (pass_resets || now - told > 5000) { LOG("[cpu] resume: %u thread(s) had other masks%s%s%s, %u newly placed, %u released; process cpus %x",
                                                    pass_resets, pass_resets ? " (" : "", reset_text, pass_resets ? ")" : "", pass_new, pass_free, own); told = now; }
    }
    if (log_placed) {
        char l[900], m[40]; size_t o = (size_t)snprintf(l, sizeof l, "[cpu] placed (%s, mode %s):", why, dm_cpu_mode_name(p.mode));
        for (int i = 0; i < g_ntr && o + 80 < sizeof l; i++) if (g_tr[i].role != R_GUEST) {
            dm_cpu_mask_text(g_tr[i].role == R_TRANSLATE && markers ? 0 : role_mask(&p, g_tr[i].role), m, sizeof m);
            o += (size_t)snprintf(l + o, sizeof l - o, " %s tid %d (%s) -> %s%s;", k_role[g_tr[i].role], g_tr[i].tid, g_tr[i].name, m, g_tr[i].role == R_TRANSLATE && markers ? " (markers)" : "");
        }
        dm_cpu_mask_text(p.guest, m, sizeof m);
        if (o + 80 < sizeof l) snprintf(l + o, sizeof l - o, " %u other guest threads -> %s; process cpus %x", nguest, m, own);
        LOG("%s", l);
    }
}

static void where_sample(void) {
    for (int i = 0; i < g_ntr; i++) {
        if (g_tr[i].role == R_GUEST) continue;
        int c = task_cpu(g_tr[i].tid); if (c < 0 || c >= DM_CPU_MAX) continue;
        g_where[g_tr[i].role][c]++; g_where_n[g_tr[i].role]++;
    }
}
static void cgroup_of(int tid, char *out, size_t n) {   // the cpuset cgroup path of a thread ("" when unreadable)
    char p[64], l[256]; snprintf(p, sizeof p, "/proc/self/task/%d/cgroup", tid); out[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return;
    while (fgets(l, sizeof l, f)) { char *s = strstr(l, ":cpuset:"); if (s) { s += 8; s[strcspn(s, "\n")] = 0; snprintf(out, n, "%s", s); break; } }
    fclose(f);
}
static void cpuset_cpus(const char *cg, char *out, size_t n) {   // /dev/cpuset<cg>/cpus where SELinux lets an app read it
    char p[200]; snprintf(p, sizeof p, "/dev/cpuset%s/cpus", strcmp(cg, "/") ? cg : ""); out[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}
static void where_report(int64_t secs, const char *cg) {
    pthread_mutex_lock(&g_mu); dm_cpu_plan_t p = g_plan; pthread_mutex_unlock(&g_mu);
    char l[900]; size_t o = (size_t)snprintf(l, sizeof l, "[cpu] where %llds (mode %s, little cpus %x):", (long long)secs, dm_cpu_mode_name(p.mode), p.little);
    for (int r = 0; r < R_GUEST && o + 120 < sizeof l; r++) {
        if (!g_where_n[r]) continue;
        unsigned lit = 0; for (int c = 0; c < DM_CPU_MAX; c++) if (p.little >> c & 1) lit += g_where[r][c];
        o += (size_t)snprintf(l + o, sizeof l - o, " %s on little %u/%u [", k_role[r], lit, g_where_n[r]);
        int first = 1; for (int c = 0; c < DM_CPU_MAX && o + 16 < sizeof l; c++) if (g_where[r][c]) { o += (size_t)snprintf(l + o, sizeof l - o, "%scpu%d:%u", first ? "" : " ", c, g_where[r][c]); first = 0; }
        o += (size_t)snprintf(l + o, sizeof l - o, "];");
    }
    if (o + 120 < sizeof l) snprintf(l + o, sizeof l - o, " masks set again %u, failed %u, free ones narrowed %u; main cpuset %s; process cpus %x", g_resets, g_fails, g_narrow, *cg ? cg : "?", g_proc_mask);
    LOG("%s", l);
    memset(g_where, 0, sizeof g_where); memset(g_where_n, 0, sizeof g_where_n);
}

static void *place_thread(void *a) {
    (void)a;
    pthread_setname_np(pthread_self(), "dm-cpu");
    int64_t next_check = 0, next_report = mono_ms() + g_report_s * 500, report_from = mono_ms(); int placed_told = 0, last_mode = -1; char cg[96] = "";
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 500000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        if (!g_why) pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        const char *why = g_why; g_why = NULL; int bg = g_bg, mode = g_plan.mode; int64_t burst = g_burst_until;
        pthread_mutex_unlock(&g_mu);
        if (bg) continue;
        int64_t now = mono_ms();
        if (why || now >= next_check || now < burst) {
            // the "placed" line: once both the main thread and the presenter are known, and after every mode change
            int xt = atomic_load(&g_xtid), have_main = 0; for (int i = 0; i < g_ntr; i++) have_main |= g_tr[i].role == R_MAIN;
            int tell = (!placed_told && xt && have_main) || (mode != last_mode && last_mode >= 0);
            place_pass(why ? why : now < burst ? "resume" : "check", tell);
            if (tell) { placed_told = 1; last_mode = mode; } else if (last_mode < 0) last_mode = mode;
            next_check = now + g_check_ms;
            // the main thread's cpuset cgroup (top-app / foreground / background...): logged when it changes
            for (int i = 0; i < g_ntr; i++) if (g_tr[i].role == R_MAIN) {
                char c[96]; cgroup_of(g_tr[i].tid, c, sizeof c);
                if (*c && strcmp(c, cg)) { char cpus[64]; cpuset_cpus(c, cpus, sizeof cpus); LOG("[cpu] main thread cpuset: %s -> %s%s%s", *cg ? cg : "(start)", c, *cpus ? ", cpus " : "", cpus); snprintf(cg, sizeof cg, "%s", c); }
                break;
            }
        }
        where_sample();
        if (now >= next_report) { where_report((now - report_from) / 1000, cg); report_from = now; next_report = now + g_report_s * 1000; }
    }
    return NULL;
}

void dm_cpu_place_start(void) {
    (void)dm_cpu_plan();
    if (!thr_guest_threads) LOG("[cpu] no guest thread list in this build: only the presenting thread is placed");
    pthread_mutex_lock(&g_mu); int was = g_started; g_started = 1; pthread_mutex_unlock(&g_mu);
    if (was) return;
    pthread_t th;
    if (pthread_create(&th, NULL, place_thread, NULL)) { LOG("[cpu] placement thread not started (errno %d)", errno); return; }
    pthread_detach(th);
}
#else
// other systems (iOS / HarmonyOS builds of the same tree, the host test): the plan only, nothing is placed
static dm_cpu_plan_t g_plan_other; static int g_plan_other_ok;
const dm_cpu_plan_t *dm_cpu_plan(void) { if (!g_plan_other_ok) { dm_cpu_topo_in_t in; dm_cpu_topo_read("", &in); dm_cpu_plan_mode(&in, DM_CPU_MODE_AUTO, 0, &g_plan_other); g_plan_other_ok = 1; } return &g_plan_other; }
void dm_cpu_place_start(void) {}
void dm_cpu_place_translator(int tid) { (void)tid; }
void dm_cpu_place_markers(int on) { (void)on; }
void dm_cpu_place_set_mode(int mode) { (void)mode; }
void dm_cpu_place_focus(int ev) { (void)ev; }
#endif
