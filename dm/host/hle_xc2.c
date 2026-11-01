// [XC2] Xenoblade Chronicles 2 (SDK 3.5.0): nn:: entry points its main imports that neither the D2R set nor the DOOM ports serve.
// Without them the native SDK client opens a service session the host does not have (hid, time, vi, oe, ...) and aborts.
// Most are ported from nxnative (MIT, https://github.com/seedds/nxnative: dm/host/hle_fs.c, hle_hid.c, hle_account.c, hle_sdk16.c,
// hle_misc.c, hle_offline.c, gfx/hle_vi.c), which runs this game on the Mac; see docs/THIRD_PARTY.md.
#ifdef DM_GAME_XC2
#include "hle.h"
#include <string.h>

#define XC2_PROGRAM_ID 0x0100E95004038000ull

// ---- fs: save data of other programs (the game asks about its own save and, read-only, about others: none here)
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE", "fsp-srv; only this game's own save data exists",
          int, h_x_SaveExisting, uint64_t app, const void *uid) {
    (void)uid; LOG("fs::IsSaveDataExisting program %016llx -> %s", (unsigned long long)app, app == XC2_PROGRAM_ID ? "yes (this game)" : "no");
    return app == XC2_PROGRAM_ID;
}
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", "fsp-srv; the game's save data always exists (mounted on demand)",
          unsigned, h_x_SaveExists, const void *uid) { (void)uid; return 1; }
HLE_FORCE("_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE", "fsp-srv; another program's save data: none",
          Result, h_x_MountSaveRO, const char *name, uint64_t app, const void *uid) {
    (void)uid; LOG("fs::MountSaveDataReadOnly %s program %016llx: none on the host", name ? name : "?", (unsigned long long)app);
    return 0x7d402u;   // fs ResultTargetNotFound
}

// ---- oe
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "display version of update v983040", void, h_x_DispVer, char *v) {
    memset(v, 0, 16); strcpy(v, "2.1.0");
}
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "exit request section; the host exits through the app lifecycle", void, h_x_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "exit request section; the host ignores it", void, h_x_LeaveExit, void) {}
static int g_inact_ext;
HLE_FORCE("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", "auto-sleep extension: recorded only (the phone keeps its own screen timeout)",
          unsigned, h_x_InactIs, void) { return (unsigned)g_inact_ext; }
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "auto-sleep extension: recorded only", void, h_x_InactSet, unsigned on) { g_inact_ext = on & 1; }
HLE_FORCE("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", "auto-sleep extension: recorded only", void, h_x_InactSetU, unsigned on) { g_inact_ext = on & 1; }
HLE_FORCE("_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv", "auto-sleep extension mode: 0 (none)", int, h_x_InactMode, void) { return 0; }

// ---- ec (shop applets: none, offline)
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE", "shop applet: none (offline)", void, h_x_ShopAoc, uint64_t app) {
    LOG("ec::ShowShopAddOnContentList %016llx (no shop, skipped)", (unsigned long long)app);
}
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdE", "shop applet: none (offline)", void, h_x_ShopApp, uint64_t app) {
    LOG("ec::ShowShopApplicationInformation %016llx (no shop, skipped)", (unsigned long long)app);
}

// ---- account
typedef struct { uint64_t v[2]; } Uid;
static const Uid k_x_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // the host's one local user (hle_account.c k_uid)
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "all users: the one local user", Result, h_x_ListAll, int *n, Uid *out, int max) {
    *n = 0; if (max > 0) { out[0] = k_x_uid; *n = 1; } return RES_OK;
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close user: the handle holds no session", void, h_x_CloseUser, const void *h) { (void)h; }

// ---- settings: available languages (Horizon's settings::Language order)
static const char *const k_x_lang[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419"};
HLE_FORCE("_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi", "available language codes (Horizon's table)", int, h_x_Langs, char *out, int count) {
    int n = 0;
    for (; n < count && n < (int)(sizeof k_x_lang / sizeof *k_x_lang); n++) { memset(out + 8 * n, 0, 8); strncpy(out + 8 * n, k_x_lang[n], 8); }
    return n;
}
HLE_FORCE("_ZN2nn8settings29GetAvailableLanguageCodeCountEv", "number of available language codes", int, h_x_LangN, void) { return (int)(sizeof k_x_lang / sizeof *k_x_lang); }

// ---- time: the standard steady clock (SteadyClockTimePoint = {i64 seconds, 16-byte clock source id})
typedef struct { int64_t sec; uint64_t src[2]; } SteadyTP;
static const uint64_t k_x_src[2] = {0x786332636c6f636bull, 0x0000000000000001ull};
HLE_FORCE("_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock: guest uptime in seconds, one fixed source id",
          Result, h_x_SteadyNow, SteadyTP *tp) {
    tp->sec = (int64_t)(guest_ns() / 1000000000ull); tp->src[0] = k_x_src[0]; tp->src[1] = k_x_src[1]; return RES_OK;
}
HLE_FORCE("_ZN2nn4time14GetSpanBetweenEPlRKNS0_20SteadyClockTimePointES4_", "steady clock span (same source only)",
          Result, h_x_Span, int64_t *out, const SteadyTP *a, const SteadyTP *b) {
    if (a->src[0] != b->src[0] || a->src[1] != b->src[1]) return 0xc8074u;   // time ResultNotComparable (module 116, description 200)
    *out = b->sec - a->sec; return RES_OK;
}

// ---- err: the application error applet is logged
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet: logged", void, h_x_AppErr, const uint8_t *arg) {
    LOG("【红】err::ShowApplicationError (arg %p): %.64s", (const void *)arg, arg ? (const char *)arg + 8 : "");
}

// ---- vi teardown (the game calls them when it exits)
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "vi: the display is the app's surface", void, h_x_CloseDisp, void *d) { (void)d; }
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "vi: the layer is the app's surface", void, h_x_DestroyLayer, void *l) { (void)l; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "vi: nothing to tear down", void, h_x_ViFini, void) {}

// ---- audio device names (audren device service)
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "output device list: the phone's one output", int, h_x_DevList, char *names, int n) {
    if (n < 1 || !names) return 0;
    memset(names, 0, 256); strcpy(names, "AudioBuiltInSpeakerOutput"); return 1;
}

// ---- Widescreen (DM_XC2_WIDE, default off until tested on the phone; docs/WIDESCREEN.md)
// XC2 never asks the system for a display size: the NVN window setup (update 2.1.0 main 0x6ffca8) builds its two swap textures
// with the constants "mov w21,#1280 / mov w22,#720" (0x6ffdc0 / 0x6ffdc4; the same site as emoose's resolution patch,
// gist.github.com/emoose/93a81ff4944933cafe64160a91f2acbe) and hands w21/w22 on to the window/framebuffer builder 0x702878.
// Everything else follows that size: the internal targets are framebuffer width x dynamic-resolution scale (0x70a628 -> 0x70c2b4),
// the camera aspect is view width / view height (0x7e308c fdiv, the instruction theboy181's 21:9 pchtxt replaces with a constant)
// and the projection keeps the vertical FOV (0x7e3558: m11 = 1/tan(fov/2), m00 = m11/aspect), so a wider window = Hor+.
// The binary has no 16:9 constant at all. So widening is one immediate: w21 = W. The 2D layer stays in its 1280x720 layout
// space mapped onto the whole framebuffer (expected: HUD stretched by W/1280 horizontally, exactly as the stretched 16:9 present
// showed it before; world-anchored markers stay aligned with the 3D). The presenter needs nothing: it takes the window texture
// size (present_native sets the buffer geometry to W x 720, the compositor scales that to the 20:9 screen without distortion).
//   DM_XC2_WIDE=1        W = 720 x screen aspect (front_game_res(0): the screen size is known, nativeSurface runs before nativeStart)
//   DM_XC2_WIDE=<width>  explicit width (1280..2560, rounded to 8)
//   unset / 0            original 1280x720
// The site is checked word for word (9 words around it, plus the aspect site as a build fingerprint): any other build -> no change.
#include <sys/mman.h>
#include <pthread.h>
#include <stdlib.h>
#include "front.h"
enum { XW_SITE = 0x6ffdb4, XW_W21 = 0x6ffdc0, XW_ASPECT = 0x7e307c };
static const uint32_t k_xw_site[9] = {0xb000247b, 0xf9423b7b, 0xf9400368, 0x5280a015 /* mov w21,#1280 */, 0x52805a16 /* mov w22,#720 */,
                                      0x910003e0, 0x2a1503e1, 0x2a1603e2, 0xd63f0100};
static const uint32_t k_xw_aspect[6] = {0x2d450500, 0x5e21d821, 0x321a03e2, 0x5e21d800, 0x1e211800 /* fdiv s0,s0,s1 */, 0xbd04c260};
static void xd_boot(void);
void dm_xc2_wide_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_xc2_wide_install(void) {
    xd_boot();
    const char *e = getenv("DM_XC2_WIDE");
    if (!e || !*e || (*e == '0' && !e[1])) { LOG("[wide] DM_XC2_WIDE off: window 1280x720"); return; }
    long want = atol(e); unsigned w;
    if (want >= 1280) w = (unsigned)want;
    else { uint64_t r = front_game_res(0); w = (uint32_t)(r >> 32) == 720 ? (uint32_t)r : 1280; }
    w = (w + 4) / 8 * 8; if (w > 2560) w = 2560;
    if (w <= 1280) { LOG("[wide] DM_XC2_WIDE=%s: screen not wider than 16:9 (or unknown): window stays 1280x720", e); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < XW_ASPECT + sizeof k_xw_aspect || memcmp(m->base + XW_SITE, k_xw_site, sizeof k_xw_site) ||
        memcmp(m->base + XW_ASPECT, k_xw_aspect, sizeof k_xw_aspect)) {
        LOG("【红】[wide] main is not XC2 update 2.1.0 (build f77f1559): window size site not found, stays 1280x720"); return; }
    uint32_t *at = (uint32_t *)(m->base + XW_W21);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule (also 4 KB aligned)
    if (mprotect(pg, 0x4000, PROT_READ | PROT_WRITE)) { LOG("【红】[wide] mprotect RW failed: window stays 1280x720"); return; }
    *at = 0x52800000u | (w << 5) | 21;   // mov w21,#W (w22 keeps 720)
    if (mprotect(pg, 0x4000, PROT_READ | PROT_EXEC)) die("[wide] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    LOG("[wide] main+%#x mov w21,#1280 -> #%u: NVN window %ux720 (Hor+ 3D, %.3f:1; 2D layer stretched x%.3f)", XW_W21, w, w,
        w / 720.0, w / 1280.0);
}

// [XC2] Dynamic resolution on / off, live (gear dialog -> nativeSetDrs, or DM_XC2_DRS=0 at boot). The game's DRS step (main 2.1.0
// 0x71b9b0 `tbnz w9,#2,0x71ba84`) takes the fixed-scale branch when bit 2 of its flags is set; theboy181's "Disable Resolution
// Scaling" pchtxt makes that branch unconditional (`b 0x71ba84`). Flipping the one word while the game runs: the page goes
// RWX for the store (never non-executable, other threads may be inside it), then back to RX; a stale I-cache line only delays it a frame.
enum { XD_SITE = 0x71b9b0 };
static const uint32_t k_xd_orig = 0x371006a9, k_xd_fixed = 0x14000035;   // tbnz w9,#2,+0xd4 / b +0xd4
static int g_xd_want = -1;   // -1 = not set yet (the game's own DRS)
static pthread_mutex_t g_xd_lock = PTHREAD_MUTEX_INITIALIZER;
static void xd_apply(void) {
    if (g_xd_want < 0) return;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !m->base || m->text_end < XD_SITE + 4) return;   // modules not loaded yet: dm_xc2_wide_install applies it
    uint32_t *at = (uint32_t *)(m->base + XD_SITE), want = g_xd_want ? k_xd_orig : k_xd_fixed;
    if (*at == want) return;
    if (*at != k_xd_orig && *at != k_xd_fixed) { static int told; if (!told++) LOG("【红】[drs] main+%#x is %#x, not XC2 2.1.0's DRS step: left alone", XD_SITE, *at); return; }
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC)) { LOG("【红】[drs] mprotect RWX failed"); return; }
    __atomic_store_n(at, want, __ATOMIC_RELEASE);
    mprotect(pg, 0x4000, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    LOG("[drs] dynamic resolution %s (main+%#x = %#x)", g_xd_want ? "on (the game's)" : "off (fixed full resolution)", XD_SITE, want);
}
void dm_xc2_set_drs(int on);   // jni_android.c nativeSetDrs (gear dialog, live)
void dm_xc2_set_drs(int on) { pthread_mutex_lock(&g_xd_lock); g_xd_want = on ? 1 : 0; xd_apply(); pthread_mutex_unlock(&g_xd_lock); }
static void xd_boot(void) {
    const char *e = getenv("DM_XC2_DRS");
    pthread_mutex_lock(&g_xd_lock); if (g_xd_want < 0 && e && *e) g_xd_want = atoi(e) != 0; xd_apply(); pthread_mutex_unlock(&g_xd_lock);
}
#endif
