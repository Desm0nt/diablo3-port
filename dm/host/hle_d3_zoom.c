// [D3] Camera zoom-out (D3 issue #31: near-square foldable screens see too little around the hero). Gear "Camera zoom-out" 100..140 %.
//
// The game's perspective builder (update 2.7.7, build 2607a74f) main 0x29b7a0(x0 = out 4x4, w1 = flag, s0 = fovY rad, s1 = aspect,
// s2 = near, s3 = far): t = tanf(fovY / 2) (PLT 0xc71980 -> GOT 0x1149ba0), m11 = 1 / t, m00 = m11 / aspect, m22 = -far / (far - near),
// m23 = -1, m32 = m22 x near; flag 0: m22 = +far / (far - near), m23 = +1. The gameplay camera reaches it from 0x264a40 (bl at 0x264a84,
// called by the camera update 0x265070 with the camera's aspect +0x48, fovY +0x4c, near / far +0x54 / +0x58); a second caller
// (0x12d7a0, a fixed 35 degrees) is another view and stays as it is.
//
// The builder is replaced by perspective() below (the same stores in the same order, the game's own tanf). For the gameplay camera's
// call (return address main+0x264a88) t is multiplied by the zoom factor k: k = 1.3 shows 1.3 times as much both across and up/down
// (a wider FOV, not a camera moved back: at 140 % the screen edges get a slightly stronger perspective stretch). The UI is drawn in its
// own space and does not change. Culling follows the matrix as far as Hor+ widescreen showed (no holes at 2.22:1); actors still appear
// in the game's own radius around the hero, hence the 140 % cap.
//   DM_D3_ZOOM=<percent>   100..140 (default 100 = the game's own camera; nothing is patched at 100 unless the gear can change it live)
//   live: dm_d3_zoom_set(percent) from the gear (DmActivity.nativeSetD3Zoom)
// Log: "[d3zoom] installed ..." once, "[d3zoom] gear: ..." on a live change, "[d3zoom] first gameplay camera: fovY ..." once.
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { ZM_PERSP = 0x29b7a0, ZM_GOT_TANF = 0x1149ba0, ZM_RET_GAMEPLAY = 0x264a88, ZM_MIN = 100, ZM_MAX = 140 };
static const uint32_t k_persp[40] = {   // main 0x29b7a0..0x29b83c, the whole builder
    0xfc1c0fea, 0x6d0123e9, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0x4ea11c2a, 0x1e2c1001, 0x1e210800, 0x2a0103f4, 0x4ea31c68, 0x4ea21c49,
    0xaa0003f3, 0x9427586c, 0x1e2e1001, 0x1e201820, 0x1e293901, 0xa901fe7f, 0xbd001660, 0x1e2a1802, 0x52b7f008, 0xf800c27f, 0xf800427f,
    0xb900367f, 0xbd000262, 0x1e211900, 0x1e214001, 0xbd002a61, 0x1e290821, 0xf802c268, 0xbd003a61, 0xb9003e7f, 0x35000094, 0x32091be8,
    0xbd002a60, 0xb9002e68, 0x6d4123e9, 0xa9437bfd, 0xa9424ff4, 0xfc4407ea, 0xd65f03c0};
static const uint32_t k_ret[2] = {0x9400db47 /* bl 0x29b7a0 at 0x264a84 */, 0xb9429e88};

typedef float (*zm_tanf_t)(float);
static zm_tanf_t g_tanf;
static uintptr_t g_ret_gameplay;
static _Atomic int g_zoom_pct = ZM_MIN;   // 100..140
static atomic_int g_logged;

static int zm_clamp(long v) { return v < ZM_MIN ? ZM_MIN : v > ZM_MAX ? ZM_MAX : (int)v; }

// 0x29b7a0, store for store (offsets in bytes of the 4x4 float matrix)
__attribute__((noinline)) static void perspective(float *o, uint32_t flag, float fov, float aspect, float zn, float zf) {
    float t = g_tanf(fov * 0.5f);
    if ((uintptr_t)__builtin_return_address(0) == g_ret_gameplay) {
        const int pct = atomic_load_explicit(&g_zoom_pct, memory_order_relaxed);
        if (pct != ZM_MIN) t *= (float)pct / 100.0f;
        if (!atomic_exchange(&g_logged, 1))
            LOG("[d3zoom] first gameplay camera: fovY %.2f deg, aspect %.4f, near %.2f, far %.1f; zoom-out %d %%", fov * 57.29578f, aspect, zn, zf, pct);
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

void dm_d3_zoom_set(int pct);   // jni_android.c nativeSetD3Zoom (gear, live)
void dm_d3_zoom_set(int pct) {
    const int v = zm_clamp(pct), was = atomic_exchange(&g_zoom_pct, v);
    if (v != was) LOG("[d3zoom] gear: camera zoom-out %d %%%s", v, g_tanf ? "" : " [perspective not hooked: no effect]");
}

void dm_d3_zoom_install(void);   // main.c host_preload, after dm_d3_drs_install (before any guest code runs)
void dm_d3_zoom_install(void) {
    const char *e = getenv("DM_D3_ZOOM");
    const int pct = e && *e ? zm_clamp(atol(e)) : ZM_MIN;
    atomic_store(&g_zoom_pct, pct);
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < ZM_PERSP + sizeof k_persp || m->size < ZM_GOT_TANF + 8 ||
        memcmp(m->base + ZM_PERSP, k_persp, sizeof k_persp) || memcmp(m->base + ZM_RET_GAMEPLAY - 4, k_ret, sizeof k_ret)) {
        LOG("【红】[d3zoom] main is not D3 update 2.7.7 (build 2607a74f): camera zoom-out not available"); return; }
    const uint64_t tf = *(uint64_t *)(m->base + ZM_GOT_TANF);
    if (!tf) { LOG("【红】[d3zoom] GOT main+%#x (tanf) is empty: camera zoom-out not available", ZM_GOT_TANF); return; }
    g_tanf = (zm_tanf_t)(uintptr_t)tf;
    g_ret_gameplay = (uintptr_t)(m->base + ZM_RET_GAMEPLAY);
    const uint64_t to = (uint64_t)(uintptr_t)perspective;
    const uint32_t jmp[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};   // 0x29b7a0 is 8-aligned
    uint32_t *at = (uint32_t *)(m->base + ZM_PERSP);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + 4);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) { g_tanf = NULL; LOG("【红】[d3zoom] mprotect RW failed: camera zoom-out not available"); return; }
    for (int i = 0; i < 4; i++) at[i] = jmp[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[d3zoom] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    LOG("[d3zoom] installed: perspective main+%#x replaced (same matrix, the game's tanf); gameplay camera (call main+%#x) zoom-out %d %% "
        "(DM_D3_ZOOM %d..%d, gear 'Camera zoom-out', live)", ZM_PERSP, ZM_RET_GAMEPLAY - 4, pct, ZM_MIN, ZM_MAX);
}
#endif
