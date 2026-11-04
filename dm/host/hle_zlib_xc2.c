// [XC2] nn::util::DecompressZlib (the SDK's zlib 1.2.8, one buffer -> one buffer) answered by libdeflate (MIT, third_party/libdeflate;
// NEON adler32, a decoder built for whole buffers). main imports it once (XBC1 containers of the .ard / .wismt / .wilay data);
// main's own inflate copy (no version string) is not touched. Same idea as the DOOM branch's zlib-ng gate (DOOM Mobile repository, c48e3d8,
// hle_zlib.c), simpler here because the SDK call is one-shot: no z_stream state to mirror.
//
// Switches (read once):
//   DM_ZLIB_FAST=1     libdeflate instead of the SDK code (default 0 until checked on the phone: the SDK function runs as before)
//   DM_ZLIB_STATS=1    every 5 s a "[zlib]" line: calls, MB in / out, ms spent (either decoder), failures / fallbacks
//   DM_ZLIB_VERIFY=<n> (implies FAST) the first n calls are also decoded by the SDK into a scratch buffer; the result and the output
//                      bytes are compared ("[zlib] VERIFY ..."), and the SDK's answer is the one returned for those calls
// Any libdeflate failure (bad data, output larger than the buffer) falls back to the SDK function, so its answer stays authoritative.
#ifdef DM_GAME_XC2
#include "dm.h"
#include "hle.h"
#include "../third_party/libdeflate/libdeflate.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XZ_SYM "_ZN2nn4util14DecompressZlibEPvmPKvmS1_m"
typedef bool (*xz_fn)(void *, size_t, const void *, size_t, void *, size_t);
static xz_fn g_orig;
static int g_fast = -1, g_stats, g_verify_n;
static _Atomic uint64_t g_calls, g_in, g_out, g_ns, g_fail, g_verified, g_mismatch, g_last_log;
static __thread struct libdeflate_decompressor *t_dec;

static uint64_t xz_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static void xz_init(void) {
    if (g_fast >= 0) return;
    const char *f = getenv("DM_ZLIB_FAST"), *s = getenv("DM_ZLIB_STATS"), *v = getenv("DM_ZLIB_VERIFY");
    g_verify_n = v ? atoi(v) : 0;
    g_stats = s && atoi(s) != 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) g_orig = (xz_fn)module_sym(&g_mod[i], XZ_SYM);
    if (!g_orig) die("[zlib] sdk has no " XZ_SYM);
    g_fast = (f && atoi(f) != 0) || g_verify_n > 0;
    LOG("[zlib] nn::util::DecompressZlib: %s%s%s", g_fast ? "libdeflate" : "SDK zlib 1.2.8 (DM_ZLIB_FAST=1 for libdeflate)",
        g_verify_n > 0 ? ", verify against the SDK" : "", g_stats ? ", stats" : "");
}

static void xz_log(void) {
    uint64_t now = xz_now(), last = atomic_load(&g_last_log);
    if (now - last < 5000000000ull || !atomic_compare_exchange_strong(&g_last_log, &last, now)) return;
    LOG("[zlib] %s: %llu calls, %.1f MB in -> %.1f MB out, %.1f ms decoding, %llu fallbacks, verified %llu (mismatch %llu)",
        g_fast ? "libdeflate" : "SDK", (unsigned long long)g_calls, g_in / 1048576.0, g_out / 1048576.0, g_ns / 1e6,
        (unsigned long long)g_fail, (unsigned long long)g_verified, (unsigned long long)g_mismatch);
}

HLE_FORCE(XZ_SYM, "XBC1 inflate: libdeflate (NEON) instead of the SDK's zlib 1.2.8 behind DM_ZLIB_FAST; SDK code otherwise",
          bool, h_util_DecompressZlib, void *dst, size_t dst_size, const void *src, size_t src_size, void *work, size_t work_size) {
    xz_init();
    uint64_t t0 = g_stats ? xz_now() : 0;
    bool ok;
    size_t produced = 0;
    if (!g_fast) {
        ok = g_orig(dst, dst_size, src, src_size, work, work_size);
    } else {
        if (!t_dec) t_dec = libdeflate_alloc_decompressor();
        enum libdeflate_result r = t_dec ? libdeflate_zlib_decompress(t_dec, src, src_size, dst, dst_size, &produced) : LIBDEFLATE_BAD_DATA;
        uint64_t n = atomic_fetch_add(&g_verified, 0);
        if (g_verify_n > 0 && n < (uint64_t)g_verify_n) {
            atomic_fetch_add(&g_verified, 1);
            void *mine = malloc(dst_size ? dst_size : 1);
            if (mine) memcpy(mine, dst, dst_size);
            ok = g_orig(dst, dst_size, src, src_size, work, work_size);   // the SDK's answer is returned for verified calls
            bool same = mine && ok == (r == LIBDEFLATE_SUCCESS) && (!ok || !memcmp(mine, dst, produced));
            if (!same) atomic_fetch_add(&g_mismatch, 1);
            if (!same || n < 4 || n + 1 == (uint64_t)g_verify_n)
                LOG("%s[zlib] VERIFY #%llu in %zu out-space %zu: SDK %s, libdeflate %d (%zu bytes) -> %s", same ? "" : "【红】",
                    (unsigned long long)n, src_size, dst_size, ok ? "ok" : "fail", (int)r, produced, same ? "same" : "MISMATCH");
            free(mine);
        } else if (r == LIBDEFLATE_SUCCESS) {
            ok = true;
        } else {
            atomic_fetch_add(&g_fail, 1);
            static int told; if (told++ < 8) LOG("[zlib] libdeflate %d on %zu -> %zu bytes: SDK fallback", (int)r, src_size, dst_size);
            ok = g_orig(dst, dst_size, src, src_size, work, work_size);
        }
    }
    if (g_stats) {
        atomic_fetch_add(&g_calls, 1); atomic_fetch_add(&g_in, src_size); atomic_fetch_add(&g_out, produced ? produced : (ok ? dst_size : 0));
        atomic_fetch_add(&g_ns, xz_now() - t0); xz_log();
    }
    return ok;
}
#endif
