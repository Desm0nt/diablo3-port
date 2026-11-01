// DOOM (Switch, nnSdk 6.4.0): service-layer takeovers the D2R build never needed.
// Compiled into every build (hle_*.c wildcard) but only registered under DM_GAME_DOOM, so the D2R force table
// (which aborts on unreferenced entries) stays unchanged.
#include "hle.h"
#include <stdbool.h>
#include <string.h>
#include <time.h>
#ifdef DM_GAME_DOOM

// Constructors of these SDK value objects are pure stores into the object (checked in sdk.raw 6.4.0: ErrorResultVariant {u32 kind, value},
// ShowWebPageArg / WebPageReturnValue memset + fields, bcat DeliveryCache{File,Directory} = 0, DeliveryCacheProgress mutex + zeroed state,
// ssl::Context = 0). A no-op takeover left them as uninitialised memory, unlike the console, and pure readers the game calls directly (e.g.
// web::WebPageReturnValue::GetWebExitReason) would read garbage; the takeovers keep their log line and then run the SDK's own constructor.
static void sdk_ctor(const char *name, void *self, const void *arg) {
    void (*f)(void *, const void *) = (void (*)(void *, const void *))guest_sym(name);
    if (!f) die("hle_doom: SDK constructor %s not found", name);
    f(self, arg);
}

// ---- bcat (background content delivery): offline. The delivery cache mounts but is empty; a sync request
// finishes immediately. Directory / file opens are never reached because enumeration returns no directories.
#define BCAT_NOT_FOUND ((Result)((1u << 9) | 122u))   // module 122 (bcat), description 1
enum { BCAT_PROGRESS_DONE = 9 };

HLE_FORCE("_ZN2nn4bcat10InitializeEv", "bcat 服务会话 | bcat service session: offline, none", void, h_bcat_Init, void) { LOG("bcat::Initialize (offline)"); }
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "delivery cache: mounted, empty", Result, h_bcat_Mount, void) { return 0; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "delivery cache unmount", void, h_bcat_Unmount, void) {}
HLE_FORCE("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", "delivery cache: no directories", Result, h_bcat_Enum, int *count, void *names, int max) {
    (void)names; (void)max; *count = 0; return 0;
}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", "directory object: no session (SDK constructor: plain stores)", void, h_bcat_DirCtor, void *self) { sdk_ctor("_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", self, NULL); }
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", "directory object: no session", void, h_bcat_DirDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", "directory open: not found", Result, h_bcat_DirOpen, void *self, const void *name) {
    (void)self; (void)name; return BCAT_NOT_FOUND;
}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", "directory read: empty", Result, h_bcat_DirRead, void *self, int *count, void *e, int max) {
    (void)self; (void)e; (void)max; *count = 0; return 0;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileC1Ev", "file object: no session (SDK constructor: plain stores)", void, h_bcat_FileCtor, void *self) { sdk_ctor("_ZN2nn4bcat17DeliveryCacheFileC1Ev", self, NULL); }
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFileD1Ev", "file object: no session", void, h_bcat_FileDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", "file open: not found", Result, h_bcat_FileOpen, void *self, const void *d, const void *f) {
    (void)self; (void)d; (void)f; return BCAT_NOT_FOUND;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", "file read: nothing", Result, h_bcat_FileRead, void *self, size_t *out, int64_t off, void *buf, size_t n) {
    (void)self; (void)off; (void)buf; (void)n; *out = 0; return BCAT_NOT_FOUND;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv", "file size: 0", int64_t, h_bcat_FileSize, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressC1Ev", "progress object (SDK constructor: mutex + zeroed state)", void, h_bcat_ProgCtor, void *self) { sdk_ctor("_ZN2nn4bcat21DeliveryCacheProgressC1Ev", self, NULL); }
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgressD1Ev", "progress object", void, h_bcat_ProgDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", "progress update: nothing pending", void, h_bcat_ProgUpd, void *self) { (void)self; }
HLE_FORCE("_ZNK2nn4bcat21DeliveryCacheProgress9GetStatusEv", "progress: done", int, h_bcat_ProgStatus, const void *self) { (void)self; return BCAT_PROGRESS_DONE; }
HLE_FORCE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", "sync request: completes immediately", Result, h_bcat_Sync, void *progress) {
    (void)progress; return 0;
}

// ---- hid entry points DOOM imports beyond the D2R set.
#if defined(DM_GAME_ETERNAL) || defined(DM_GAME_XC2) || defined(DM_GAME_D3)
// [ETERNAL] eShop pages are library applets (none here): the menu opens the store when the title bundle lookup fails offline
HLE_FORCE("_ZN2nn2ec30ShowShopApplicationInformationEv", "eShop page: none (offline)", void, h_ec_ShowAppInfo, void) { LOG("ec::ShowShopApplicationInformation (no eShop applet, skipped)"); }
HLE_FORCE("_ZN2nn2ec26ShowShopConsumableItemListEv", "eShop page: none (offline)", void, h_ec_ShowCons, void) { LOG("ec::ShowShopConsumableItemList (skipped)"); }
HLE_FORCE("_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidE", "eShop page: none (offline)", void, h_ec_ShowDetails, uint64_t uid) { (void)uid; LOG("ec::ShowShopProductDetails (skipped)"); }
HLE_FORCE("_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKc", "eShop page: none (offline)", void, h_ec_ShowList, const void *u, int n, const char *s) { (void)u; (void)n; (void)s; LOG("ec::ShowShopProductList (skipped)"); }
HLE_FORCE("_ZN2nn2ec28ShowShopConsumableItemDetailERKNS0_12ConsumableIdENS0_5NsUidE", "eShop page: none (offline)", void, h_ec_ShowConsD, const void *c, uint64_t uid) { (void)c; (void)uid; LOG("ec::ShowShopConsumableItemDetail (skipped)"); }
#endif
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "controller disconnect: host-owned pads stay", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18InitializeDebugPadEv", "debug pad: none", void, h_hid_DbgInit, void) {}
HLE_FORCE("_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", "debug pad: disconnected (32-byte state zeroed)", void, h_hid_DbgState, void *s) { memset(s, 0, 32); }

// ---- err / web applets: no system UI on the host; the error is logged instead.
HLE_FORCE("_ZN2nn3err18ErrorResultVariantC1Ev", "error variant: plain storage (SDK constructor)", void, h_err_VarCtor, void *self) { sdk_ctor("_ZN2nn3err18ErrorResultVariantC1Ev", self, NULL); }
HLE_FORCE("_ZN2nn3err18ErrorResultVariantC1ERKNS_6ResultE", "error variant from Result: logged, SDK constructor", void, h_err_VarCtorR, void *self, const Result *r) {
    LOG("err: ErrorResultVariant(Result %#x)", r ? *r : 0); sdk_ctor("_ZN2nn3err18ErrorResultVariantC1ERKNS_6ResultE", self, r);
}
HLE_FORCE("_ZN2nn3err18ErrorResultVariantC1ERKNS0_9ErrorCodeE", "error variant from ErrorCode: logged, SDK constructor", void, h_err_VarCtorC, void *self, const uint32_t *c) {
    LOG("err: ErrorResultVariant(ErrorCode %u-%u)", c ? c[0] : 0, c ? c[1] : 0); sdk_ctor("_ZN2nn3err18ErrorResultVariantC1ERKNS0_9ErrorCodeE", self, c);
}
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error applet: logged", void, h_err_ShowVar, const void *v) { (void)v; LOG("err::ShowError (applet not shown)"); }
HLE_FORCE("_ZN2nn3err21IsNetworkServiceErrorENS0_9ErrorCodeE", "network service errors cannot occur offline", bool, h_err_IsNse, uint64_t code) { (void)code; return false; }
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", "network error applet: logged", void, h_err_ShowNse, uint64_t code) { LOG("err::ShowNetworkServiceError %u-%u", (unsigned)code, (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3web14ShowWebPageArgC1EPKc", "web applet argument: logged, SDK constructor", void, h_web_ArgCtor, void *self, const char *url) {
    LOG("web: ShowWebPageArg(%s)", url ? url : ""); sdk_ctor("_ZN2nn3web14ShowWebPageArgC1EPKc", self, url);
}
HLE_FORCE("_ZN2nn3web18WebPageReturnValueC1Ev", "web applet result: SDK constructor (zeroed)", void, h_web_RetCtor, void *self) { sdk_ctor("_ZN2nn3web18WebPageReturnValueC1Ev", self, NULL); }
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web applet: not available", Result, h_web_Show, void *ret, const void *arg) { (void)ret; (void)arg; return BCAT_NOT_FOUND; }

// ---- time / vi
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "network clock -> host wall clock", Result, h_time_NetNow, int64_t *t) { *t = (int64_t)time(NULL); return 0; }
HLE_FORCE("_ZN2nn4time8FinalizeEv", "time service: nothing to close", Result, h_time_Fin, void) { return 0; }
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling: the host presenter scales", Result, h_vi_Scaling, void *layer, int mode) { (void)layer; LOG("vi::SetLayerScalingMode %d", mode); return 0; }

// ---- aoc: nninitFinalizeSdkModule (sdk+0x289d50) tail-calls aoc::Finalize at process exit; the SDK's version closes the aoc session that the
// host's aoc::Initialize (hle_sdkinit.c) never opened
HLE_FORCE("_ZN2nn3aoc8FinalizeEv", "aoc service session: none to close (nninitFinalizeSdkModule)", void, h_aoc_Finalize, void) {}

// ---- friends::Profile::GetProfileImageUrl asks the friends service through its sync session (sdk+0x1d1634, ShimLibraryGlobal::GetSyncSession);
// GetProfileList never succeeds offline, so no profile is valid: an empty URL and a failure
HLE_FORCE("_ZNK2nn7friends7Profile18GetProfileImageUrlEPNS0_3UrlENS0_9ImageSizeE", "profile image URL: friends service, offline -> failure", Result, h_fr_ProfileUrl,
          const void *self, char *url, int size) { (void)self; (void)size; if (url) url[0] = 0; return RES_FAIL; }

// ---- crypto: the csrng service -> host getrandom(2). Hashing / AES in nn::crypto are pure code and run as-is.
#include <sys/random.h>
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service -> getrandom(2)", void, h_crypto_Rand, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { ssize_t r = getrandom(p, n, 0); if (r < 0) { if (errno == EINTR) continue; die("getrandom: %s", strerror(errno)); } p += r; n -= (size_t)r; }
}

// ---- settings: LanguageCode::Make asks the settings service in 6.4; the enum -> code table is fixed (nn::settings::Language).
HLE_FORCE("_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", "Language enum -> 8-byte language code (fixed table)", uint64_t, h_set_LangMake, int lang) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    uint64_t v = 0;
    const char *c = lang >= 0 && lang < (int)(sizeof codes / sizeof *codes) ? codes[lang] : "en-US";
    memcpy(&v, c, strlen(c));
    return v;
}

#if defined(DM_GAME_ETERNAL) || defined(DM_GAME_XC2) || defined(DM_GAME_D3)
// ---- [ETERNAL] nn::ro (dynamic modules): the only NRO is /nro/Party.nro (PlayFab Party, multiplayer voice / text chat). The port is
// offline, so the ro service "works" but every module load fails: the game then has no Party (its Party* imports stay host stubs).
// ---- [ETERNAL] oe calls DOOM (2016) never made (each would IPC the applet service, which the host does not have, and abort)
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "oe: active time = guest uptime (TimeSpan ns)", int64_t, h_oe_ActiveTime, void) { return (int64_t)guest_ns(); }
HLE_FORCE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", "oe: clocks of a mode (handheld default 0x92220007)", uint32_t, h_oe_GetPerfConf, int mode) { (void)mode; return 0x92220007u; }
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "oe: CPU boost is the phone's", void, h_oe_CpuBoost, int mode) { LOG("oe::SetCpuBoostMode %d (ignored)", mode); }
// [ETERNAL] nn::settings::operator==(LanguageCode, Language): the SDK asks the settings service ("set") to turn the Language enum
// into its code; the host answers from Horizon's table (settings::Language order) and compares the 8-byte codes.
HLE_FORCE("_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", "settings: Language enum -> code (Horizon table), compare", bool, h_set_LangEq, const char *code, const int *lang) {
    static const char *const k[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!code || !lang || *lang < 0 || *lang >= (int)(sizeof k / sizeof k[0])) return false;
    return strncmp(code, k[*lang], 8) == 0;
}
// [ETERNAL] eShop (nn::ec): offline. The purchased event exists but never fires; shop service access fails.
#define EC_OFFLINE ((Result)((1u << 9) | 164u))   // module 164 (ec), description 1
HLE_FORCE("_ZN2nn2ec24InitializePurchasedEventEv", "ec: purchased event (offline: never signaled)", void, h_ec_InitPurch, void) { LOG("ec::InitializePurchasedEvent (offline)"); }
HLE_FORCE("_ZN2nn2ec17GetPurchasedEventEv", "ec: the purchased event, bound and never signaled", void *, h_ec_GetPurch, void) {
    static _Alignas(16) uint8_t ev[0x80]; static int bound; if (!bound) { bound = 1; hle_bind_system_event(ev, 1); } return ev;
}
HLE_FORCE("_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", "ec: shop service offline", Result, h_ec_InitShop, void *buf, uint64_t n) { (void)buf; (void)n; return EC_OFFLINE; }
HLE_FORCE("_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", "ec: shop service offline", void, h_ec_FiniShop, void) {}
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessorC1Ev", "ec: accessor object (no session)", void, h_ec_AccCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessorD1Ev", "ec: accessor object (no session)", void, h_ec_AccDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", "ec: shop service offline", Result, h_ec_AccInit, void *self, const void *svc) { (void)self; (void)svc; return EC_OFFLINE; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev", "ec: response object (no session)", void, h_ec_RespCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponseD1Ev", "ec: response object (no session)", void, h_ec_RespDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE",
          "ec: shop request fails (offline)", Result, h_ec_Request, void *self) { (void)self; return EC_OFFLINE; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", "ec: response ready (failed)", bool, h_ec_TryWait, void *self) { (void)self; return true; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", "ec: response fails (offline)", Result, h_ec_GetSize, void *self, uint64_t *n) { (void)self; if (n) *n = 0; return EC_OFFLINE; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", "ec: response fails (offline)", Result, h_ec_Get, void *self, void *b, uint64_t n) { (void)self; (void)b; (void)n; return EC_OFFLINE; }
#define RO_NOT_LOADED ((Result)((1u << 9) | 22u))   // module 22 (ro), description 1
HLE_FORCE("_ZN2nn2ro10InitializeEv", "ro: no service, nothing to set up", Result, h_ro_Init, void) { LOG("ro::Initialize (offline: modules are not loaded)"); return 0; }
HLE_FORCE("_ZN2nn2ro13GetBufferSizeEPmPKv", "ro: work buffer size (none needed)", Result, h_ro_BufSize, uint64_t *size, const void *nro) {
    (void)nro; if (size) *size = 0x1000; return 0;
}
HLE_FORCE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", "ro: NRR registration accepted", Result, h_ro_RegInfo, void *info, const void *nrr) {
    (void)info; (void)nrr; LOG("ro::RegisterModuleInfo"); return 0;
}
HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "ro: modules are not loaded (Party.nro = online chat)", Result, h_ro_Load, void *mod, const void *image, void *buf, uint64_t size, int flag) {
    (void)mod; (void)buf; (void)flag; LOG("ro::LoadModule image %p size %#llx -> not loaded", image, (unsigned long long)size); return RO_NOT_LOADED;
}
#endif
#endif
