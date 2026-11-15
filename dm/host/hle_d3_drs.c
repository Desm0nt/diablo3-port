// [D3] The game's dynamic resolution (DRS) under host control: off from the gear, the 3D width aligned to 64 on non-Adreno, and a
// 10-second log line of what DRS does (D3 issue #23, KB MALI-16).
//
// What the game does (update 2.7.7, build 2607a74f; found 15.11 with the "Variable Res Percent %6.2f. Device Frame us: %d" stats
// print, main 0x3ee30): one state of 0x44 bytes per world view (constructor 0x3cb90, at view +0x2c0):
//   +0x00 u32 14250   lower GPU time (us): below it the scale goes up
//   +0x04 u32 15250   upper GPU time (us): above it the scale goes down
//   +0x08 u32 cooldown frames after a change (8)
//   +0x0c f32 0.7     smallest scale          +0x10 f32 1.0  largest scale
//   +0x14 u32 samples taken                    +0x18 f32 the scale (1.0 at start)
//   +0x1c f32 0.05    step                     +0x20 u32 average of the last 8 GPU frame times (us)
//   +0x24..+0x40      ring of the last 8 GPU frame times
// The render thread (0x428f0) calls the controller 0x3cd10 once a frame: the last GPU frame time (0xeaf60: the NX64NVNCore timer
// of the frame before last, ReportCounter timestamps), the average, then game option 0x79 set -> scale 1.0; else after the
// cooldown average < lower -> +step, > upper -> -step (cooldown 8), clamped to 0.7..1.0. The scene pass (0x43220, at 0x43c14) takes
// the 3D target's size from 0xebac0 (handheld: the docked-size target x 0.8) and renders into the rect (0, 0, (u32)(scale x w),
// (u32)(scale x h)) - no rounding at all; PostFX Upscale (shader 0x81) stretches that rect to the window. So on a GPU slower than
// the Switch's 15 ms budget the 3D settles at 0.7: 2048x900 widescreen handheld = 1638x720 x 0.7 = 1146x504.
//
// The controller is replaced (0x3cd10 -> drs_tick, the same logic) so the host can steer it:
//   DM_D3_DRS=auto (default)  the game's own DRS;  =off (or 0)  always 1.0 (full resolution, more GPU load).
//                             Live from the gear "Dynamic resolution" (dm_d3_drs_set, DmActivity.nativeSetD3Drs).
//   DM_D3_DRS_ALIGN=<n>       the 3D rect width a multiple of n (16..256): the scale written for the scene pass is lowered to
//                             (floor(scale x w / n) x n + 0.5) / w, the controller keeps its own unaligned value. Default (unset /
//                             auto): 64 on every GPU family except Adreno, 0 (off) on Adreno; known once the Vulkan device exists
//                             (xbg_gpu_policy_family, so DM_GFX_SIM_GPU=mali counts as Mali). Mali-G57 showed garbage columns every 64
//                             texels of a 1146-wide DRS rect (MALI-16, hypothesis 1); 16:9 handheld (1280 x 0.7 = 896 = 14 x 64) did not.
//                             Note: even at scale 1.0 widescreen handheld renders 1638 (not a multiple of 64) -> 1600 with the alignment.
// Log: "[d3drs] installed ...", "[d3drs] 3D target ... align ..." once the family is known, "[d3drs] 10s: ..." every 10 s while the
// world renders (scale now / min / avg / max, rect, GPU time), "[d3drs] gear: ..." on a live change.
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { DRS_TICK = 0x3cd10, DRS_GPU_US = 0xeaf60, DRS_OPT = 0x4c4b40, DRS_TARGET_SIZE = 0xebac0, DRS_SCENE = 0x43c14,
       DRS_GOT_CORE = 0x114a838, DRS_GOT_WIN = 0x114c970, DRS_WIN = 0x179bc58, DRS_OPT_OFF = 0x79 };
static const uint32_t k_tick[102] = {   // main 0x3cd10..0x3cea8, the whole controller
    0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xd0008868, 0xf9441d08, 0xaa0003f3, 0xf9400100, 0x9402b88d, 0xb9401668, 0x92400909, 0x8b090a69,
    0xb9002520, 0x31000509, 0xb9001669, 0x54000400, 0xb9402668, 0x7100053f, 0x54000409, 0xb9402a6a, 0x0b080148, 0x7100093f, 0x540003a0,
    0xb9402e6a, 0x0b080148, 0x7100113f, 0x54000683, 0xb940326a, 0x0b080148, 0x7100113f, 0x540002a0, 0xb940366a, 0x0b080148, 0x7100193f,
    0x54000823, 0xb9403a6a, 0x0b080148, 0x7100193f, 0x540001a0, 0xb9403e6a, 0x0b080148, 0x7100213f, 0x54000763, 0xb9404269, 0x0b080128,
    0x321d03e9, 0x14000005, 0x2a1f03e8, 0x320003e9, 0x14000004, 0x320003e9, 0x7100053f, 0x1a9f8529, 0x1ac90908, 0x52800f20 /* mov w0,#0x79 */,
    0xb9002268, 0x94121f55 /* bl 0x4c4b40 */, 0x340000e0, 0x32091be8, 0xb9001a68, 0xb9000a7f, 0xa9417bfd, 0xf84207f3, 0xd65f03c0, 0xb9400a68,
    0x340000c8, 0x51000508, 0xb9000a68, 0xa9417bfd, 0xf84207f3, 0xd65f03c0, 0xb9402268, 0xb9400269, 0x6b09011f, 0x540000c2, 0x2d430660,
    0x1e202820, 0x14000008, 0x320007e9, 0x17ffffe4, 0xb9400669, 0x6b09011f, 0x540000e9, 0x2d430660, 0x1e213800, 0x321d03e8, 0xbd001a60,
    0xb9000a68, 0x14000002, 0xbd401a60, 0x2d418a61, 0x1e222000, 0x1e20cc42, 0x1e212000, 0x1e224c20, 0xbd001a60, 0xa9417bfd, 0xf84207f3,
    0xd65f03c0, 0x528000a9, 0x17ffffcf, 0x32000be9, 0x17ffffcd};
static const uint32_t k_gpu_us[4] = {0xd0008308, 0xf944b908, 0x528c1509, 0xb8696909};
static const uint32_t k_opt[4] = {0xd0006428, 0xf9442108, 0x93407c09, 0x8b490d08};
static const uint32_t k_target_size[8] = {0xb0008309, 0xf944b929, 0xb9401028, 0x2940292b, 0x6b0b011f, 0x54000081, 0xb940142c, 0x6b0a019f};
// 0x43c14: the scene pass's rect = (u32)(scale x w), (u32)(scale x h) - what the alignment assumes
static const uint32_t k_scene[15] = {0xbd40e3e0, 0xbd40a3e1, 0x910383e1, 0x7e21d821, 0xbd401a82 /* ldr s2,[x20,#0x18] */, 0x7e21d800,
    0x1e200840 /* fmul */, 0x1e210841, 0x1e390028 /* fcvtzu */, 0x1e390009, 0xf9400260, 0xf9000fff, 0xf90073f6, 0xaa1703f3, 0x290423e9};

typedef uint64_t (*drs_gpu_us_t)(void *core);
typedef int32_t (*drs_opt_t)(int32_t id);
typedef void (*drs_target_size_t)(void *core, const void *target, uint32_t *w, int32_t *h);

static uint8_t *g_base;
static atomic_int g_drs_off;            // 1: always 1.0 (gear / DM_D3_DRS=off)
static int g_align_env = -1;            // DM_D3_DRS_ALIGN: -1 auto (by GPU family), 0 off, else the multiple
static int g_align = 0, g_align_known;  // the alignment in force; set once the family is known (auto)
static int g_align_ok;                  // the scene pass is as expected (k_scene): the alignment may act
static struct { const uint32_t *s; float ctl; } g_slot[4];   // the controller's own (unaligned) scale per state
static struct { uint64_t t0; unsigned frames; double sum; float lo, hi; uint64_t gpu_sum; unsigned changes; float last; } g_win;

static uint64_t drs_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void *drs_core(void) { return *(void **)*(uint8_t **)(g_base + DRS_GOT_CORE); }
static float *drs_slot(const uint32_t *s) {
    for (unsigned i = 0; i < 4; i++) if (g_slot[i].s == s) return &g_slot[i].ctl;
    for (unsigned i = 0; i < 4; i++) if (!g_slot[i].s) { float f; memcpy(&f, &s[6], 4); g_slot[i].s = s; g_slot[i].ctl = f; return &g_slot[i].ctl; }
    return NULL;   // more than 4 views: the state's own scale (no shadow)
}
// The scene pass's 3D target width / height for a window-sized target, exactly as the game gets it (0xebac0: x 0.8 in handheld)
static void drs_target(uint32_t *w, int32_t *h) {
    const uint32_t *win = (const uint32_t *)(g_base + DRS_WIN);
    uint8_t fake[0x18] = {0}; memcpy(fake + 0x10, &win[0], 4); memcpy(fake + 0x14, &win[1], 4);   // 0xebac0 reads only +0x10 / +0x14
    *w = 0; *h = 0;
    ((drs_target_size_t)(void *)(g_base + DRS_TARGET_SIZE))(drs_core(), fake, w, h);
}
__attribute__((weak)) int xbg_gpu_policy_family(void);   // xbg_game.cpp: -1 until the device is made, 1 = Adreno (XbgGpuFamily)
static int drs_align_now(void) {
    if (!g_align_known) {
        const int fam = g_align_env >= 0 ? 0 : xbg_gpu_policy_family ? xbg_gpu_policy_family() : 2 /* no Vulkan engine: not Adreno */;
        if (g_align_env < 0 && fam < 0) return 0;
        g_align_known = 1;
        g_align = !g_align_ok ? 0 : g_align_env >= 0 ? g_align_env : fam == 1 ? 0 : 64;
        uint32_t w; int32_t h; drs_target(&w, &h);
        LOG("[d3drs] 3D target %ux%d (window %ux%u); width alignment %d (%s)%s", w, (int)h, ((const uint32_t *)(g_base + DRS_WIN))[0],
            ((const uint32_t *)(g_base + DRS_WIN))[1], g_align, g_align_env >= 0 ? "DM_D3_DRS_ALIGN" : fam == 1 ? "default: Adreno, off" : "default: not Adreno, 64",
            g_align_ok ? "" : " [scene pass main+0x43c14 differs: alignment off]");
    }
    return g_align;
}
static void drs_log_window(const uint32_t *s, float out, uint64_t now) {
    if (!g_win.t0) { g_win.t0 = now; g_win.lo = g_win.hi = out; g_win.last = out; }
    g_win.frames++; g_win.sum += out; g_win.gpu_sum += s[8];
    if (out < g_win.lo) g_win.lo = out;
    if (out > g_win.hi) g_win.hi = out;
    if (out != g_win.last) { g_win.changes++; g_win.last = out; }
    if (now - g_win.t0 < 10000000000ull) return;
    uint32_t w; int32_t h; drs_target(&w, &h);
    float lo, hi, step; memcpy(&lo, &s[3], 4); memcpy(&hi, &s[4], 4); memcpy(&step, &s[7], 4);
    LOG("[d3drs] 10s: DRS %s, scale now %.3f (min %.3f avg %.3f max %.3f, %u changes) -> 3D %ux%u of %ux%d%s, GPU %.2f ms avg "
        "(game target %.2f..%.2f ms, step %.2f, range %.2f..%.2f), %u frames", atomic_load(&g_drs_off) ? "off (gear / DM_D3_DRS=off)" : "auto",
        out, g_win.lo, g_win.frames ? g_win.sum / g_win.frames : 0.0, g_win.hi, g_win.changes, (uint32_t)(out * (float)w), (uint32_t)(out * (float)h), w, (int)h,
        g_align ? " aligned" : "", g_win.frames ? g_win.gpu_sum / 1000.0 / g_win.frames : 0.0, s[0] / 1000.0, s[1] / 1000.0, step, lo, hi, g_win.frames);
    memset(&g_win, 0, sizeof g_win);
}
// The replacement of main 0x3cd10 (void controller(state *s)), on the game's render thread once a frame
static void drs_tick(uint32_t *s) {
    const uint32_t us = (uint32_t)((drs_gpu_us_t)(void *)(g_base + DRS_GPU_US))(drs_core());
    uint32_t n = s[5];
    s[9 + (n & 7)] = us;
    s[5] = ++n;
    uint32_t sum = 0, cnt = 1;
    if (n) { cnt = n < 8 ? n : 8; for (uint32_t i = 0; i < cnt; i++) sum += s[9 + i]; }
    s[8] = sum / cnt;
    float scale;
    if (((drs_opt_t)(void *)(g_base + DRS_OPT))(DRS_OPT_OFF)) {   // the game's own "variable resolution off": 1.0 and the scene pass ignores it
        scale = 1.0f; memcpy(&s[6], &scale, 4); s[2] = 0; return;
    }
    float *ctl = drs_slot(s), own; memcpy(&own, &s[6], 4);
    if (!ctl) ctl = &own;
    if (atomic_load_explicit(&g_drs_off, memory_order_relaxed)) { *ctl = 1.0f; s[2] = 0; }   // the cooldown cleared: auto resumes at once
    else if (s[2]) s[2]--;
    else {
        float c = *ctl, lo, hi, step; memcpy(&lo, &s[3], 4); memcpy(&hi, &s[4], 4); memcpy(&step, &s[7], 4);
        if (s[8] < s[0]) { c += step; s[2] = 8; }
        else if (s[8] > s[1]) { c -= step; s[2] = 8; }
        *ctl = c < lo ? lo : c > hi ? hi : c;   // 0x3ce74: fcsel max, then min
    }
    scale = *ctl;
    const int a = drs_align_now();
    if (a > 0) {
        uint32_t w; int32_t h; drs_target(&w, &h);
        if (w >= 256 && w <= 8192) {
            const uint32_t px = (uint32_t)(scale * (float)w), al = px / (uint32_t)a * (uint32_t)a;
            if (al >= (uint32_t)a && al != px) scale = ((float)al + 0.5f) / (float)w;   // (u32)(scale x w) = al at the scene pass
        }
    }
    memcpy(&s[6], &scale, 4);
    drs_log_window(s, scale, drs_now());
}

void dm_d3_drs_set(int on);   // jni_android.c nativeSetD3Drs (gear, live)
void dm_d3_drs_set(int on) {
    atomic_store(&g_drs_off, on ? 0 : 1);
    LOG("[d3drs] gear: game dynamic resolution %s%s", on ? "on (auto)" : "off (always full resolution)", g_base ? "" : " [controller not hooked: no effect]");
}
void dm_d3_drs_install(void);   // main.c host_preload, after dm_d3_wide_install (before any guest code runs)
void dm_d3_drs_install(void) {
    const char *e = getenv("DM_D3_DRS"), *al = getenv("DM_D3_DRS_ALIGN");
    const int off = e && (!strcmp(e, "off") || !strcmp(e, "0"));
    if (e && *e && !off && strcmp(e, "auto") && strcmp(e, "1")) LOG("【红】[d3drs] DM_D3_DRS=%s unknown (auto / off): auto", e);
    atomic_store(&g_drs_off, off);
    if (al && *al && strcmp(al, "auto")) {
        long v = atol(al);
        g_align_env = v <= 0 ? 0 : v < 16 ? 16 : v > 256 ? 256 : (int)v;
        if (v > 0 && v != g_align_env) LOG("[d3drs] DM_D3_DRS_ALIGN=%s out of 16..256: %d", al, g_align_env);
    }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < DRS_GPU_US + sizeof k_gpu_us || m->text_end < DRS_OPT + sizeof k_opt || m->size < DRS_WIN + 8 ||
        memcmp(m->base + DRS_TICK, k_tick, sizeof k_tick) || memcmp(m->base + DRS_GPU_US, k_gpu_us, sizeof k_gpu_us) ||
        memcmp(m->base + DRS_OPT, k_opt, sizeof k_opt) || memcmp(m->base + DRS_TARGET_SIZE, k_target_size, sizeof k_target_size)) {
        LOG("【红】[d3drs] main is not D3 update 2.7.7 (build 2607a74f): the game's dynamic resolution untouched (DM_D3_DRS / _ALIGN have no effect)"); return; }
    const uint64_t core_got = *(uint64_t *)(m->base + DRS_GOT_CORE);
    if (*(uint64_t *)(m->base + DRS_GOT_WIN) != (uint64_t)(uintptr_t)(m->base + DRS_WIN) || core_got < (uint64_t)(uintptr_t)m->base ||
        core_got >= (uint64_t)(uintptr_t)m->base + m->size) {
        LOG("【红】[d3drs] GOT main+%#x / +%#x not as expected: the game's dynamic resolution untouched", DRS_GOT_WIN, DRS_GOT_CORE); return; }
    g_align_ok = !memcmp(m->base + DRS_SCENE, k_scene, sizeof k_scene);
    g_base = m->base;
    const uint64_t to = (uint64_t)(uintptr_t)drs_tick;
    const uint32_t jmp[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};   // 0x3cd10 is 8-aligned
    uint32_t *at = (uint32_t *)(m->base + DRS_TICK);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + 4);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) { g_base = NULL; LOG("【红】[d3drs] mprotect RW failed: the game's dynamic resolution untouched"); return; }
    for (int i = 0; i < 4; i++) at[i] = jmp[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[d3drs] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    LOG("[d3drs] installed: controller main+%#x replaced (same logic: GPU time 14.25..15.25 ms, step 0.05, scale 0.70..1.00); DM_D3_DRS=%s "
        "(gear 'Dynamic resolution', live); DM_D3_DRS_ALIGN=%s%s", DRS_TICK, off ? "off: always 1.0" : "auto: the game's own",
        g_align_env < 0 ? "auto (64 except Adreno, decided when the GPU is known)" : al, g_align_ok ? "" : " [scene pass main+0x43c14 differs: no alignment]");
}
#endif
