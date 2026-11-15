// [D3] Diablo III: Eternal Collection (SDK 8.2.0, sdk build 3ae34e68): nn:: entry points its main imports that neither the D2R set nor
// the DOOM / Eternal / XC2 ports serve, or serve under another overload. Without them the native SDK client opens a service session
// the host does not have (nfp, ldn, applets, ...) and aborts. The port is offline-only: every network / local-wireless / amiibo /
// shop entry answers "unavailable / none / empty" the way the SDK's callers expect, so the game takes its offline branch.
// The nfp and ldn parts are ported from nxnative (MIT, https://github.com/seedds/nxnative: dm/host/hle_sdk16.c "nfp: amiibo",
// dm/host/hle_offline.c "ldn: local wireless", now replaced by hle_ldn.c); the account / fs / oe entries follow hle_xc2.c (itself from nxnative).
#ifdef DM_GAME_D3
#include "hle.h"
#include <arpa/inet.h>
#include <stdatomic.h>
#include <string.h>

#define D3_PROGRAM_ID 0x01001B300B9BE000ull

// ---- account / fs / oe (as hle_xc2.c)
typedef struct { uint64_t v[2]; } Uid;
static const Uid k_d3_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // the host's one local user (hle_account.c k_uid)
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "all users: the one local user", Result, h_d3_ListAll, int *n, Uid *out, int max) {
    *n = 0; if (max > 0) { out[0] = k_d3_uid; *n = 1; } return RES_OK;
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close user: the handle holds no session", void, h_d3_CloseUser, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", "fsp-srv; the game's save data always exists (host directory, mounted on demand)",
          unsigned, h_d3_SaveExists, const void *uid) { (void)uid; return 1; }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "exit request section; the host exits through the app lifecycle", void, h_d3_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "exit request section; the host ignores it", void, h_d3_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "hide the system startup logo (applet): nothing shown on the host", void, h_d3_StartupLogo, void) { LOG("oe::FinishStartupLogo"); }

// ---- shop / parental controls / microphone
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", "shop applet: none (offline)", void, h_d3_ShopAoc,
          uint64_t app, const void *user) { (void)user; LOG("ec::ShowShopAddOnContentList %016llx (no shop, skipped)", (unsigned long long)app); }
// Result: success = no parental-control restriction on communicating with others (a console with no restrictions set; nothing goes
// online anyway because nifm reports no network)
HLE_FORCE("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", "parental controls: no restriction set (nothing is online anyway)", Result, h_d3_PctlFree, unsigned show_ui) {
    (void)show_ui; return RES_OK;
}
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", "no microphone: open fails (SDK 8 overload without an event)",
          Result, h_d3_OpenAudioIn, void *in, const char *name, const void *param) {
    (void)in; (void)param; LOG("audio::OpenAudioIn %s: no microphone", name ? name : "(default)"); return RES_FAIL;
}

// ---- nifm / socket / friends: what hle_net.c / hle_friends.c do not cover in this SDK
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline: the network request is not submitted (returns at once, IsNetworkAvailable stays false)", void, h_d3_SubmitWait, void) {
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) LOG("nifm::SubmitNetworkRequestAndWait: offline (no network)");
}
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with socket::Initialize (no session)", Result, h_d3_SockFini, void) { return RES_OK; }
// Pure text conversions, taken over only because the SDK versions share code with the service path (ipc_reach.py): host inet_ntop.
// Horizon's address families are FreeBSD's: AF_INET 2, AF_INET6 28.
HLE_FORCE("_ZN2nn6socket8InetNtopEiPKvPcj", "address to text (host inet_ntop, FreeBSD address families)", const char *, h_d3_Ntop, int af, const void *src, char *dst, unsigned n) {
    return inet_ntop(af == 2 ? AF_INET : af == 28 ? AF_INET6 : -1, src, dst, n);
}
HLE_FORCE("_ZN2nn6socket8InetNtoaE7in_addr", "IPv4 address to text (thread-local buffer, as inet_ntoa)", const char *, h_d3_Ntoa, uint32_t a) {
    static __thread char buf[16]; return inet_ntop(AF_INET, &a, buf, sizeof buf);
}
HLE_FORCE("_ZN2nn7friends17NotificationQueue8FinalizeEv", "friend notification queue: no session to close", Result, h_d3_NqFini, void *self) { (void)self; return RES_OK; }

// ---- nfp: amiibo (nxnative hle_sdk16.c, MIT). The host has no NFC reader: one NFC device on the connected controller whose detection
// never finds a tag, as a real console with no amiibo at hand: the game's "touch an amiibo" screen appears as usual and cancelling
// leaves it. Operations that need a tag return "tag not found".
#define NFP_RES_TAG_NOT_FOUND (115u | 97u << 9)   // module 115 (nfp), description 97
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1 };   // 2 = tag found: never on the host
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
static _Atomic int g_nfp_state;
static uint32_t connected_npad(void) { return dm_opmode() == 0 ? 0x20u : 0u; }   // hle_hid.c connected_id: Handheld / No1
static Result nfp_no_tag(const char *what) {
    static _Atomic int logged; if (!atomic_exchange(&logged, 1)) LOG("nfp: %s and other operations that need an amiibo tag -> no tag found (the host has no NFC)", what);
    return NFP_RES_TAG_NOT_FOUND;
}
HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo library init; the host has one NFC device that never finds a tag", void, h_nfp_Init, void) { LOG("nfp::Initialize: one NFC device, no amiibo tag ever"); }
HLE_FORCE("_ZN2nn3nfp8FinalizeEv", "amiibo library finalize; the host has no session", void, h_nfp_Fini, void) {}
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC availability change event; bound, never signalled", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC device list: one device", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "controller the NFC device is on: the one connected now", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) { (void)h; *out = connected_npad(); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "start tag detection (never finds one)", Result, h_nfp_Start, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_SEARCH); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "stop tag detection", Result, h_nfp_Stop, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_INIT); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "device state: initialized / searching for a tag", int, h_nfp_State, const NfpDeviceHandle *h) { (void)h; return atomic_load(&g_nfp_state); }
HLE_FORCE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag placed event; bound, never signalled", Result, h_nfp_AttachAct, void *ev, const NfpDeviceHandle *h) { (void)h; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", "tag removed event; bound, never signalled", Result, h_nfp_AttachDeact, void *ev, const NfpDeviceHandle *h) { (void)h; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "tag info: no tag", Result, h_nfp_TagInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetTagInfo"); }
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "model info: no tag", Result, h_nfp_ModelInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetModelInfo"); }
HLE_FORCE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", "common info: no tag", Result, h_nfp_CommonInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetCommonInfo"); }
HLE_FORCE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", "register info: no tag", Result, h_nfp_RegInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetRegisterInfo"); }
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "mount tag: no tag", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { (void)h; (void)model; (void)target; return nfp_no_tag("Mount"); }
HLE_FORCE("_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", "unmount tag: no tag", Result, h_nfp_Unmount, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Unmount"); }
HLE_FORCE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "flush tag: no tag", Result, h_nfp_Flush, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Flush"); }
HLE_FORCE("_ZN2nn3nfp7RestoreERKNS0_12DeviceHandleE", "restore tag backup: no tag", Result, h_nfp_Restore, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Restore"); }
HLE_FORCE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "open application area: no tag", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) { (void)h; (void)id; return nfp_no_tag("OpenApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "read application area: no tag", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) { (void)buf; (void)h; (void)sz; if (n) *n = 0; return nfp_no_tag("GetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "write application area: no tag", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) { (void)h; (void)buf; (void)sz; return nfp_no_tag("SetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "create application area: no tag", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("CreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo restorer applet: no tag", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartRestorer"); }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo game data eraser applet: no tag", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartGameDataEraser"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo nickname and owner settings applet: no tag",
          Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag) { (void)h; (void)reg; (void)p; (void)tag; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }

// ---- ldn: local wireless: hle_ldn.c (local play over the LAN, DM_LDN; DM_LDN=0 gives the former nxnative answers: Initialize fails, state
// none). MakeIpv4Address / operator== are pure and stay in the SDK.

// ---- Widescreen (DM_D3_WIDE, default off until tested on the phone; docs/d3/WIDESCREEN.md)
// D3 never asks the system for a display size. NX64NVNCore's init (update 2.7.7 main 0xe7830, called once from the Gfx window open
// 0x29a12c) fills its global (GOT 0x114c970 -> 0x179bc58) with constants:
//   0xe7858 mov x8,#1600 / 0xe7860 movk x8,#900,lsl#32  -> +0x00 docked window size 1600x900
//   0xe785c mov x9,#1280 / 0xe7864 movk x9,#720,lsl#32  -> +0x08 handheld size 1280x720
//   0xe7868 mov w10,#0x8e39 / 0xe786c movk w10,#0x3fe3  -> +0x10 aspect 16/9 (1.7777778f)
// Everything else follows them: the three swap textures and nvnWindowBuilderSetTextures (0xe6940) take +0x00 (docked size, always);
// the present (0xeae2c) crops to +0x00 docked / +0x08 handheld; GetCurrentDisplayMode (0xea0e0) reports the mode's size as the
// DisplayMode window / UI-optimal / full sizes and +0x10 as its aspect (DisplayMode at gfx+0x224, aspect at gfx+0x258, GetAspect
// 0x29a7f0). The 3D cameras take that aspect (0x266740 -> camera+0x48 -> 0x29b7a0 perspective: m11 = 1/tan(fovY/2), m00 = m11/aspect,
// vertical FOV => wider = Hor+). The UI (960x720 virtual space) is laid out in the centred 4:3 region of the real window size
// (0x34baf0) with PC-style edge anchoring, so it needs nothing. The only 16:9 constant is the "Letterbox" check (0x29b480: option
// flag and |16/9 - aspect| > 0.01 -> the 3D forced to 16/9); it is made to answer "no" so the option can never squeeze the wide view.
//   DM_D3_WIDE=1        docked width = 900 x screen aspect (front_game_res(1): the screen size is known, nativeSurface runs before nativeStart);
//                       a screen narrower than 16:9 (4:3 tablets, 10:9 / 6:5 foldables, issue #8): 900 x its aspect, at least 1200 = 4:3
//   DM_D3_WIDE=4:3      1200x900 whatever the screen (gear "4:3"; the front end centres it with bars)
//   DM_D3_WIDE=<width>  explicit docked width (1200..3200)
//   unset / 0           original 1600x900 / 1280x720
// Widths are rounded to 16; handheld = docked x 4/5 (720 high), aspect = docked width / 900. All sites are checked word for word.
// Narrower than 16:9 is the PC version's 4:3 / 16:10 behaviour: the vertical FOV stays, so the 3D loses a little at the sides (Hor-, the
// hero stays in the middle); the UI's centred 4:3 region is the whole window at 4:3 (0x34baf0 does nothing when |aspect - 4/3| <= 0.01) and
// within it at 16:10, the edge anchors stay on the screen. Below 4:3 that region (window height x 4/3, centred) would be wider than the
// window and the HUD would leave the screen, so the window stops at 1200x900 and the front end shows it with bars at the top and bottom
// (DmActivity applyAspectLayout fits the SurfaceView to the game's render size). The loading-screen art (16:9) is not cropped there:
// it already covers the window (ls_install only for windows wider than 1600).
#include <math.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include "front.h"
__attribute__((weak)) uint64_t front_screen_size(void);   // front_android.c: (u64)short side << 32 | long side, 0 = unknown
enum { DW_INIT = 0xe7850, DW_LBOX = 0x29b480 };
static const uint32_t k_dw_init[10] = {0xb0008336, 0xf944bad6, 0xd280c808 /* mov x8,#1600 */, 0xd280a009 /* mov x9,#1280 */,
    0xf2c07088 /* movk x8,#900,lsl#32 */, 0xf2c05a09 /* movk x9,#720,lsl#32 */, 0x5291c72a /* mov w10,#0x8e39 */,
    0x72a7fc6a /* movk w10,#0x3fe3,lsl#16 */, 0xb90012ca /* str w10,[x22,#0x10] */, 0xa90026c8 /* stp x8,x9,[x22] */};
static const uint32_t k_dw_lbox[4] = {0xb00075a8 /* adrp x8,0x1150000 */, 0xf9423908 /* ldr x8,[x8,#0x470] */, 0xf9400108, 0x39400109};
static int dw_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[wide] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}
// ---- Loading screens stretched to the screen in widescreen (DM_D3_WIDE_STRETCH_LOADING, default 1 with DM_D3_WIDE; docs/d3/WIDESCREEN.md)
// The loading screen is not a UI control: the main loop (0x40810 -> 0x41418 / 0x408dc, also 0x3d37c / 0x3d510 from blocking loads) calls
// LoadingScreen_Frame 0x3ebc0 with st = *(*(g + 0x10) + 0x800) (g = main+0x1905610):
//   st+0x6c  1 = loading screen up (set at 0x3ecf8 on its first frame, cleared only by 0x3dbd0 at 0x3dc9c when the loop sees loading over)
//   st+0x70  its mode, = global 0x1767bc4 at start: 0 = the act art (0x32e200 loads the textures, 0x3ee18 b 0x32ec60 draws), else a plain
//            window-sized fill (0x3ed8c..0x3edc8, nothing to stretch)
// 0x32ec60 draws the art in UI units (960x720 space): EndCapLeft x -160.2..0 (0x32ecac mov/movk = 0xc3203334) when the full-screen rect
// (0x3d9e0) starts left of 0, the 4x3 centre 0..960, EndCapRight 960..1120.2 (0x32ed74 = 0x448c0666) when it ends right of 960, y 0..720;
// the logo, tips text and loading dots are inside 0..960. -160.2..1120.2 = 1280.4 x 720 = 16:9. UI -> pixels: 0x34bf50, px = ox + ui*sx,
// py = ui*sy with {sx, sy, ox} at 0x1862680 (the centred 4:3 region of the window: 2000x900 -> sx 1.25, ox 400 -> art 199.75..1800.25).
// While st+0x6c && !st+0x70 the present (hle_nvn.c g_nvn_present_crop, game thread) takes only the art's whole-pixel rect (200,0 1600x900)
// inside the game's own crop; vulkan_present.cpp present_native gives the window buffer that size and the compositor scales it to the
// full-screen SurfaceView (non-uniform: stretched, like the 'stretched' picture shape). Every other frame keeps the full window.
enum { LS_G = 0x1905610, LS_GOT_G = 0x114a840, LS_UIMAP = 0x1862680 };
static const struct { uint32_t off, n, w[5]; } k_ls_sites[] = {
    {0x3ebe0, 5, {0xf9442108 /* ldr x8,[x8,#0x840] (GOT g) */, 0xf9400908 /* ldr x8,[x8,#0x10] */, 0xf9440115 /* ldr x21,[x8,#0x800] */,
                  0x97ff2f81 /* bl 0xa9f0 */, 0xb9406ea8 /* ldr w8,[x21,#0x6c] */}},
    {0x3ecf4, 2, {0x320003e8 /* mov w8,#1 */, 0xb9006ea8 /* str w8,[x21,#0x6c] */}},
    {0x3ed84, 3, {0xb94072a9 /* ldr w9,[x21,#0x70] */, 0x290c02a8, 0x340002e9 /* cbz w9 -> art */}},
    {0x3ee18, 1, {0x140bbf92 /* b 0x32ec60 */}},
    {0x32ecac, 2, {0x52866688, 0x72b86408 /* w8 = -160.2f */}},
    {0x32ed74, 2, {0xd280ccc8, 0xf2a89188 /* x8 lo = 1120.2f */}},
    {0x34bf50, 4, {0xf000a8a8, 0x911a0108 /* x8 = 0x1862680 */, 0xbd400800, 0x2d401101 /* ldp s1(sx), s4(sy), [x8] */}},
};
static uint8_t *g_ls_base; static size_t g_ls_size;
static int ls_read(uint64_t a, void *out, size_t n) {
    int dm_guest_heap_readable(uint64_t a, size_t n);
    uint64_t b = (uint64_t)(uintptr_t)g_ls_base;
    if ((a >= b && n <= g_ls_size && a - b <= g_ls_size - n) || dm_guest_heap_readable(a, n)) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    // st (the loading-screen state) is outside the guest heap range the fast path knows: read it through the kernel (a bad pointer
    // gives EFAULT, never a fault) - found on the phone with DM_D3_LS_DEBUG=1 (o read, o+0x800 refused, loading screens kept their bars)
    if (a < 0x10000 || a >= (1ull << 48) || n > 4096) return 0;
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == (ssize_t)n;
}
static void ls_present_crop(int crop[4]) {
    static int was, logged;
    int on = 0, c[4] = {0};
    uint64_t o = 0, st = 0; uint32_t s[2] = {0, 0}; float m[3];
    {   // DM_D3_LS_DEBUG=1: once a second, what the detector reads (o, st, +0x6c, +0x70, the UI map) - to see why a loading screen is missed
        static int dbg = -1; static uint64_t last; if (dbg < 0) { const char *e = getenv("DM_D3_LS_DEBUG"); dbg = e && *e == '1'; }
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); uint64_t now = (uint64_t)ts.tv_sec;
        if (dbg && now != last) { last = now; uint64_t o2 = 0, st2 = 0; uint32_t s2[2] = {9, 9}; float m2[3] = {0, 0, 0};
            int r1 = ls_read((uint64_t)(uintptr_t)(g_ls_base + LS_G) + 0x10, &o2, 8), r2 = o2 && ls_read(o2 + 0x800, &st2, 8),
                r3 = st2 && ls_read(st2 + 0x6c, s2, 8), r4 = ls_read((uint64_t)(uintptr_t)(g_ls_base + LS_UIMAP), m2, sizeof m2);
            LOG("[wide] ls-debug crop %d,%d %dx%d | o %#llx (%d) st %#llx (%d) +0x6c %u +0x70 %u (%d) | sx %.3f sy %.3f ox %.1f (%d)", crop[0], crop[1], crop[2], crop[3],
                (unsigned long long)o2, r1, (unsigned long long)st2, r2, s2[0], s2[1], r3, m2[0], m2[1], m2[2], r4); } }
    if (crop[2] > 0 && crop[3] > 0 && ls_read((uint64_t)(uintptr_t)(g_ls_base + LS_G) + 0x10, &o, 8) && o && ls_read(o + 0x800, &st, 8) && st &&
        ls_read(st + 0x6c, s, 8) && s[0] && !s[1] && ls_read((uint64_t)(uintptr_t)(g_ls_base + LS_UIMAP), m, sizeof m) &&
        isfinite(m[0]) && isfinite(m[1]) && isfinite(m[2]) && m[0] > 0.1f && m[1] > 0.1f) {
        float fl = m[2] - 160.2f * m[0], fr = m[2] + 1120.2f * m[0], fb = 720.0f * m[1];
        int l = (int)ceilf(fl - 0.01f), r = (int)floorf(fr + 0.01f), b = (int)floorf(fb + 0.5f);
        if (l < 0) l = 0;
        if (r > crop[2]) r = crop[2];
        if (b > crop[3]) b = crop[3];
        if (r - l >= 64 && b >= 64 && (l > 0 || r < crop[2])) { on = 1; c[0] = crop[0] + l; c[1] = crop[1]; c[2] = r - l; c[3] = b; }
        if (on != was && logged < 400) { logged++;
            LOG("[wide] loading screen (st %#llx +0x6c %u +0x70 %u): art UI -160.2..1120.2 -> px %.2f..%.2f x 0..%.1f (sx %.4f ox %.1f): %s",
                (unsigned long long)st, s[0], s[1], fl, fr, fb, m[0], m[2], on ? "present crop below, stretched to the screen" : "fills the window, nothing to crop"); }
    }
    // [D3] breadcrumbs (crash reports): each loading screen numbered, with the time since the host started (one line per change)
    static int nth;
    if (on != was && logged < 400) { logged++;
        if (on) LOG("[wide] loading screen #%d at %.1f s: present %d,%d %dx%d of the window crop %d,%d %dx%d, stretched to the screen", ++nth, (now_ns() - g_t0_ns) / 1e9, c[0], c[1], c[2], c[3], crop[0], crop[1], crop[2], crop[3]);
        else LOG("[wide] loading screen #%d over at %.1f s: present the whole window %d,%d %dx%d again", nth, (now_ns() - g_t0_ns) / 1e9, crop[0], crop[1], crop[2], crop[3]); }
    was = on;
    if (on) memcpy(crop, c, sizeof c);
}
static void ls_install(module_t *m) {
    const char *e = getenv("DM_D3_WIDE_STRETCH_LOADING");
    if (e && *e == '0' && !e[1]) { LOG("[wide] DM_D3_WIDE_STRETCH_LOADING=0: loading screens keep their 16:9 art with side bars"); return; }
    for (unsigned i = 0; i < sizeof k_ls_sites / sizeof *k_ls_sites; i++)
        if (m->size < k_ls_sites[i].off + 4 * k_ls_sites[i].n || memcmp(m->base + k_ls_sites[i].off, k_ls_sites[i].w, 4 * k_ls_sites[i].n)) {
            LOG("【红】[wide] main+%#x differs: loading-screen stretch off (not update 2.7.7 build 2607a74f)", k_ls_sites[i].off); return; }
    if (m->size < LS_G + 0x18 || m->size < LS_UIMAP + 12 || *(uint64_t *)(m->base + LS_GOT_G) != (uint64_t)(uintptr_t)(m->base + LS_G)) {
        LOG("【红】[wide] GOT main+%#x does not point at main+%#x: loading-screen stretch off", LS_GOT_G, LS_G); return; }
    g_ls_base = m->base; g_ls_size = m->size;
    extern void (*g_nvn_present_crop)(int crop[4]);
    g_nvn_present_crop = ls_present_crop;
    LOG("[wide] loading screens: the 16:9 act art (main 0x32ec60, UI -160.2..1120.2) is presented alone and stretched to the screen "
        "while st+0x6c (loading screen up) && !st+0x70 (art mode); DM_D3_WIDE_STRETCH_LOADING=0 turns it off");
}
// ---- Render height (DM_D3_HEIGHT, gear "Native screen resolution", experimental; default 900 = the Switch's docked window)
// The same init site holds the height: 0xe7860 movk x8,#900,lsl#32 (docked) / 0xe7864 movk x9,#720,lsl#32 (handheld). Everything that
// follows the window size (swap textures, crop, DisplayMode, the 3D cameras' aspect, the UI's centred 4:3 region and its scale) reads
// the struct, so a taller window only means more pixels: the UI keeps its 960x720 virtual space scaled by height / 720, touch and the
// cursor work in screen fractions (dm_cursor.c), the HUD icon copies take the UI map (hle_d3_icons.c).
//   DM_D3_HEIGHT=native  the screen's short side (front_screen_size: 1080 on a 2400x1080 phone, 1440 on QHD)
//   DM_D3_HEIGHT=<n>     explicit docked height (720..2160)
//   unset / 900          as before
// The width keeps the picture shape at that height: 16:9 (H x 16/9) or, with DM_D3_WIDE, H x the shape's aspect (the 900-high width
// x H / 900: 2000x900 -> 2400x1080); rounded to 16, at most 4096. Handheld = docked x 4/5 as before (rounded to 16 / 8).
// GPU cost ~ pixels: 2400x1080 is 1.44x the 2000x900 of widescreen (the owner's "~+40 %"); 3200x1440 is 2.56x.
static unsigned d3_height(void) {
    const char *e = getenv("DM_D3_HEIGHT");
    if (!e || !*e) return 900;
    long h = 0;
    if (!strcmp(e, "native")) {
        uint64_t s = front_screen_size ? front_screen_size() : 0; h = (long)(uint32_t)(s >> 32);
        if (h <= 0) { LOG("[wide] DM_D3_HEIGHT=native: screen size unknown, height stays 900"); return 900; }
    } else h = atol(e);
    if (h < 720 || h > 2160) { LOG("【红】[wide] DM_D3_HEIGHT=%s out of 720..2160: height stays 900", e); return 900; }
    return (unsigned)(h & ~1L);
}
// ---- Text at a height the Switch never runs (DM_D3_HEIGHT != 900; DM_D3_FONT_FIT; KB WIDE-08, docs/d3/WIDESCREEN.md)
// The console fonts are bitmaps baked per point size (Font SNO +0xc8: FontPointSizeData[+0xbc], 0x58 bytes: +0x20 resolution-class
// flags, +0x24 glyph count, +0x54 the size in pixels; every entry with flags != 0 gets its glyph table and textures at load, 0x276170).
// The Switch data has only two classes: 2 = 720 lines (the base sizes) and 0x20 = 900 lines (the same sizes x 1.25), 0x22 = both.
// The lookup 0x276a10 (font, size delta) picks the class from the window height (0x29a760, DisplayMode +0x24): < 720 -> 1 (576 -> 4),
// 720.. -> 2, 900.. -> 0x20, 1080.. -> 8, 2160.. -> 0x10, then wants scale x (font +0x9c base + delta) px (scale 0x34bda0 = UI height /
// 720) among that class's entries: exact (+-1e-6) or, with 0x10a21c0 (= 1), the first larger in the class. At 1080 the class 8 has no
// entry at all: it returns 0 and every caller (0x28fce0 draw, 0x28fad0 / 0x2911e0 measure, ...) skips the text, so a native 2400x1080
// window drew no text at all (frames, icons and the 3D were fine). Glyphs are drawn 1:1 in pixels and measured back through the same
// scale, so any baked size is crisp and lays out consistently; it only changes the text size.
//   DM_D3_FONT_FIT=near (default)  0x276a10 replaced (hook) by the same search over every baked size of any class: exact, else the
//                                  next larger if at most 5 % larger, else the next smaller (2400x1080: mostly exact, 21 -> 22, 33 -> 30,
//                                  54 -> 50; never bigger than +5 %, so no text outgrows its box)
//   DM_D3_FONT_FIT=up              game code only: class mask = every class (0x276a78) and the "first larger" fallback unconditional
//                                  (0x276b0c), i.e. the next larger baked size (up to +11 % at 1080: 54 -> 60)
//   DM_D3_FONT_FIT=0               off (the bug: no text at 1080)
// 0x276b30 (the same class search, "snap a size down") is left alone: its only caller 0x293600 drops the result.
enum { DF_FIND = 0x276a10, DF_MASK = 0x276a78, DF_APPROX = 0x276b0c, DF_WINSIZE = 0x29a760, DF_SCALE = 0x34bda0 };
static const uint32_t k_df_find[69] = {
    0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0x2a0103f4, 0xaa0003f3, 0x910033e0,
    0x910023e1, 0x94008f4b /* bl 0x29a760 */, 0xb9400bea, 0x7109015f, 0x321e03ed, 0x321f03ec, 0x1a9f05ad, 0x710b3d5f,
    0x321b03eb, 0x1a8d818c, 0x710e0d5f, 0xb9409e76 /* ldr w22,[x19,#0x9c] */, 0x321d03e9, 0x1a8c816b, 0x7110dd5f, 0x321c03e8,
    0x1a8b8129, 0x7121bd5f, 0x1a898115 /* csel w21,w8,w9,hi: the class mask */, 0x940354c9 /* bl 0x34bda0 */, 0xb940be68, 0x34000388, 0x0b1402cb, 0x1e220161,
    0xd00061cb, 0x1e210801, 0xbd4bc960, 0xd00061cb, 0xbd4e0962, 0x1e202820, 0x1e222821, 0xf9406660 /* ldr x0,[x19,#0xc8] */,
    0xaa1f03ea, 0xaa1f03e9, 0xb940200b /* ldr w11,[x0,#0x20] */, 0x6a15017f, 0x54000100, 0xbd405402 /* ldr s2,[x0,#0x54] */, 0x1e202040, 0x1e219448,
    0x5400020a, 0x1e202040, 0xaa0003e9, 0x5400010a, 0x9100054a, 0x91016000 /* add x0,x0,#0x58 */, 0xeb08015f, 0x54fffe63,
    0x14000004, 0xaa1f03e9, 0x14000002, 0xaa0003e9, 0x90007168, 0xb941c108 /* ldr w8,[0x10a21c0] */, 0x7100011f, 0x9a8903e0 /* csel x0,xzr,x9,eq */,
    0xa9437bfd, 0xa9424ff4, 0xa94157f6, 0x910103ff, 0xd65f03c0};
static const uint32_t k_df_winsize[4] = {0xd00075a8, 0xf9423908, 0xf9400109, 0xb9424529 /* ldr w9,[x9,#0x244] */};
static const uint32_t k_df_scale[4] = {0xd10083ff, 0xa9017bfd, 0x910043fd, 0x940c6fd1};
static uint8_t *g_df_base;
static atomic_int g_df_logged;
typedef void (*df_winsize_t)(uint32_t *w, uint32_t *h);
typedef float (*df_scale_t)(void);
// The replacement of 0x276a10: FontPointSizeData *(Font *font, int delta). Runs on the game's own threads (guest code calls it directly).
static const uint8_t *df_find(const uint8_t *font, int32_t delta) {
    const uint32_t n = *(const uint32_t *)(font + 0xbc);
    const uint8_t *e = *(const uint8_t *const *)(font + 0xc8), *lo = NULL, *hi = NULL;
    const int32_t base = *(const int32_t *)(font + 0x9c);
    const float scale = ((df_scale_t)(void *)(g_df_base + DF_SCALE))(), want = scale * (float)(base + delta);
    if (!n || !e) return NULL;
    for (uint32_t i = 0; i < n; i++, e += 0x58) {
        if (!*(const uint32_t *)(e + 0x20)) continue;   // no glyph table (not loaded for any class)
        const float s = *(const float *)(e + 0x54);
        if (s <= want + 1e-6f && s >= want - 1e-6f) return e;   // the game's own test (0x276ac8)
        if (s < want) lo = e; else if (!hi) hi = e;   // sizes ascend (every Font SNO of every language checked)
    }
    const uint8_t *r = hi && (!lo || *(const float *)(hi + 0x54) <= want * 1.05f) ? hi : lo;
    if (atomic_fetch_add(&g_df_logged, 1) < 48) {
        uint32_t w = 0, h = 0; ((df_winsize_t)(void *)(g_df_base + DF_WINSIZE))(&w, &h);
        LOG("[wide] font %p (base %d%+d, %u sizes) at %ux%u wants %.2f px (UI scale %.3f): no baked size, using %.2f (flags %#x)",
            (const void *)font, base, delta, n, w, h, want, scale, r ? *(const float *)(r + 0x54) : 0.0f, r ? *(const uint32_t *)(r + 0x20) : 0u);
    }
    return r;
}
static void df_install(module_t *m, unsigned H) {
    const char *e = getenv("DM_D3_FONT_FIT");
    int mode = !e || !*e || !strcmp(e, "near") || !strcmp(e, "1") ? 1 : !strcmp(e, "up") ? 2 : (*e == '0' && !e[1]) ? 0 : -1;
    if (mode < 0) { LOG("【红】[wide] DM_D3_FONT_FIT=%s unknown (near / up / 0): near", e); mode = 1; }
    if (!mode) { LOG("[wide] DM_D3_FONT_FIT=0: font size lookup main+%#x untouched (at %u lines the text may be missing)", DF_FIND, H); return; }
    if (m->text_end < DF_SCALE + sizeof k_df_scale || memcmp(m->base + DF_FIND, k_df_find, sizeof k_df_find) ||
        memcmp(m->base + DF_WINSIZE, k_df_winsize, sizeof k_df_winsize) || memcmp(m->base + DF_SCALE, k_df_scale, sizeof k_df_scale)) {
        LOG("【红】[wide] font size lookup main+%#x differs (not update 2.7.7, build 2607a74f): untouched, text may be missing at %u lines", DF_FIND, H); return; }
    if (mode == 2) {
        const uint32_t all[1] = {0x12800015 /* mov w21,#-1: every class */}, approx[1] = {0xaa0903e0 /* mov x0,x9: fallback always */};
        if (dw_poke(m, DF_MASK, all, 1) || dw_poke(m, DF_APPROX, approx, 1)) { LOG("【红】[wide] font fit: mprotect RW failed"); return; }
        LOG("[wide] DM_D3_FONT_FIT=up: font size lookup main+%#x searches every baked size (720 + 900 classes), else the next larger "
            "(window %u lines: the game's own class %#x has none above 900)", DF_FIND, H, H >= 2160 ? 0x10 : H >= 1080 ? 8 : H >= 900 ? 0x20 : H >= 720 ? 2 : 1);
        return;
    }
    g_df_base = m->base;
    const uint64_t to = (uint64_t)(uintptr_t)df_find;
    const uint32_t jmp[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};   // 0x276a10 is 8-aligned
    if (dw_poke(m, DF_FIND, jmp, 4)) { LOG("【红】[wide] font fit: mprotect RW failed"); return; }
    LOG("[wide] DM_D3_FONT_FIT=near: font size lookup main+%#x replaced: every baked size (720 + 900 classes), exact or the nearest "
        "(larger only up to +5 %%); window %u lines (handheld %u) - the Switch fonts have sizes only for 720 / 900", DF_FIND, H, (H * 4 / 5 + 4) / 8 * 8);
}
void dm_d3_wide_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_d3_wide_install(void) {
    const char *e = getenv("DM_D3_WIDE");
    const unsigned H = d3_height();
    if ((!e || !*e || (*e == '0' && !e[1])) && H == 900) { LOG("[wide] DM_D3_WIDE off: window 1600x900 (handheld 1280x720)"); return; }
    if (!e || !*e || (*e == '0' && !e[1])) e = NULL;
    long want = e ? atol(e) : 0; unsigned w;
    if (!e) w = 1600;   // DM_D3_HEIGHT alone: 16:9
    else if (!strcmp(e, "4:3")) w = 1200;
    else if (want >= 1200) w = (unsigned)want;
    else {
        uint64_t r = front_game_res(1); w = (uint32_t)(r >> 32) == 900 ? (uint32_t)r : 1600;
        uint64_t s = front_screen_size ? front_screen_size() : 0; uint32_t sw = (uint32_t)s, sh = (uint32_t)(s >> 32);
        if (w <= 1600 && sw && sh && (uint64_t)sw * 9 < (uint64_t)sh * 16) {   // narrower than 16:9: same height, the screen's aspect, not below 4:3
            w = (unsigned)(llround(900.0 * sw / sh / 16) * 16); if (w < 1200) w = 1200;
            LOG("[wide] screen %ux%u (aspect %.4f) narrower than 16:9: window %ux900%s", sw, sh, (double)sw / sh, w,
                (uint64_t)sw * 3 < (uint64_t)sh * 4 ? " (4:3, the narrowest the UI fits; bars at the top and bottom)" : "");
        }
    }
    w = (w + 8) / 16 * 16; if (w > 3200) w = 3200; if (w < 1200) w = 1200;
    if (w == 1600 && H == 900) { LOG("[wide] DM_D3_WIDE=%s: screen 16:9 (or unknown): window stays 1600x900", e); return; }
    const unsigned w900 = w;   // the width at 900 lines: its aspect is the picture shape
    if (H != 900) { w = (unsigned)(llround((double)w900 * H / 900.0 / 16) * 16); if (w > 4096) w = 4096; }
    const unsigned hh = (H * 4 / 5 + 4) / 8 * 8;   // handheld height (720 at 900)
    unsigned wh = (w * 4 / 5 + 8) / 16 * 16;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < DW_LBOX + sizeof k_dw_lbox || memcmp(m->base + DW_INIT, k_dw_init, sizeof k_dw_init) ||
        memcmp(m->base + DW_LBOX, k_dw_lbox, sizeof k_dw_lbox)) {
        LOG("【红】[wide] main is not D3 update 2.7.7 (build 2607a74f): window size site not found, stays 1600x900"); return; }
    float a = (float)w / (float)H; uint32_t ab; memcpy(&ab, &a, 4);
    const uint32_t init[6] = {0xd2800008u | w << 5 /* mov x8,#W */, 0xd2800009u | wh << 5 /* mov x9,#Wh */,
                              0xf2c00008u | H << 5 /* movk x8,#H,lsl#32 */, 0xf2c00009u | hh << 5 /* movk x9,#Hh,lsl#32 */,
                              0x5280000au | (ab & 0xffff) << 5 /* mov w10,#lo */, 0x72a0000au | (ab >> 16) << 5 /* movk w10,#hi,lsl#16 */};
    const uint32_t lbox[2] = {0x2a1f03e0 /* mov w0,wzr */, 0xd65f03c0 /* ret */};
    if (dw_poke(m, DW_INIT + 8, init, 6) || dw_poke(m, DW_LBOX, lbox, 2)) { LOG("【红】[wide] mprotect RW failed: window stays 1600x900"); return; }
    LOG("[wide] main+%#x window 1600x900 -> %ux%u, handheld 1280x720 -> %ux%u, aspect %.4f (%s 3D, UI edge-anchored)%s; letterbox check main+%#x off",
        DW_INIT + 8, w, H, wh, hh, a, (uint64_t)w * 9 > (uint64_t)H * 16 ? "Hor+" : (uint64_t)w * 9 == (uint64_t)H * 16 ? "16:9" : "Hor-, vertical FOV kept",
        H != 900 ? " [DM_D3_HEIGHT: render height, experimental]" : "", DW_LBOX);
    if (H != 900) df_install(m, H);   // fonts are baked only for 720 / 900 lines (WIDE-08)
    if ((uint64_t)w * 9 > (uint64_t)H * 16 + H / 8) ls_install(m);   // wider than 16:9 (the 16-pixel rounding of a 16:9 width is not)
}
#endif
