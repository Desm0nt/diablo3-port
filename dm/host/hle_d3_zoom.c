// [D3] Camera zoom-out (D3 issue #31: near-square foldable screens see too little around the hero). Gear "Camera zoom-out" 100..140 %.
//
// 1. The camera moved back (DM_D3_ZOOM, the gear's slider). The gameplay camera's place comes from data, not code: the Observer assets
//    (Observer\*.obs in ClientCommon.cpk). The camera picks GlobalSNO 0x42d consoleObserver (0x42f consoleObserverRetro in the retro mode;
//    0x265ff0 / 0x2661a0), and every frame its modifiers copy the asset's eye offset V0 (+0x30) and look-at offset V1 (+0x3c) relative to
//    the hero (0x26c0f0: SNOGet(mgr, sno) -> +0x14 fovY, +0x18 far, +0x1c near, +0x30 V0, +0x3c V1; 0x26cc20 re-reads them too), then
//    eye = hero + V0 (+ its unit vector), target = hero + V1 (look-at 0xa358f0). consoleObserver: fovY 35, V0 (44.45, 44.45, 49.49),
//    V1 (-3, -3, 0), distance 80 (+0x2c, not read by the client paths found); the PC's observer is 98 away (k 1.225 = the PC camera at the
//    console's pitch). So the zoom-out writes V0 = V1 + (V0_orig - V1) x k into the resident asset data (the game's heap, RW) of 0x42d,
//    0x42f, 0x433 console_wide, 0x434 console_wide_retro: same pitch, same framing of the hero, same FOV; the camera follows on its next
//    frame. Scripted cameras (console_zoom 0x432, cutscenes) and the PC observer 0x42c (the server's player view, with a 2x radius margin:
//    0x665860) are not touched. Lookups are the games's own fast paths re-done as plain reads (no guest call: SNOGet's slow path loads):
//      GlobalSNOGet 0x4e5f50: sno = [[[[GOT 0x114a840] + 0x1100]] + id x 16 + 8]
//      SNOGet 0x6c4a40(mgr = [[GOT 0x114cdf0]], sno): sno < [[GOT 0x11572e8]], i = [[GOT 0x11572f0]][sno] != -1,
//                                                     data = [[[mgr + 0x20] + 0x120] + (i & 0x3ffff) x 16 + 8], [data] == sno
//    Applied from the npad poll (the game's main thread, dm_d3_zoom_pump) about once a second and at once after a gear change; an asset
//    reloaded (its V0 no longer what was written) is taken as the new original.
//    DM_D3_ZOOM=<percent> 100..140 (default 100: the assets are not touched); live: dm_d3_zoom_set (DmActivity.nativeSetD3Zoom).
// 2. Kept in reserve, not in the gear: a wider FOV (DM_D3_FOV_ZOOM=<percent> 100..140, unset = not installed). The perspective builder
//    main 0x29b7a0(x0 = out 4x4, w1 = flag, s0 = fovY rad, s1 = aspect, s2 = near, s3 = far): t = tanf(fovY / 2) (PLT 0xc71980 -> GOT
//    0x1149ba0), m11 = 1 / t, m00 = m11 / aspect, m22 = -far / (far - near), m23 = -1, m32 = m22 x near; flag 0: m22 = +far / (far - near),
//    m23 = +1. Replaced by perspective() below (the same stores in the same order, the game's own tanf; 96 vectors word for word equal to
//    the original run in unicorn); for the gameplay camera's call (0x264a84, return main+0x264a88) t is multiplied by k (k = 1.3 shows 1.3
//    times as much across and up/down; the screen edges stretch a little). The fixed 35-degree view (0x12d7a0) stays as it is.
// Log: "[d3zoom] observer ..." once per asset found, "[d3zoom] gear: ..." on a live change, "[d3zoom] fov installed ..." (reserve only).
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { ZM_MIN = 100, ZM_MAX = 140 };
static int zm_clamp(long v) { return v < ZM_MIN ? ZM_MIN : v > ZM_MAX ? ZM_MAX : (int)v; }

// ---- 1. The camera moved back: the Observer assets' eye offset
enum { ZD_GOT_GLOBALS = 0x114a840, ZD_GLOBALS_SNO = 0x1100, ZD_GOT_MGR = 0x114cdf0, ZD_GOT_SNO_COUNT = 0x11572e8, ZD_GOT_SNO_INDEX = 0x11572f0,
       ZD_GLOBALSNOGET = 0x4e5f50, ZD_SNOGET = 0x6c4a40, ZD_COPY = 0x26c0f0 };
static const uint32_t k_globalsnoget[8] = {0x3100041f, 0x540000e0, 0xb0006328, 0xf9442108, 0xf9488108, 0xf9400108, 0x8b20d108, 0xb9400900};
static const uint32_t k_snoget[17] = {0xf0005488, 0xf9417508, 0xb9400108, 0x6b01011f, 0x540001c9, 0xf0005488, 0xf9417908, 0xf9400108,
    0xb8615908, 0x3100051f, 0x54000100, 0xf9401009, 0xf9409129, 0x92404508, 0xab081128, 0x54000060, 0xf9400500};
static const uint32_t k_copy[16] = {0xb9401408, 0xb9004668, 0xb9401c08, 0xb9004e68, 0xb9401808, 0xb9005268, 0xb9403008, 0xb9005668,
    0xb9403408, 0xb9005a68, 0xb9403808, 0xb9005e68, 0xb9403c08, 0xb9006268, 0xb9404008, 0xb9006668};   // 0x26c124..0x26c160: the asset's +0x14..+0x40
static const struct { uint32_t id; const char *name; } k_obs[] = {
    {0x42d, "consoleObserver"}, {0x42f, "consoleObserverRetro"}, {0x433, "console_wide"}, {0x434, "console_wide_retro"}};
enum { NOBS = sizeof k_obs / sizeof k_obs[0] };
static struct { uint8_t *data; float v0[3], dist, last[3]; int have, logged; } g_obs[NOBS];
static uint8_t *g_zbase;
static _Atomic int g_dist_pct = ZM_MIN, g_dist_dirty, g_dist_on;

static uint64_t zr64(uint64_t a) { return a ? *(const uint64_t *)(uintptr_t)a : 0; }
static uint32_t zd_sno(uint32_t id) {   // GlobalSNOGet, fast path only (0 = not there yet)
    uint64_t g = zr64((uint64_t)(uintptr_t)(g_zbase + ZD_GOT_GLOBALS)), q = g ? zr64(g + ZD_GLOBALS_SNO) : 0, t = q ? zr64(q) : 0;
    return t ? *(const uint32_t *)(uintptr_t)(t + (uint64_t)id * 16 + 8) : 0;
}
static uint8_t *zd_data(uint32_t sno) {   // SNOGet, fast path only (NULL = not resident: the game's slow path would load it)
    if (!sno || sno == 0xffffffffu) return NULL;
    uint64_t cntp = zr64((uint64_t)(uintptr_t)(g_zbase + ZD_GOT_SNO_COUNT)), idxp = zr64((uint64_t)(uintptr_t)(g_zbase + ZD_GOT_SNO_INDEX));
    uint64_t mgrp = zr64((uint64_t)(uintptr_t)(g_zbase + ZD_GOT_MGR)), mgr = mgrp ? zr64(mgrp) : 0;
    if (!cntp || !idxp || !mgr || sno >= *(const uint32_t *)(uintptr_t)cntp) return NULL;
    uint64_t arr = zr64(idxp); if (!arr) return NULL;
    const uint32_t i = ((const uint32_t *)(uintptr_t)arr)[sno]; if (i == 0xffffffffu) return NULL;
    uint64_t t = zr64(mgr + 0x20), tab = t ? zr64(t + 0x120) : 0; if (!tab) return NULL;
    uint8_t *d = (uint8_t *)(uintptr_t)zr64(tab + (uint64_t)(i & 0x3ffff) * 16 + 8);
    return d && *(const uint32_t *)d == sno ? d : NULL;
}
static void zd_apply(void) {
    const float k = (float)atomic_load(&g_dist_pct) / 100.0f;
    for (int n = 0; n < NOBS; n++) {
        const uint32_t sno = zd_sno(k_obs[n].id); uint8_t *d = zd_data(sno);
        if (!d) continue;
        float *v0 = (float *)(d + 0x30), *v1 = (float *)(d + 0x3c), *dist = (float *)(d + 0x2c);
        if (d != g_obs[n].data || !g_obs[n].have || memcmp(v0, g_obs[n].last, sizeof g_obs[n].last)) {   // first seen or reloaded: its values are the originals
            const float dx = v0[0] - v1[0], dy = v0[1] - v1[1], dz = v0[2] - v1[2], len = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!(len > 10.0f && len < 2000.0f)) { if (!g_obs[n].logged++) LOG("【红】[d3zoom] observer %#x %s: eye offset length %.1f not plausible, left alone", k_obs[n].id, k_obs[n].name, len); continue; }
            g_obs[n].data = d; memcpy(g_obs[n].v0, v0, sizeof g_obs[n].v0); g_obs[n].dist = *dist; g_obs[n].have = 1;
            if (!g_obs[n].logged++) LOG("[d3zoom] observer %#x %s (sno %#x): eye (%.2f, %.2f, %.2f) look-at (%.2f, %.2f, %.2f) distance %.1f, fovY %.1f deg",
                                         k_obs[n].id, k_obs[n].name, sno, v0[0], v0[1], v0[2], v1[0], v1[1], v1[2], *dist, *(const float *)(d + 0x14) * 57.29578f);
        }
        for (int c = 0; c < 3; c++) v0[c] = v1[c] + (g_obs[n].v0[c] - v1[c]) * k;
        *dist = g_obs[n].dist * k;
        memcpy(g_obs[n].last, v0, sizeof g_obs[n].last);
    }
}
void dm_d3_zoom_pump(void);   // hle_hid.c fill(): every npad poll (the game's main thread)
void dm_d3_zoom_pump(void) {
    if (!atomic_load_explicit(&g_dist_on, memory_order_relaxed)) return;
    static unsigned polls;
    const int dirty = atomic_exchange_explicit(&g_dist_dirty, 0, memory_order_relaxed);
    if (!dirty && ++polls % 60) return;   // about once a second, at once after a change
    if (atomic_load(&g_dist_pct) == ZM_MIN) { int any = 0; for (int n = 0; n < NOBS; n++) any |= g_obs[n].have; if (!any) return; }   // nothing changed yet: assets untouched
    zd_apply();
}

void dm_d3_zoom_set(int pct);   // jni_android.c nativeSetD3Zoom (gear, live)
void dm_d3_zoom_set(int pct) {
    const int v = zm_clamp(pct), was = atomic_exchange(&g_dist_pct, v);
    if (v != was) { atomic_store(&g_dist_dirty, 1); LOG("[d3zoom] gear: camera zoom-out %d %%%s", v, atomic_load(&g_dist_on) ? "" : " [not available: no effect]"); }
}

// ---- 2. In reserve: a wider FOV of the gameplay camera (DM_D3_FOV_ZOOM, not in the gear)
enum { ZM_PERSP = 0x29b7a0, ZM_GOT_TANF = 0x1149ba0, ZM_RET_GAMEPLAY = 0x264a88 };
static const uint32_t k_persp[40] = {   // main 0x29b7a0..0x29b83c, the whole builder
    0xfc1c0fea, 0x6d0123e9, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0x4ea11c2a, 0x1e2c1001, 0x1e210800, 0x2a0103f4, 0x4ea31c68, 0x4ea21c49,
    0xaa0003f3, 0x9427586c, 0x1e2e1001, 0x1e201820, 0x1e293901, 0xa901fe7f, 0xbd001660, 0x1e2a1802, 0x52b7f008, 0xf800c27f, 0xf800427f,
    0xb900367f, 0xbd000262, 0x1e211900, 0x1e214001, 0xbd002a61, 0x1e290821, 0xf802c268, 0xbd003a61, 0xb9003e7f, 0x35000094, 0x32091be8,
    0xbd002a60, 0xb9002e68, 0x6d4123e9, 0xa9437bfd, 0xa9424ff4, 0xfc4407ea, 0xd65f03c0};
static const uint32_t k_ret[2] = {0x9400db47 /* bl 0x29b7a0 at 0x264a84 */, 0xb9429e88};

typedef float (*zm_tanf_t)(float);
static zm_tanf_t g_tanf;
static uintptr_t g_ret_gameplay;
static int g_fov_pct = ZM_MIN;
static atomic_int g_fov_logged;

// 0x29b7a0, store for store (offsets in bytes of the 4x4 float matrix)
__attribute__((noinline)) static void perspective(float *o, uint32_t flag, float fov, float aspect, float zn, float zf) {
    float t = g_tanf(fov * 0.5f);
    if ((uintptr_t)__builtin_return_address(0) == g_ret_gameplay) {
        t *= (float)g_fov_pct / 100.0f;
        if (!atomic_exchange(&g_fov_logged, 1))
            LOG("[d3zoom] fov: first gameplay camera: fovY %.2f deg, aspect %.4f, near %.2f, far %.1f; x%d %%", fov * 57.29578f, aspect, zn, zf, g_fov_pct);
    }
    const float m11 = 1.0f / t;
    uint32_t *u = (uint32_t *)o;
    u[6] = u[7] = u[8] = u[9] = 0;            // stp xzr, xzr, [x19, #0x18]
    o[5] = m11;                                // str s0, [x19, #0x14]
    const float m00 = m11 / aspect;
    u[3] = u[4] = 0;                           // stur xzr, [x19, #0xc]
    u[1] = u[2] = 0;                           // stur xzr, [x19, #4]
    u[13] = 0;                                 // str wzr, [x19, #0x34]
    o[0] = m00;
    const float q = zf / (zf - zn);
    o[10] = -q;                                // [0x28]
    u[11] = 0xbf800000u; u[12] = 0;            // stur x8 (= -1.0f), [x19, #0x2c]
    o[14] = -q * zn;                           // [0x38]
    u[15] = 0;                                 // [0x3c]
    if (!flag) { o[10] = q; u[11] = 0x3f800000u; }
}
static void fov_install(module_t *m, int pct) {
    if (m->text_end < ZM_PERSP + sizeof k_persp || m->size < ZM_GOT_TANF + 8 ||
        memcmp(m->base + ZM_PERSP, k_persp, sizeof k_persp) || memcmp(m->base + ZM_RET_GAMEPLAY - 4, k_ret, sizeof k_ret)) {
        LOG("【红】[d3zoom] fov: perspective main+%#x differs: DM_D3_FOV_ZOOM has no effect", ZM_PERSP); return; }
    const uint64_t tf = *(uint64_t *)(m->base + ZM_GOT_TANF);
    if (!tf) { LOG("【红】[d3zoom] fov: GOT main+%#x (tanf) is empty: DM_D3_FOV_ZOOM has no effect", ZM_GOT_TANF); return; }
    g_tanf = (zm_tanf_t)(uintptr_t)tf; g_ret_gameplay = (uintptr_t)(m->base + ZM_RET_GAMEPLAY); g_fov_pct = pct;
    const uint64_t to = (uint64_t)(uintptr_t)perspective;
    const uint32_t jmp[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};   // 0x29b7a0 is 8-aligned
    uint32_t *at = (uint32_t *)(m->base + ZM_PERSP);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + 4);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) { g_tanf = NULL; LOG("【红】[d3zoom] fov: mprotect RW failed: DM_D3_FOV_ZOOM has no effect"); return; }
    for (int i = 0; i < 4; i++) at[i] = jmp[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[d3zoom] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    LOG("[d3zoom] fov installed: perspective main+%#x replaced (same matrix, the game's tanf); gameplay camera (call main+%#x) FOV x%d %% (DM_D3_FOV_ZOOM)",
        ZM_PERSP, ZM_RET_GAMEPLAY - 4, pct);
}

void dm_d3_zoom_install(void);   // main.c host_preload, after dm_d3_drs_install (before any guest code runs)
void dm_d3_zoom_install(void) {
    const char *e = getenv("DM_D3_ZOOM"), *f = getenv("DM_D3_FOV_ZOOM");
    atomic_store(&g_dist_pct, e && *e ? zm_clamp(atol(e)) : ZM_MIN);
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    if (f && *f && zm_clamp(atol(f)) != ZM_MIN) fov_install(m, zm_clamp(atol(f)));
    if (m->text_end < ZD_GLOBALSNOGET + sizeof k_globalsnoget || m->size < ZD_GOT_SNO_INDEX + 8 ||
        memcmp(m->base + ZD_GLOBALSNOGET, k_globalsnoget, sizeof k_globalsnoget) || memcmp(m->base + ZD_SNOGET, k_snoget, sizeof k_snoget) ||
        memcmp(m->base + ZD_COPY + 0x34, k_copy, sizeof k_copy)) {
        LOG("【红】[d3zoom] main is not D3 update 2.7.7 (build 2607a74f): camera zoom-out not available"); return; }
    g_zbase = m->base;
    atomic_store(&g_dist_dirty, 1); atomic_store(&g_dist_on, 1);
    LOG("[d3zoom] camera zoom-out %d %% (DM_D3_ZOOM %d..%d, gear 'Camera zoom-out', live): the Observer assets' eye offset (0x42d / 0x42f / 0x433 / 0x434), "
        "applied from the game's thread once they are loaded", atomic_load(&g_dist_pct), ZM_MIN, ZM_MAX);
}
#endif
