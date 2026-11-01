// [D3] Diablo III: Eternal Collection (SDK 8.2.0, sdk build 3ae34e68): nn:: entry points its main imports that neither the D2R set nor
// the DOOM / Eternal / XC2 ports serve, or serve under another overload. Without them the native SDK client opens a service session
// the host does not have (nfp, ldn, applets, ...) and aborts. The port is offline-only: every network / local-wireless / amiibo /
// shop entry answers "unavailable / none / empty" the way the SDK's callers expect, so the game takes its offline branch.
// The nfp and ldn parts are ported from nxnative (MIT, https://github.com/seedds/nxnative: dm/host/hle_sdk16.c "nfp: amiibo",
// dm/host/hle_offline.c "ldn: local wireless"); the account / fs / oe entries follow hle_xc2.c (itself from nxnative).
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

// ---- ldn: local wireless (nxnative hle_offline.c, MIT). Initialization fails, state is always "none"; MakeIpv4Address / operator== are
// pure and stay in the SDK.
HLE_FORCE("_ZN2nn3ldn10InitializeEv", "local wireless (cut): initialization fails", Result, h_ldn_Init, void) { LOG("ldn::Initialize: local wireless unavailable"); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "local wireless (cut)", void, h_ldn_Fini, void) {}
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "local wireless (cut): state 0 (none)", int, h_ldn_State, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "local wireless (cut)", Result, h_ldn_OpenSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "local wireless (cut)", Result, h_ldn_CloseSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "local wireless (cut)", Result, h_ldn_OpenAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "local wireless (cut)", Result, h_ldn_CloseAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "local wireless (cut)", Result, h_ldn_Disc, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "local wireless (cut)", Result, h_ldn_Destroy, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan (cut): 0 found", Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) {
    (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "local wireless (cut)", Result, h_ldn_Create,
          const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless (cut)", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) { (void)a; (void)b; (void)c; (void)v; (void)o; return RES_FAIL; }
// [D3] SDK 8 overload with the node update array (not in nxnative's list)
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoEPNS0_16NodeLatestUpdateEi", "local wireless (cut)", Result, h_ldn_NetInfoU, void *o, void *upd, int n) { (void)o; (void)upd; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "local wireless (cut)", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) { if (a) *a = 0; if (m) *m = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "local wireless (cut)", Result, h_ldn_Adv, const void *d, uint64_t n) { (void)d; (void)n; return RES_FAIL; }
#endif
