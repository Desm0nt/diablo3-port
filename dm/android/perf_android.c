// Android performance plumbing for the presents: CPU performance hints (ADPF), boost hints, the game state, thermal status and the display
// refresh-rate vote.
// - ADPF (APerformanceHint, Android 13; resolved with dlsym so older systems still load): the frame's work is reported against a target, so the
//   CPU governor raises clocks before a frame runs late instead of after. The work of a frame is the largest CPU time any thread of the session
//   spent between two presents (per-thread CPU clocks): a wall interval would include the pacer's idle wait and ask for clocks the frame does
//   not need (the same reasoning as hle_nvn.c pace_adapt_tick). Android 15 also gets the frame's GPU time (reportActualWorkDuration2) when the
//   Vulkan backend measures it (xbg_recent_gpu_ms).
// - [adpf] From DOOM Eternal (PERF-E14, ETR 26ece9a / adpf 53e8f89; port-common/modules/adpf-boost):
//   - target = min(present period, frame budget DM_ADPF_BUDGET_MS, default 33.33 = the Switch's 30 fps). The old target followed the pacer's
//     tier, so when slow frames pushed the tier down the hint relaxed with it and asked for less clock exactly when the game was late.
//     DM_ADPF_TARGET_MS=period is the old behaviour; <ms> fixed; auto = the mean work of the last 60 reports. Changes sent at most every 2 s.
//   - reports: every return code is counted; EPIPE (session lost) closes the session and re-opens it after 5 s (at most 8 times). A frame longer
//     than 1 s, or longer than 250 ms with < 25% CPU, is a hold (load, pause) and is not reported.
//   - members: re-scanned every DM_ADPF_SCAN_MS (5000; was every 300 presents). The presenting thread (nvn-translate) is always a member. [D3]
//     The game's main guest thread is a member from the start, its job workers (guest names DM_CPU_JOB_NAMES, default "JobWorker", the
//     cpu-place list; hle_thread.c thr_guest_threads) join at >= 5% of a core, before any other thread; the other game threads join at >= 15%;
//     the Java UI thread, RenderThread, binder, audio, JIT, GC and our idle helpers never; a member leaves after two scans below 5%; at most
//     DM_ADPF_MAX_THREADS (6) tids.
//   - setPreferPowerEfficiency(false) once (Android 15; DM_ADPF_POWER_EFF=1 asks for true, -1 does not call).
//   - DM_ADPF_DIAG_MS (10000; 0 = off): "[perf] ADPF 10s:" line with the report and boost counters, work vs target, each member's name / last
//     cpu / uclamp.min (when the kernel shows it) and every cpufreq policy's scaling_min / cur / max: whether the system acts on the hints.
//     Then the GPU clock "GPU MHz cur C max M" (Adreno kgsl, MediaTek GED / gpufreq, Mali / devfreq; probed once: gpu_probe).
// - [adpf-boost] Workload-increase hints (diablo3-port #23: after the launch boost runs out a Helio G99 drops the game to slow motion until
//   the recents screen or a screen recording boosts it again). On the presenting thread, when DM_ADPF_BOOST_FRAMES (3) frames in a row did
//   more CPU work than the target, or the frames of a whole second came on average > 15% slower than the target (fps under the game's pace),
//   the session gets a CPU load-up hint, at most once per DM_ADPF_BOOST_MS (500): APerformanceHint_notifyWorkloadIncrease(cpu, !gpu) where it
//   exists (public NDK, Android 16 / API 36), else APerformanceHint_sendHint(CPU_LOAD_UP) where libandroid exports it (platform symbol behind
//   Java PerformanceHintManager.Session.sendHint, which is @hide: Android 14-15 builds; not a public API, DM_ADPF_BOOST=2 skips it). After
//   the app comes back to the front (dm_aperf_focus) and when a loading screen goes away, a reset (notifyWorkloadReset / CPU_LOAD_RESET).
//   Android 13 without either: no boost hints (logged). DM_ADPF_BOOST=0 turns them off.
//   The decisions (when to boost, who is in the session) are plain functions in perf_boost.h (host test in port-common adpf-boost/test).
// - [game-state] GameManager.setGameState (Java, Android 13; GameStateHint.java polls nativeGameState twice a second while the app is in
//   front): loading = the game's loading screen is up (diag.c dm_loadscreen_up); mode = gameplay interruptible in a game (uninterruptible
//   once local play was opened: hle_ldn.c dm_ldn_lan_on), none in the front end (touch_android.c dm_android_scene_game). DM_GAME_STATE=0: off.
// - Thermal (AThermal, Android 11/12): the status is followed with a listener, logged with the headroom, and returned by dm_thermal_now()
//   (hle_nvn.c's perf summary). At SEVERE or worse the window's frame-rate vote drops to the lowest rate that is a multiple of the game's
//   rate and >= 60 Hz (a 120 Hz panel costs power the 30 / 60 fps game does not use); it returns to the marker vote at MODERATE or better.
// - Display rate: the vote vulkan_present.cpp makes at surface creation (marker gfx-frame-rate, default 120 Hz) is re-applied here whenever the
//   window or the wanted rate changes, with ANativeWindow_setFrameRateWithChangeStrategy(ONLY_IF_SEAMLESS) where available (Android 12), so a
//   thermal step never blanks the screen for a mode switch.
// - [pace-vote] (port-common/modules/pace-vote, KB PNH-C05 / PNH-C03; rules adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later):
//   - ADPF target = the period of the frame cap the player / the game chose (dm_pace_cap_hz, hle_nvn.c: 60 -> 16.67 ms, 30 -> 33.33), never
//     shorter than one screen refresh (DM_ADPF_TARGET=game, default). The old rule min(present period, DM_ADPF_BUDGET_MS) let a game that
//     dropped to 30 aim at 33.3 ms: the clocks were held right under 30 and it never stepped back to 60. DM_ADPF_TARGET=old: the old rule.
//   - the window's frame-rate vote = the lowest display rate that is a whole multiple of the cap and at least max(cap, 60) Hz, not above
//     the marker (pace_vote.h pv_vote_hz): cap 30 -> 60 Hz (a 30 fps cap on a 120 Hz panel came as 25 / 42 ms pairs at peanuts), 60 -> 60
//     where the display has a 60 Hz mode, above 60 / uncapped -> the marker (120). Followed live (the gear, eco_fps), held at its highest
//     for DM_VOTE_HOLD_MS (5000) before it goes down, re-applied every 5 s (a new surface votes the marker in vulkan_present.cpp).
//     PaceVote.java asks the window for the display mode of that rate (preferredDisplayModeId, same resolution) while the activity is in
//     front: the content vote alone left peanuts' panel at 120 Hz. DM_VOTE_HZ=max: the marker vote, no mode request (old).
// Switches (env file): DM_NO_ADPF=1 / DM_ADPF=0 no hint session; DM_ADPF_TARGET=game (old), DM_VOTE_HZ=cap (max), DM_VOTE_HOLD_MS=5000; DM_ADPF_TARGET_MS, DM_ADPF_BUDGET_MS=33.33, DM_ADPF_POWER_EFF=0,
// DM_ADPF_SCAN_MS=5000, DM_ADPF_MAX_THREADS=6, DM_ADPF_DIAG_MS=10000; DM_ADPF_BOOST=1 (0 off, 2 public NDK call only), DM_ADPF_BOOST_MS=500,
// DM_ADPF_BOOST_FRAMES=3; DM_GAME_STATE=1 (0 off); DM_THERMAL_VOTE=0 keep the marker vote whatever the temperature.
// Hook: dm_aperf_present(window, frame_rate_vote) once per present on the presenting thread (vulkan_present.cpp present(), before
// vkQueuePresentKHR); dm_aperf_start() once at boot and dm_aperf_focus(ev) from nativeFocus (jni_android.c).
// Portions ported from nx2android (https://github.com/seedds/nx2android) nxnative/dm/android/perf_android.c (ADPF session, thread scan,
// load increase after 3 frames over the target), Copyright (c) 2026 seedds, MIT License.
#include "../host/dm.h"
#include "touch_android.h"
#include "perf_boost.h"
#include "pace_vote.h"
#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double dm_present_period(void);   // gfx/vulkan/nvn_vk.c: this present's pacing period (s)
__attribute__((weak)) double xbg_recent_gpu_ms(void);
// [D3] the game's threads (hle_thread.c, cpu-place): live guest threads with host tid, kind (1 = the main thread) and the guest's own name
__attribute__((weak)) int thr_guest_threads(int32_t *tid, uint8_t *kind, char (*name)[32], int max);
// [game-state] what the game shows: diag.c (the loading screen), touch_android.c (in a game / front end), hle_ldn.c (local play opened)
__attribute__((weak)) int dm_loadscreen_up(void);
__attribute__((weak)) int dm_android_scene_game(void);
__attribute__((weak)) int dm_ldn_lan_on(void);
// [pace-vote] the frame cap the player / the game chose (Hz, 0 = uncapped; hle_nvn.c) and the screen's refresh (Hz, 0 = unknown; nvn_vk.c)
__attribute__((weak)) double dm_pace_cap_hz(void);
__attribute__((weak)) double mtl_display_refresh(void);
__attribute__((weak)) void nvn_present_cap_set(int hz);   // hle_nvn.c: the gear's frame rate limit while the game runs (PaceVote.setCap)

// ---------------- ADPF ----------------
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef struct AWorkDuration AWorkDuration;
static struct {
    APerformanceHintManager *(*get)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int (*update)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    void (*close)(APerformanceHintSession *);
    int (*set_threads)(APerformanceHintSession *, const int32_t *, size_t);   // Android 14
    AWorkDuration *(*wd_create)(void);                                        // Android 15
    void (*wd_start)(AWorkDuration *, int64_t), (*wd_total)(AWorkDuration *, int64_t), (*wd_cpu)(AWorkDuration *, int64_t), (*wd_gpu)(AWorkDuration *, int64_t);
    int (*report2)(APerformanceHintSession *, AWorkDuration *);
    int (*power)(APerformanceHintSession *, bool);                            // Android 15
    int (*increase)(APerformanceHintSession *, bool, bool, const char *);     // Android 16 (API 36)
    int (*wreset)(APerformanceHintSession *, bool, bool, const char *);       // Android 16 (API 36)
    int (*send_hint)(APerformanceHintSession *, int32_t);                     // platform symbol (Android 14-15 libandroid): SessionHint
} H;
enum { HINT_CPU_LOAD_UP = 0, HINT_CPU_LOAD_RESET = 2 };   // the platform's SessionHint values (= Java PerformanceHintManager.Session.CPU_LOAD_*)
static APerformanceHintManager *g_hint_mgr;
static APerformanceHintSession *g_hint; static int64_t g_hint_target; static int32_t g_hint_tids[8]; static int g_hint_n;
static uint8_t g_hint_low[8];   // consecutive scans each member spent below 5% of a core
static int g_adpf_power = 0, g_adpf_auto, g_adpf_period_mode, g_adpf_max_threads = 6;
static int g_adpf_game = 1;                // [pace-vote] DM_ADPF_TARGET=game (default): the cap's period; old = min(period, budget)
static _Atomic int64_t g_period_min;       // [pace-vote] the shortest present period of this session (ns; 0 = none yet)
static int64_t g_adpf_target_ns, g_adpf_budget_ns = 33333333, g_adpf_scan_ns = 5000000000LL, g_adpf_diag_ns = 10000000000LL;
static int g_boost = 1, g_boost_frames = 3; static int64_t g_boost_ns = 500000000;
static char g_job_names[128] = "JobWorker";
static AWorkDuration *g_wd;
// report and boost counters (presenting thread only), printed by the 10 s diagnostic line
static struct { unsigned ok, ok2, fail2, fail, lost, reopened; int last_err; int64_t wsum, wmax; unsigned wn;
                unsigned up_work, up_slow, up_refused, up_limited, resets, reset_refused; int up_rc; } g_rep;
static int64_t g_hint_reopen_at; static int64_t g_hint_last_update;
static _Atomic int g_reset_pending;   // 1: back in front (dm_aperf_focus), 2: a loading screen went away
static int g_ls_up;                   // the loading screen is up (dm_loadscreen_up, polled every 250 ms on the presenting thread)

static int64_t perf_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
// CPU time of any thread of this process: the kernel's per-thread CPU clock id (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
static int64_t thread_cpu_ns(int32_t tid) {
    clockid_t c = (clockid_t)(((uint32_t)~(uint32_t)tid << 3) | 6u); struct timespec ts;
    if (clock_gettime(c, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
// /proc/self/task/<tid>/stat: comm, utime + stime (clock ticks), the CPU it last ran on (field 39) (from DOOM 2016's r2 perf_android.c)
typedef struct { uint64_t ticks; int cpu; char comm[16]; } tstat_t;
static int task_stat(int tid, tstat_t *o) {
    char p[64], b[1024]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    o->ticks = 0; o->cpu = -1; o->comm[0] = 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1;
    b[n] = 0;
    char *l = strchr(b, '('), *r = strrchr(b, ')'); if (!l || !r || r < l) return -1;
    size_t cl = (size_t)(r - l - 1); if (cl > 15) cl = 15;
    memcpy(o->comm, l + 1, cl); o->comm[cl] = 0;
    unsigned long long ut = 0, st = 0; int field = 2;
    for (char *t = r + 1; *t; t++) if (*t == ' ') { field++;
        if (field == 14) ut = strtoull(t + 1, NULL, 10); else if (field == 15) st = strtoull(t + 1, NULL, 10);
        else if (field == 39) { o->cpu = atoi(t + 1); break; } }
    o->ticks = ut + st; return 0;
}

static void adpf_cfg_read(void) {
    const char *e;
    if ((e = getenv("DM_ADPF_POWER_EFF")) && *e) g_adpf_power = atoi(e) < 0 ? -1 : atoi(e) != 0;
    if ((e = getenv("DM_ADPF_TARGET_MS")) && *e) {
        if (!strcmp(e, "auto")) g_adpf_auto = 1; else if (!strcmp(e, "period")) g_adpf_period_mode = 1;
        else if (atof(e) > 0.5 && atof(e) < 500) g_adpf_target_ns = (int64_t)(atof(e) * 1e6);
    }
    if ((e = getenv("DM_ADPF_BUDGET_MS")) && *e && atof(e) > 0.5 && atof(e) < 500) g_adpf_budget_ns = (int64_t)(atof(e) * 1e6);
    if ((e = getenv("DM_ADPF_TARGET")) && *e) g_adpf_game = strcmp(e, "old") != 0 && strcmp(e, "0") != 0;
    if ((e = getenv("DM_ADPF_SCAN_MS")) && *e && atoi(e) >= 500) g_adpf_scan_ns = (int64_t)atoi(e) * 1000000;
    if ((e = getenv("DM_ADPF_DIAG_MS")) && *e) g_adpf_diag_ns = (int64_t)(atoi(e) > 0 ? atoi(e) : 0) * 1000000;
    if ((e = getenv("DM_ADPF_MAX_THREADS")) && *e) { int v = atoi(e); g_adpf_max_threads = v < 1 ? 1 : v > 8 ? 8 : v; }
    if ((e = getenv("DM_ADPF_BOOST")) && *e) g_boost = atoi(e) < 0 ? 0 : atoi(e) > 2 ? 2 : atoi(e);
    if ((e = getenv("DM_ADPF_BOOST_MS")) && *e && atoi(e) >= 100) g_boost_ns = (int64_t)atoi(e) * 1000000;
    if ((e = getenv("DM_ADPF_BOOST_FRAMES")) && *e && atoi(e) >= 1) g_boost_frames = atoi(e);
    if ((e = getenv("DM_CPU_JOB_NAMES")) && *e) snprintf(g_job_names, sizeof g_job_names, "%s", e);
    LOG("[perf] ADPF switches: DM_ADPF_TARGET=%s DM_ADPF_TARGET_MS=%s DM_ADPF_BUDGET_MS=%.2f DM_ADPF_POWER_EFF=%d DM_ADPF_SCAN_MS=%lld DM_ADPF_MAX_THREADS=%d DM_ADPF_DIAG_MS=%lld"
        " DM_ADPF_BOOST=%d DM_ADPF_BOOST_MS=%lld DM_ADPF_BOOST_FRAMES=%d; job workers = guest names %s*",
        g_adpf_game ? dm_pace_cap_hz ? "game (cap period)" : "game (no cap hook: shortest period)" : "old (min(period, budget))",
        g_adpf_auto ? "auto" : g_adpf_period_mode ? "period" : g_adpf_target_ns ? "fixed" : "unset", g_adpf_budget_ns / 1e6, g_adpf_power,
        (long long)(g_adpf_scan_ns / 1000000), g_adpf_max_threads, (long long)(g_adpf_diag_ns / 1000000), g_boost, (long long)(g_boost_ns / 1000000),
        g_boost_frames, g_job_names);
}
// The target the session gets for a present period p (ns): fixed / auto value, the period itself, [pace-vote] the period of the game's
// cap (DM_ADPF_TARGET=game, default; pace_vote.h pv_adpf_target_ns), or the period capped at the frame budget (DM_ADPF_TARGET=old).
static double cap_hz_now(void) { return dm_pace_cap_hz ? dm_pace_cap_hz() : 0; }
static double screen_hz_now(void) { return mtl_display_refresh ? mtl_display_refresh() : 0; }
static int64_t adpf_target(int64_t p) {
    if (g_adpf_target_ns) return g_adpf_target_ns;
    if (g_adpf_period_mode) return p;
    if (g_adpf_game) return pv_adpf_target_ns(cap_hz_now(), screen_hz_now(), atomic_load(&g_period_min));
    return p < g_adpf_budget_ns ? p : g_adpf_budget_ns;
}
static const char *boost_path(void) {
    if (!g_boost) return "off (DM_ADPF_BOOST=0)";
    if (H.increase && H.wreset) return "notifyWorkloadIncrease / Reset (Android 16)";
    if (H.send_hint && g_boost == 1) return "sendHint CPU_LOAD_UP / RESET (platform symbol)";
    return "none on this Android";
}
static void hint_create(int64_t target) {
    g_hint = H.create(g_hint_mgr, g_hint_tids, (size_t)g_hint_n, target);
    g_hint_target = target; g_hint_last_update = perf_now();
    if (g_hint && g_adpf_power >= 0 && H.power) { int r = H.power(g_hint, g_adpf_power != 0); LOG("[perf] ADPF setPreferPowerEfficiency(%s) -> %d", g_adpf_power ? "true" : "false", r); }
}
static void hint_init(int64_t period) {
    static int tried; if (tried) return; tried = 1;
    if (getenv("DM_NO_ADPF")) { LOG("[perf] DM_NO_ADPF: CPU hints off"); return; }
    void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) return;
    H.get = dlsym(lib, "APerformanceHint_getManager"); H.create = dlsym(lib, "APerformanceHint_createSession");
    H.update = dlsym(lib, "APerformanceHint_updateTargetWorkDuration"); H.report = dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    H.close = dlsym(lib, "APerformanceHint_closeSession"); H.set_threads = dlsym(lib, "APerformanceHint_setThreads");
    H.wd_create = dlsym(lib, "AWorkDuration_create"); H.wd_start = dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos");
    H.wd_total = dlsym(lib, "AWorkDuration_setActualTotalDurationNanos"); H.wd_cpu = dlsym(lib, "AWorkDuration_setActualCpuDurationNanos");
    H.wd_gpu = dlsym(lib, "AWorkDuration_setActualGpuDurationNanos"); H.report2 = dlsym(lib, "APerformanceHint_reportActualWorkDuration2");
    H.power = dlsym(lib, "APerformanceHint_setPreferPowerEfficiency");
    H.increase = dlsym(lib, "APerformanceHint_notifyWorkloadIncrease"); H.wreset = dlsym(lib, "APerformanceHint_notifyWorkloadReset");
    H.send_hint = dlsym(lib, "APerformanceHint_sendHint");
    if (!H.get || !H.create || !H.report) { LOG("[perf] no APerformanceHint on this Android: CPU hints off"); return; }
    adpf_cfg_read();
    const int64_t target = adpf_target(period);
    LOG("[perf] ADPF symbols: setThreads %s, reportActualWorkDuration2 %s, setPreferPowerEfficiency %s, notifyWorkloadIncrease %s, sendHint (platform) %s; boost hints: %s",
        H.set_threads ? "yes" : "no", H.report2 ? "yes" : "no", H.power ? "yes" : "no", H.increase && H.wreset ? "yes" : "no", H.send_hint ? "yes" : "no", boost_path());
    g_hint_mgr = H.get(); if (!g_hint_mgr) { LOG("[perf] ADPF: APerformanceHint_getManager returned NULL: CPU hints off"); return; }
    g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1;
    hint_create(target);
    if (g_hint && H.wd_create && H.wd_start && H.wd_total && H.wd_cpu && H.wd_gpu && H.report2) g_wd = H.wd_create();
    LOG("[perf] ADPF session %s (target %.2f ms: %s, period %.2f, cap %.0f Hz, screen %.1f Hz, budget %.2f, thread %d%s%s)", g_hint ? "open" : "refused",
        target / 1e6, g_adpf_target_ns ? "fixed" : g_adpf_period_mode ? "period" : g_adpf_game ? "cap period (DM_ADPF_TARGET=game)" : "min(period, budget)",
        period / 1e6, cap_hz_now(), screen_hz_now(), g_adpf_budget_ns / 1e6, g_hint_tids[0], H.set_threads ? ", threads join by load" : "", g_wd ? ", GPU time reported" : "");
}
// EPIPE from a report: the system dropped the session (binder death, HAL restart). Close it and open a new one 5 s later (at most 8 times).
static void hint_lost(int64_t now) {
    if (!g_hint) return;
    if (H.close) H.close(g_hint);
    g_hint = NULL; g_rep.lost++;
    g_hint_reopen_at = g_rep.lost <= 8 ? now + 5000000000LL : 0;
    LOG("[perf] ADPF session lost (EPIPE, %u so far): %s", g_rep.lost, g_hint_reopen_at ? "re-opening in 5 s" : "giving up");
}
// Which threads may join by load: the game's (guest threads) and our own frame threads; never the Java UI thread (tid = pid), the platform's
// threads or our idle / diagnostic helpers, whose load is not the frame's.
static int adpf_thread_ok(int tid, const char *comm) {
    if (tid == (int)getpid()) return 0;
    static const char *const deny[] = {"RenderThread", "binder:", "HwBinder:", "AudioTrack", "AAudio", "Jit thread", "HeapTaskDaemon", "hwuiTask",
        "blastAsyncWork", "FinalizerDaemon", "FinalizerWatchd", "ReferenceQueueD", "Signal Catcher", "Profile Saver", "perfetto", "queued-work",
        "GPU completion", "ADB-JDWP", "dm-sensors", "dm-ctl", "pin-trace", "dm-crumbs", "dm-lifecycle", "dm-java-input", "haptics", "dm-listw",
        "dm-prep", "dm-hold", "dm-l3mem", "dm-csched", "dm-cpu"};
    for (size_t i = 0; i < sizeof deny / sizeof deny[0]; i++) if (!strncmp(comm, deny[i], strlen(deny[i]))) return 0;
    return 1;
}
static int is_job_name(const char *name) {
    char list[128]; snprintf(list, sizeof list, "%s", g_job_names);
    for (char *sv = NULL, *t = strtok_r(list, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if (*t && !strncmp(name, t, strlen(t))) return 1;
    return 0;
}
// The other threads that do frame work: the presenting thread is the Vulkan backend's, the game's main thread (simulation) and its job workers
// set the pace as much (nx2android: without them the main thread stayed on mid cores and its bursts made long frames).
// Every DM_ADPF_SCAN_MS: the presenting thread is always a member, [D3] the game's main thread too (from the first scan, unless idle on two
// scans in a row); the other members stay unless they spent < 5% of a core on two scans in a row; free places go first to the game's job
// workers with >= 5% of a core, then to the busiest other allowed threads with >= 15% (DOOM 2016 r2 DM_ADPF_HYST, ETR adpf).
static void hint_threads_scan(int32_t self) {
    static struct { int tid; uint64_t ticks; } prev[512]; static int np; static int64_t last;
    int64_t now = perf_now(); double secs = last ? (now - last) / 1e9 : 0; last = now;
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    static struct { int tid; uint64_t used; char comm[16]; } cur[512]; static dm_hint_cand_t cand[512]; int nc = 0; struct dirent *e;
    while ((e = readdir(d)) && nc < 512) { int tid = atoi(e->d_name); if (tid <= 0) continue;
        tstat_t ts; task_stat(tid, &ts); cur[nc].tid = tid; cur[nc].used = ts.ticks; memcpy(cur[nc].comm, ts.comm, sizeof ts.comm); nc++; }
    closedir(d);
    // [D3] the game's roles: 1 = its main thread, 2 = a job worker (by the guest's own thread name), 0 = anything else
    static int32_t gt[256]; static uint8_t gk[256]; static char gn[256][32];
    int ng = thr_guest_threads ? thr_guest_threads(gt, gk, gn, 256) : 0, main_tid = 0;
    for (int i = 0; i < ng; i++) if (gk[i] == 1 && gt[i] > 0) main_tid = gt[i];
    for (int i = 0; i < nc; i++) {
        uint64_t was = 0; for (int j = 0; j < np; j++) if (prev[j].tid == cur[i].tid) { was = prev[j].ticks; break; }
        cand[i].tid = cur[i].tid; cand[i].was = was != 0; cand[i].u = cur[i].used > was ? cur[i].used - was : 0;
        cand[i].ok = adpf_thread_ok(cur[i].tid, cur[i].comm); cand[i].role = 0;
        for (int k = 0; k < ng; k++) if (gt[k] == cur[i].tid) { cand[i].role = gk[k] == 1 ? 1 : is_job_name(gn[k]) ? 2 : 0; if (cand[i].role) { cand[i].ok = 1; snprintf(cur[i].comm, sizeof cur[i].comm, "%.15s", gk[k] == 1 ? "main" : gn[k]); } break; }
    }
    np = 0; for (int i = 0; i < nc; i++) { prev[np].tid = cur[i].tid; prev[np].ticks = cur[i].used; np++; }
    long hz = sysconf(_SC_CLK_TCK); double tick_s = secs * (double)(hz > 0 ? hz : 100);
    int32_t pick[8]; uint8_t plow[8] = {0};
    int npick = dm_hint_pick(cand, nc, self, main_tid, g_hint_tids, g_hint_low, g_hint_n, (uint64_t)(0.15 * tick_s), (uint64_t)(0.05 * tick_s), secs <= 0, g_adpf_max_threads, pick, plow);
    if (!g_hint || !H.set_threads) return;
    int same = npick == g_hint_n;
    for (int i = 0; same && i < npick; i++) { int in = 0; for (int j = 0; j < g_hint_n; j++) in |= g_hint_tids[j] == pick[i]; same = in; }
    if (same) { for (int i = 0; i < npick; i++) for (int j = 0; j < g_hint_n; j++) if (g_hint_tids[j] == pick[i]) g_hint_low[j] = plow[i]; return; }
    int r = H.set_threads(g_hint, pick, (size_t)npick);
    if (r) { static unsigned told; if (told++ < 8) LOG("[perf] ADPF: the session refused %d threads (rc %d)", npick, r); if (r == EPIPE) hint_lost(now); return; }
    memcpy(g_hint_tids, pick, sizeof(int32_t) * (size_t)npick); memcpy(g_hint_low, plow, (size_t)npick); g_hint_n = npick;
    char l[320]; size_t o = 0;
    for (int i = 0; i < npick && o < sizeof l; i++) { const char *nm = "?"; for (int k = 0; k < nc; k++) if (cur[k].tid == pick[i]) { nm = cur[k].comm; break; }
        o += (size_t)snprintf(l + o, sizeof l - o, " %d:%s", pick[i], nm); }
    LOG("[perf] ADPF session threads:%s", l);
}
// The session threads' CPU time since the previous present: the largest one is the frame's work (the critical path's CPU share).
static int64_t g_tid_cpu[8]; static int32_t g_tid_seen[8];
static int64_t frame_cpu_work(void) {
    int64_t worst = 0;
    for (int i = 0; i < g_hint_n; i++) {
        int64_t c = thread_cpu_ns(g_hint_tids[i]);
        if (c < 0) { g_tid_seen[i] = 0; continue; }   // the thread ended: the next scan drops it
        if (g_tid_seen[i] == g_hint_tids[i] && c > g_tid_cpu[i] && c - g_tid_cpu[i] > worst) worst = c - g_tid_cpu[i];
        g_tid_cpu[i] = c; g_tid_seen[i] = g_hint_tids[i];
    }
    return worst;
}

// ---------------- [adpf-boost] workload hints ----------------
// One hint to the session: up (CPU load increase) or reset. Public NDK calls where Android 16 has them, else the platform's sendHint.
// Returns the call's rc, or -1 when this Android has no way to send it.
static int boost_call(int reset, const char *why) {
    if (H.increase && H.wreset) return reset ? H.wreset(g_hint, true, false, why) : H.increase(g_hint, true, false, why);
    if (H.send_hint && g_boost == 1) return H.send_hint(g_hint, reset ? HINT_CPU_LOAD_RESET : HINT_CPU_LOAD_UP);
    return -1;
}
// Per reported frame (not on holds): N frames in a row over the target, or a whole second of frames > 15% slower than the target, asks for
// more CPU (at most once per DM_ADPF_BOOST_MS); a pending reset (front again, loading screen gone) is sent first.
static void boost_tick(int64_t t, int64_t work, int64_t iv, int held, int64_t target) {
    static dm_boost_t st;
    if (!g_boost || !g_hint || (!(H.increase && H.wreset) && !(H.send_hint && g_boost == 1))) return;
    int why_reset = atomic_exchange(&g_reset_pending, 0);
    if (why_reset) {
        const char *why = why_reset == 1 ? "back in front" : "loading screen gone";
        int r = boost_call(1, why);
        if (r == 0) g_rep.resets++; else g_rep.reset_refused++;
        static unsigned told; if (told++ < 8) LOG("[perf] ADPF workload reset (%s) -> rc %d", why, r);
        dm_boost_clear(&st);
        if (r == EPIPE) { hint_lost(t); return; }
    }
    // loads: GameState isLoading speaks for them
    int d = dm_boost_decide(&st, t, work, iv, held || g_ls_up, target, g_boost_frames, g_boost_ns);
    if (d == DM_BOOST_LIMITED) { g_rep.up_limited++; return; }
    if (d == DM_BOOST_NONE) return;
    const char *why = d == DM_BOOST_SLOW ? "frames slow for 1 s" : "frames over target";
    int r = boost_call(0, why);
    if (r == 0) { if (d == DM_BOOST_SLOW) g_rep.up_slow++; else g_rep.up_work++; }
    else if (r == EBUSY) g_rep.up_limited++;   // the system's own rate limit (Android 16)
    else { g_rep.up_refused++; g_rep.up_rc = r; if (r == EPIPE) hint_lost(t); }
    static unsigned told; if (told++ < 5) LOG("[perf] ADPF load increase (%s: work %.1f ms, frame %.1f ms, target %.1f ms) -> rc %d", why, work / 1e6, iv / 1e6, target / 1e6, r);
}
// nativeFocus (jni_android.c, UI thread): 2 / 3 = in front again: the next present sends a workload reset.
void dm_aperf_focus(int ev) { if (ev == 2 || ev == 3) atomic_store(&g_reset_pending, 1); }

// [adpf] DM_ADPF_DIAG_MS: does the system act on the hints? A boosting HAL either raises the session threads' uclamp.min (AOSP / Pixel
// ADPF: /proc/self/task/<tid>/sched shows it when the kernel has CONFIG_SCHED_DEBUG + uclamp) or takes a perf lock that raises a cluster's
// scaling_min_freq. File reads only (no sched_getattr: the app seccomp policy is not known to allow it).
static int uclamp_of(int tid, int *umin, int *eff) {
    char p[64], b[4096]; snprintf(p, sizeof p, "/proc/self/task/%d/sched", tid); *umin = *eff = -1;
    int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n <= 0) return -1; b[n] = 0;
    for (char *s = b; s && *s; s = strchr(s, '\n'), s = s ? s + 1 : NULL) {
        char *c = strchr(s, ':'); if (!c) continue;
        if (!strncmp(s, "uclamp.min", 10)) *umin = atoi(c + 1); else if (!strncmp(s, "effective uclamp.min", 20)) *eff = atoi(c + 1);
    }
    return *umin >= 0 ? 0 : -1;
}
static int read_khz(const char *path) { char b[32]; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1; ssize_t r = read(fd, b, sizeof b - 1); close(fd); if (r <= 0) return -1; b[r] = 0; return atoi(b); }
// [adpf] GPU frequency for the "[perf] ADPF 10s" line (D3 issue #23: does the slow-down follow a GPU clock drop?). Probed once (first
// diag line): the first readable source of the list below, logged as "[perf] GPU frequency: <path> ..." or "no readable GPU frequency"
// with each tried path's error (SELinux denies most of them to apps on many phones); then " GPU MHz cur C max M" every line. Values
// in Hz, kHz or MHz are told apart by size. MediaTek GED "current_freqency" is "<opp index> <kHz>" (the last number is taken), the
// gpufreqv2 status "... Freq: <kHz> ..." (the first "Freq:"), the gpufreq v1 dump "g_cur_gpu_freq = <kHz>".
enum { GF_NUM = 0, GF_LAST, GF_TAG_FREQ, GF_TAG_CUR };
typedef struct { const char *cur, *max; int kind; } gpu_src_t;
static const gpu_src_t k_gpu_src[] = {
    {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/max_gpuclk", GF_NUM},                       // Adreno (Hz)
    {"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq", "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", GF_NUM},       // Adreno devfreq (Hz)
    {"/sys/kernel/ged/hal/current_freqency", NULL, GF_LAST},                                                   // MediaTek GED "<opp> <kHz>"
    {"/sys/kernel/ged/hal/gpu_cur_freq", NULL, GF_LAST},
    {"/proc/gpufreqv2/gpufreq_status", NULL, GF_TAG_FREQ},                                                    // MediaTek gpufreq v2 (kHz)
    {"/proc/gpufreq/gpufreq_var_dump", NULL, GF_TAG_CUR},                                                     // MediaTek gpufreq v1 (kHz)
    {"/sys/class/misc/mali0/device/cur_freq", "/sys/class/misc/mali0/device/max_freq", GF_NUM},               // Mali generic
};
static char g_gpu_cur[160], g_gpu_max[160]; static int g_gpu_kind = -1;   // -1 not probed, -2 none readable
static int gpu_read(const char *path, char *b, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -errno;
    ssize_t r = read(fd, b, n - 1); int e = errno; close(fd); if (r <= 0) return r < 0 ? -e : -ENODATA; b[r] = 0; return 0;
}
static int gpu_mhz(long long v) { return v <= 0 ? -1 : v >= 10000000LL ? (int)(v / 1000000) : v >= 10000 ? (int)(v / 1000) : (int)v; }
static int gpu_parse(const char *b, int kind) {
    const char *p = b;
    if (kind == GF_TAG_FREQ || kind == GF_TAG_CUR) { p = strstr(b, kind == GF_TAG_FREQ ? "Freq:" : "cur_gpu_freq"); if (!p) return -1; }
    long long v = -1;
    for (; *p && *p != '\n'; p++) if (*p >= '0' && *p <= '9') {
        v = strtoll(p, (char **)&p, 10); if (kind != GF_LAST) break; if (!*p || *p == '\n') break;
    }
    return gpu_mhz(v);
}
static int gpu_now(const char *path, int kind) { char b[2048]; return path && *path && !gpu_read(path, b, sizeof b) ? gpu_parse(b, kind) : -1; }
static void gpu_probe(void) {
    char tried[900]; size_t o = 0; tried[0] = 0;
    for (unsigned i = 0; i < sizeof k_gpu_src / sizeof *k_gpu_src; i++) {
        char b[2048]; int r = gpu_read(k_gpu_src[i].cur, b, sizeof b), mhz = r ? -1 : gpu_parse(b, k_gpu_src[i].kind);
        if (mhz > 0) { snprintf(g_gpu_cur, sizeof g_gpu_cur, "%s", k_gpu_src[i].cur); g_gpu_kind = k_gpu_src[i].kind;
            g_gpu_max[0] = 0; if (k_gpu_src[i].max && gpu_now(k_gpu_src[i].max, GF_NUM) > 0) snprintf(g_gpu_max, sizeof g_gpu_max, "%s", k_gpu_src[i].max);
            LOG("[perf] GPU frequency: %s (now %d MHz), max %s%s", g_gpu_cur, mhz, g_gpu_max[0] ? g_gpu_max : "not readable", tried[0] ? tried : ""); return; }
        if (o < sizeof tried) o += (size_t)snprintf(tried + o, sizeof tried - o, "%s %s: %s", o ? ";" : "; tried", k_gpu_src[i].cur, r ? strerror(-r) : "no number");
    }
    DIR *d = opendir("/sys/class/devfreq");   // devfreq nodes of the GPU: *mali*, *gpu*, *g3d*, *kgsl*
    if (d) { struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || !(strstr(de->d_name, "mali") || strstr(de->d_name, "gpu") || strstr(de->d_name, "g3d") || strstr(de->d_name, "kgsl"))) continue;
            char c[160], m[160]; snprintf(c, sizeof c, "/sys/class/devfreq/%s/cur_freq", de->d_name); snprintf(m, sizeof m, "/sys/class/devfreq/%s/max_freq", de->d_name);
            char b[256]; int r = gpu_read(c, b, sizeof b), mhz = r ? -1 : gpu_parse(b, GF_NUM);
            if (mhz > 0) { snprintf(g_gpu_cur, sizeof g_gpu_cur, "%s", c); g_gpu_kind = GF_NUM; g_gpu_max[0] = 0; if (gpu_now(m, GF_NUM) > 0) snprintf(g_gpu_max, sizeof g_gpu_max, "%s", m);
                closedir(d); LOG("[perf] GPU frequency: %s (now %d MHz), max %s%s", g_gpu_cur, mhz, g_gpu_max[0] ? g_gpu_max : "not readable", tried); return; }
            if (o < sizeof tried) o += (size_t)snprintf(tried + o, sizeof tried - o, "; %s: %s", c, r ? strerror(-r) : "no number");
        }
        closedir(d);
    } else if (o < sizeof tried) o += (size_t)snprintf(tried + o, sizeof tried - o, "; /sys/class/devfreq: %s", strerror(errno));
    g_gpu_kind = -2;
    LOG("[perf] GPU frequency: no readable GPU frequency%s", tried);
}
static void adpf_diag(int64_t now, int64_t target) {
    static int64_t last; if (!g_adpf_diag_ns || (last && now - last < g_adpf_diag_ns)) return;
    if (!last) { last = now; return; }
    double secs = (now - last) / 1e9; last = now;
    char l[1280]; size_t o = (size_t)snprintf(l, sizeof l, "[perf] ADPF %.0fs: session %s target %.2f ms; reports ok %u (with GPU time %u, GPU refused %u) failed %u (last rc %d) lost %u;"
        " work mean %.1f / max %.1f ms; boost up %u (over target %u, slow 1 s %u) refused %u (rc %d) limited %u, reset %u (refused %u); threads",
        secs, g_hint ? "open" : "none", target / 1e6, g_rep.ok, g_rep.ok2, g_rep.fail2, g_rep.fail, g_rep.last_err, g_rep.lost,
        g_rep.wn ? g_rep.wsum / g_rep.wn / 1e6 : 0.0, g_rep.wmax / 1e6, g_rep.up_work + g_rep.up_slow, g_rep.up_work, g_rep.up_slow, g_rep.up_refused, g_rep.up_rc,
        g_rep.up_limited, g_rep.resets, g_rep.reset_refused);
    for (int i = 0; i < g_hint_n && o < sizeof l; i++) {
        tstat_t ts; int um, ue; task_stat(g_hint_tids[i], &ts); uclamp_of(g_hint_tids[i], &um, &ue);
        o += (size_t)snprintf(l + o, sizeof l - o, " %d:%s@cpu%d", g_hint_tids[i], ts.comm[0] ? ts.comm : "?", ts.cpu);
        if (um >= 0 && o < sizeof l) o += (size_t)snprintf(l + o, sizeof l - o, " uclamp.min %d/eff %d", um, ue);
    }
    if (o < sizeof l) o += (size_t)snprintf(l + o, sizeof l - o, "; policies MHz min/cur/max:");
    for (int p = 0; p < 16 && o < sizeof l; p++) {
        char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", p);
        int cur = read_khz(path); if (cur < 0) continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq", p); int mn = read_khz(path);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq", p); int mx = read_khz(path);
        o += (size_t)snprintf(l + o, sizeof l - o, " p%d %d/%d/%d", p, mn > 0 ? mn / 1000 : -1, cur / 1000, mx > 0 ? mx / 1000 : -1);
    }
    if (g_gpu_kind == -1) gpu_probe();
    if (g_gpu_kind >= 0 && o < sizeof l) {
        int cur = gpu_now(g_gpu_cur, g_gpu_kind), mx = g_gpu_max[0] ? gpu_now(g_gpu_max, GF_NUM) : -1;
        o += (size_t)snprintf(l + o, sizeof l - o, "; GPU MHz cur %d", cur);
        if (mx > 0 && o < sizeof l) o += (size_t)snprintf(l + o, sizeof l - o, " max %d", mx);
    }
    LOG("%s", l);
    // per window (lost / last rc are totals)
    g_rep.ok = g_rep.ok2 = g_rep.fail2 = g_rep.fail = 0; g_rep.wsum = g_rep.wmax = 0; g_rep.wn = 0;
    g_rep.up_work = g_rep.up_slow = g_rep.up_refused = g_rep.up_limited = g_rep.resets = g_rep.reset_refused = 0;
}

// ---------------- [game-state] GameManager.setGameState (Java GameStateHint polls this) ----------------
// Returns -2 when off (DM_GAME_STATE=0: Java stops asking), else loading (bit 0) | GameState mode << 4 (0 unknown, 1 none, 2 gameplay
// interruptible, 3 gameplay uninterruptible). Logs every change.
enum { GS_UNKNOWN = 0, GS_NONE = 1, GS_PLAY = 2, GS_PLAY_UNINT = 3 };
static int game_state_code(void) {
    static int off = -1; if (off < 0) { const char *e = getenv("DM_GAME_STATE"); off = e && *e && !atoi(e); if (off) LOG("[perf] DM_GAME_STATE=0: no GameManager.setGameState"); }
    if (off) return -2;
    int ls = dm_loadscreen_up ? dm_loadscreen_up() : 0, sg = dm_android_scene_game ? dm_android_scene_game() : -1, lan = dm_ldn_lan_on ? dm_ldn_lan_on() : 0;
    int mode = sg > 0 ? (lan ? GS_PLAY_UNINT : GS_PLAY) : ls ? GS_PLAY : sg == 0 ? GS_NONE : GS_UNKNOWN;
    int code = (ls ? 1 : 0) | mode << 4;
    static int last = -1;
    if (code != last) {
        static const char *const mn[] = {"unknown", "none", "gameplay interruptible", "gameplay uninterruptible"};
        LOG("[perf] game state: loading %d, mode %s (load screen %s, scene %s, local play %s)", ls ? 1 : 0, mn[mode], ls ? "up" : "down",
            sg < 0 ? "unknown" : sg ? "game" : "front end", lan ? "opened" : "no");
        last = code;
    }
    return code;
}
JNIEXPORT jint JNICALL Java_com_xiaoboluo_d2r_GameStateHint_nativeGameState(JNIEnv *env, jclass cls) { (void)env; (void)cls; return game_state_code(); }
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_GameStateHint_nativeGameStateNote(JNIEnv *env, jclass cls, jstring s) {
    (void)cls; const char *c = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    LOG("[perf] game state (java): %s", c ? c : "?");
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}

// ---------------- thermal ----------------
typedef struct AThermalManager AThermalManager;
static struct {
    AThermalManager *(*acquire)(void);
    int (*status)(AThermalManager *);
    int (*listen)(AThermalManager *, void (*)(void *, int), void *);
    float (*headroom)(AThermalManager *, int);
} T;
static AThermalManager *g_thermal; static _Atomic int g_tstatus = -1;
enum { TH_NONE, TH_LIGHT, TH_MODERATE, TH_SEVERE, TH_CRITICAL, TH_EMERGENCY, TH_SHUTDOWN };
static const char *th_name(int s) { static const char *n[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"}; return s >= 0 && s <= 6 ? n[s] : "unknown"; }
static void thermal_changed(void *data, int status) {   // a binder thread of the system's
    (void)data;
    int old = atomic_exchange(&g_tstatus, status);
    if (old != status) {
        float h = T.headroom && g_thermal ? T.headroom(g_thermal, 10) : NAN;   // forecast 10 s ahead; NaN when asked too often
        LOG("[perf] thermal status %s -> %s (headroom in 10 s %.2f)", th_name(old), th_name(status), (double)h);
    }
}
int dm_thermal_now(void) { return atomic_load(&g_tstatus); }   // strong over hle_nvn.c's weak default (-1): the perf summary's thermal column
void dm_aperf_start(void) {
    static int done; if (done) return; done = 1;
    void *lib = dlopen("libandroid.so", RTLD_NOW); if (!lib) return;
    T.acquire = dlsym(lib, "AThermal_acquireManager"); T.status = dlsym(lib, "AThermal_getCurrentThermalStatus");
    T.listen = dlsym(lib, "AThermal_registerThermalStatusListener"); T.headroom = dlsym(lib, "AThermal_getThermalHeadroom");
    if (!T.acquire || !T.status) { LOG("[perf] no AThermal on this Android: thermal status unknown"); return; }
    g_thermal = T.acquire(); if (!g_thermal) return;
    atomic_store(&g_tstatus, T.status(g_thermal));
    if (T.listen && T.listen(g_thermal, thermal_changed, NULL)) LOG("[perf] thermal listener refused: status read at start only");
    LOG("[perf] thermal status at start: %s", th_name(atomic_load(&g_tstatus)));
}

// ---------------- display rate vote ----------------
static int32_t (*g_set_rate)(ANativeWindow *, float, int8_t);
static int32_t (*g_set_rate2)(ANativeWindow *, float, int8_t, int8_t);   // Android 12: with a change strategy
// [pace-vote] the display's refresh rates at its current resolution (PaceVote.java hands them over twice a second, UI thread) and the vote
// for its mode request (milli-Hz; -1 = DM_VOTE_HZ=max: no request)
static float g_modes[16]; static _Atomic int g_nmodes; static _Atomic int g_pv_want_mhz;
static int vote_cap_mode(void) {   // DM_VOTE_HZ=cap (default): the vote follows the frame cap; max (or 0): the marker as before
    static _Atomic int v = -1; int r = atomic_load(&v); if (r >= 0) return r;
    const char *e = getenv("DM_VOTE_HZ"); r = !(e && (!strcmp(e, "max") || !strcmp(e, "0")));
    atomic_store(&v, r);
    LOG("[perf] DM_VOTE_HZ=%s: %s", r ? "cap" : "max", r ? "the window votes the lowest display rate that is a multiple of the frame cap and >= 60 Hz" : "the marker vote (old)");
    return r;
}
static int64_t vote_hold_ns(void) {
    static int64_t v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_VOTE_HOLD_MS"); v = e && *e && atoi(e) >= 0 ? (int64_t)atoi(e) * 1000000 : 5000000000LL;
    return v;
}
static int modes_now(float *m) { int n = atomic_load(&g_nmodes); if (n > 16) n = 16; if (n > 0) memcpy(m, g_modes, (size_t)n * sizeof *m); return n; }
static float vote_for(float marker, double period, double *cap_out) {
    if (marker <= 0) return 0;   // marker gfx-frame-rate = 0: no vote at all
    static int thermal_vote = -1; if (thermal_vote < 0) { const char *e = getenv("DM_THERMAL_VOTE"); thermal_vote = !(e && !atoi(e)); }
    static int hot; int s = atomic_load(&g_tstatus);
    if (s >= TH_SEVERE) hot = 1; else if (s >= 0 && s <= TH_MODERATE) hot = 0;
    float base = marker;
    if (vote_cap_mode()) {   // [pace-vote] the cap's display rate (pace_vote.h pv_vote_hz); a port without a cap hook: the present rate
        double cap = dm_pace_cap_hz ? dm_pace_cap_hz() : period > 0 ? 1 / period : 0;
        float m[16]; int n = modes_now(m);
        base = pv_vote_hz(cap, m, n, marker); *cap_out = cap;
    }
    if (!thermal_vote || !hot) return base;
    return pv_thermal_hz(base, period);   // the lowest multiple of the game's rate that is at least 60 Hz
}
static void vote(ANativeWindow *w, float hz, double cap, int log) {
    static int resolved; if (!resolved) { resolved = 1; void *lib = dlopen("libnativewindow.so", RTLD_NOW);
        if (lib) { g_set_rate = dlsym(lib, "ANativeWindow_setFrameRate"); g_set_rate2 = dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy"); } }
    int r = g_set_rate2 ? g_set_rate2(w, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT, 0 /* ONLY_IF_SEAMLESS */)
          : g_set_rate ? g_set_rate(w, hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT) : -1;
    if (!log) return;
    if (cap < 0) { LOG("[perf] window frame-rate vote %.0f Hz%s (thermal %s)", (double)hz, r ? " refused" : "", th_name(atomic_load(&g_tstatus))); return; }
    char ml[128] = ""; float m[16]; int n = modes_now(m); size_t o = 0;
    for (int i = 0; i < n && o < sizeof ml; i++) o += (size_t)snprintf(ml + o, sizeof ml - o, "%s%.2f", i ? " " : "", (double)m[i]);
    LOG("[perf] window frame-rate vote %.0f Hz%s (cap %.0f Hz, DM_VOTE_HZ=cap; modes %s; thermal %s)", (double)hz, r ? " refused" : "", cap,
        n ? ml : "not known yet", th_name(atomic_load(&g_tstatus)));
}
// Per present: the vote for this window, sent again when the window or the rate changes, [pace-vote] and every 5 s in cap mode (a surface
// made again votes the marker in vulkan_present.cpp create_surface, maybe on the same ANativeWindow pointer).
static void pace_vote_tick(ANativeWindow *w, float marker, double period, int64_t t) {
    static ANativeWindow *voted_w; static float voted; static int64_t voted_t; static pv_hold_t hold;
    double cap = -1; const int capm = vote_cap_mode();
    float want = vote_for(marker, period, &cap);
    if (capm) want = pv_hold(&hold, want, t, vote_hold_ns());   // up at once, down after DM_VOTE_HOLD_MS at the lower rate
    atomic_store(&g_pv_want_mhz, capm ? (int)lroundf(want * 1000) : -1);
    if (!w || want <= 0) return;
    const int changed = w != voted_w || fabsf(want - voted) > 0.5f;
    if (changed || (capm && t - voted_t >= 5000000000LL)) { vote(w, want, capm ? cap : -1, changed); voted_w = w; voted = want; voted_t = t; }
}
// [pace-vote] PaceVote.java: the modes in, the vote out (Hz; 0 = no request, < 0 = off: DM_VOTE_HZ=max); a line in boot.log; the gear's limit.
JNIEXPORT jfloat JNICALL Java_com_xiaoboluo_d2r_PaceVote_nativeVote(JNIEnv *env, jclass cls, jfloatArray modes) {
    (void)cls;
    if (modes) {
        jsize n = (*env)->GetArrayLength(env, modes); if (n > 16) n = 16;
        float m[16]; if (n > 0) { (*env)->GetFloatArrayRegion(env, modes, 0, n, m); memcpy(g_modes, m, (size_t)n * sizeof *m); }
        atomic_store(&g_nmodes, n > 0 ? (int)n : 0);
    }
    int v = atomic_load(&g_pv_want_mhz);
    return v < 0 ? -1.0f : v / 1000.0f;
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_PaceVote_nativeNote(JNIEnv *env, jclass cls, jstring s) {
    (void)cls; const char *c = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    LOG("[perf] pace-vote (java): %s", c ? c : "?");
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_PaceVote_nativeSetCap(JNIEnv *env, jclass cls, jint hz) {
    (void)env; (void)cls;
    if (nvn_present_cap_set) nvn_present_cap_set((int)hz);
    else LOG("[perf] pace-vote: frame rate limit %d from the gear: this tree has no live limit (next start)", (int)hz);
}

// ---------------- per present ----------------
// Performance hints are skipped when DM_ADPF=0 or when the process runs under an ARM translation layer (LDPlayer /
// emulators: libhoudini / libndk_translation bridge these platform calls and crash in them); real devices keep them.
static int g_off_xlat;   // [pace-vote] the hints are off for a translation layer (the vote stays as vulkan_present.cpp made it there)
static int perf_off(void) {
    static int off = -1;
    if (off >= 0) return off;
    const char *e = getenv("DM_ADPF"); off = e && !strcmp(e, "0");
    if (!off) {
        FILE *f = fopen("/proc/self/maps", "r"); char line[512];
        if (f) { while (fgets(line, sizeof line, f)) if (strstr(line, "libhoudini") || strstr(line, "libndk_translation")) { off = 1; g_off_xlat = 1; break; } fclose(f); }
    }
    if (off) LOG("[perf] performance hints off (%s)", e && !strcmp(e, "0") ? "DM_ADPF=0" : "ARM translation layer detected");
    return off;
}
void dm_aperf_present(ANativeWindow *w, float marker_vote) {
    double period = dm_present_period();
    int64_t t = perf_now();
    // [pace-vote] the shortest present period of the session (the ADPF target of a port without a cap hook) and the window's vote, also with
    // the hints off by DM_ADPF=0 (the vote is the display's, not the hint session's)
    if (period > 0.004 && period < 0.2) { int64_t pn = (int64_t)(period * 1e9), m = atomic_load(&g_period_min); if (!m || pn < m) atomic_store(&g_period_min, pn); }
    const int off = perf_off();
    if (!g_off_xlat) pace_vote_tick(w, marker_vote, period, t);
    if (off) return;
    int64_t p = period > 0 && period < 1 ? (int64_t)(period * 1e9) : 16666667;
    hint_init(p);
    p = adpf_target(p);   // [adpf] the hint target: min(period, budget) by default (DM_ADPF_TARGET_MS: fixed / auto / period)
    static unsigned frames; static int64_t prev_t, next_scan, next_ls;
    // [adpf] a lost session (EPIPE) is re-opened here, with the current members
    if (!g_hint && g_hint_reopen_at && t >= g_hint_reopen_at && g_hint_mgr) {
        g_hint_reopen_at = 0; hint_create(p); if (g_hint) g_rep.reopened++;
        LOG("[perf] ADPF session re-open %s (%d threads, target %.2f ms)", g_hint ? "ok" : "refused", g_hint_n, p / 1e6);
    }
    if (frames++ == 0 || t >= next_scan) { next_scan = t + g_adpf_scan_ns; hint_threads_scan((int32_t)gettid()); }
    // [adpf-boost] a loading screen that went away asks for a workload reset (polled every 250 ms: dm_loadscreen_up takes a lock)
    if (dm_loadscreen_up && t >= next_ls) { next_ls = t + 250000000; int ls = dm_loadscreen_up() != 0; if (g_ls_up && !ls) atomic_store(&g_reset_pending, 2); g_ls_up = ls; }
    if (g_hint) {
        if (!g_hint_n) { g_hint_tids[0] = (int32_t)gettid(); g_hint_n = 1; }
        int64_t work = frame_cpu_work(), iv = prev_t ? t - prev_t : 0;
        if (work > iv && iv > 0) work = iv;
        // A hold (load screen, pause menu, background) is not a frame: longer than 1 s, or longer than 250 ms with under 25% CPU. Slow real
        // frames are reported: they are exactly the frames the governor should see.
        const int held = iv <= 0 || iv >= 1000000000 || (iv >= 250000000 && work < iv / 4);
        if (work > 0 && !held) {
            double gms = xbg_recent_gpu_ms ? xbg_recent_gpu_ms() : -1; int rc;
            if (g_wd && gms > 0) {
                int64_t g = (int64_t)(gms * 1e6);
                H.wd_start(g_wd, prev_t); H.wd_total(g_wd, work > g ? work : g); H.wd_cpu(g_wd, work); H.wd_gpu(g_wd, g);
                rc = H.report2(g_hint, g_wd);
                if (rc) { g_rep.fail2++; rc = H.report(g_hint, work); } else g_rep.ok2++;
            } else rc = H.report(g_hint, work);
            if (rc) { g_rep.fail++; g_rep.last_err = rc; static unsigned told; if (told++ < 4) LOG("[perf] ADPF report of %.2f ms failed: rc %d (%s)", work / 1e6, rc, strerror(rc));
                if (rc == EPIPE) hint_lost(t); }
            else g_rep.ok++;
            g_rep.wsum += work; g_rep.wn++; if (work > g_rep.wmax) g_rep.wmax = work;
            static int64_t wsum, wmax, asum; static unsigned wn, an; wsum += work; if (work > wmax) wmax = work;
            if (g_adpf_auto && ++an == 60) {   // DM_ADPF_TARGET_MS=auto: the target follows the mean work of the last 60 reports
                int64_t m = (asum + work) / 60; if (m < 1000000) m = 1000000; if (m > 500000000) m = 500000000;
                if (!g_adpf_target_ns || m > g_adpf_target_ns + g_adpf_target_ns / 10 || m < g_adpf_target_ns - g_adpf_target_ns / 10) g_adpf_target_ns = m;
                asum = 0; an = 0;
            } else asum += work;
            if (++wn == 600) { LOG("[perf] ADPF: frame CPU work mean %.1f / max %.1f ms against %.1f ms, thermal %s", wsum / 600 / 1e6, wmax / 1e6, (g_hint_target ? g_hint_target : p) / 1e6, th_name(atomic_load(&g_tstatus))); wsum = wmax = 0; wn = 0; }
        }
        if (g_hint) boost_tick(t, work, iv, held || work <= 0, g_hint_target ? g_hint_target : p);
        // [adpf] target changes at most every 2 s (the pacer's tier moves every few seconds; each update is a binder call and a HAL re-tune)
        if (g_hint && p != g_hint_target && H.update && t - g_hint_last_update >= 2000000000LL) {
            int r = H.update(g_hint, p); g_hint_last_update = t;
            if (!r) { g_hint_target = p; static unsigned told; if (told++ < 32) LOG("[perf] ADPF target %.2f ms", p / 1e6); }
            else { LOG("[perf] ADPF target %.2f ms refused: rc %d", p / 1e6, r); if (r == EPIPE) hint_lost(t); }
        }
    }
    if (g_hint_mgr) adpf_diag(t, g_hint_target ? g_hint_target : p);
    prev_t = t;
}
