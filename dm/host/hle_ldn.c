// [D3] [TL2] nn::ldn (local wireless play) served over the LAN: every ldn entry point either game imports, the same file in both trees
// (port-common module `ldn-lan`). The protocol and the state machine are in hle_ldn_core.c (see hle_ldn_core.h); this file maps the SDK's
// calls onto it, binds the state change event, and tells hle_net.c when the game's own sockets may reach the local network.
//
// What the games do (update 2.7.7 main of D3, 1.0 main of TL2; tools: work/../xref_imports.py + headless Ghidra):
//   D3: its party code (main 0xc031b0) calls ldn::Initialize only when the session is a local one (flag 0xc07f80), then a transport object
//       (0x9a8 bytes, ctor 0xc12520): host = OpenAccessPoint, SetAdvertiseData(0x180 bytes), CreateNetwork, GetIpv4Address, then its game
//       server listens on that address (0xc134b0); client = Scan (0xc133a0), OpenStation + Connect (0xc12f80), the host's address from
//       NetworkInfo +0x68 (node 0), GetIpv4Address for its own; while connected it polls GetNetworkInfo(info, NodeLatestUpdate[8]) and
//       finds its own node by comparing addresses (0xc13aa0): bit 0 = joined, bit 1 = left. A failing call ends the session.
//   TL2: NintendoWare Pia's LDN transport (nn::pia::local::Ldn*): Scan / Connect / CreateNetwork and the *Private* forms for host migration,
//       AttachStateChangeEvent, MakeBroadcastAddress (its "Pia Send Broadcast" UDP), Reject, SetStationAcceptPolicy.
// On a Switch the nodes get addresses on the console-to-console Wi-Fi; here each node's address is its LAN address (as ldn_mitm does),
// so the games' sockets talk over the real network.
//
// Switches (boot; development only - players use the gear's "Local network play" option, which writes DM_LDN):
//   DM_LDN=1 (default)  local play over the LAN; nothing opens until the game initializes ldn (D3: only with the "local" network option)
//   DM_LDN=0            off: the old answers (Initialize fails, state none) - the game says local play is unavailable
//   DM_LDN_IP=a.b.c.d[/bits]  this node's address (default: the Wi-Fi / hotspot interface); an address the phone does not own is used
//                       only in NetworkInfo and GetIpv4Address (a NAT setup; the game's binds to it go to "any")
//   DM_LDN_PEERS=a.b.c.d,...  extra unicast scan targets (networks that drop broadcasts, a VPN such as Tailscale between two phones)
//   DM_LDN_PORT=11452   discovery / membership port (ldn_mitm's)       DM_LDN_SCAN_MS=600   how long a scan waits for answers
//   DM_NET_TRACE=1      (or DM_LDN_TRACE=1) one "[trace] ..." line per ldn / socket call: arguments, result, errno, the game's caller
// Log lines: "[ldn] ..." (hle_ldn_core.c) and "[net] lan: ..." (hle_net.c).
// How D3 uses it (phone test 13.11): the network option "Local" (ConsoleNetworkOptions row 1, "Console:PartyLocalCommunicationNX64") is
// the party type 1; a new game starts as an offline party (type 0, HUD "offline", main 0x805d0), and choosing "Local" in the in-game
// network options (0x226030 -> 0x80ef0 -> 0x8e290 -> 0x7bd20 level 1 -> 0x7ea40 session) makes it local. Every local party then hosts
// its own network (party tick 0x8f540 -> 0x857b0: state Initialized -> CreateNetwork) and, while alone, scans for the others when its
// party list asks (0x857b0 -> 0x2171c0 / 0x357440 -> 0x81670); joining one (0x1bc080 -> 0x821c0 -> 0x818d0) closes the access point and
// connects as a station.
// Copyright (c) 2026 Desm0nt. MIT License (the repository's LICENSE).
#ifdef DM_GAME_D3
#include "hle.h"
#include "hle_ldn_core.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ldn_ctx *g_ctx;
static void *_Atomic g_event;              // AttachStateChangeEvent
static _Atomic int g_lan_used;             // local play opened (sticky: hle_net.c keeps the Horizon socket semantics from then on)
static _Atomic uint32_t g_ip, g_mask, g_virtual_ip;   // for the socket policy after Finalize
static uint32_t g_peers[8]; static int g_npeers;

static int ldn_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("DM_LDN"); m = !(e && e[0] == '0' && !e[1]); }
    return m;
}
static void ldn_log(const char *l) { LOG("%s", l); }
int dm_net_trace(void);
static void ldn_notify(void *o) {
    (void)o; void *ev = atomic_load(&g_event); if (ev) hle_signal_system_event(ev);
    if (dm_net_trace()) LOG("[trace] ldn state change event %s", ev ? "signalled" : "not attached (nothing to signal)");
}
static int parse_ip(const char *s, uint32_t *ip, uint32_t *mask) {
    char b[40]; snprintf(b, sizeof b, "%s", s); char *sl = strchr(b, '/'); int bits = 24;
    if (sl) { *sl = 0; bits = atoi(sl + 1); if (bits < 1 || bits > 32) bits = 24; }
    struct in_addr a; if (inet_pton(AF_INET, b, &a) != 1) return -1;
    *ip = ntohl(a.s_addr); if (mask) *mask = bits == 32 ? 0xffffffffu : ~((1u << (32 - bits)) - 1);
    return 0;
}
static ldn_ctx *ctx(void) { pthread_mutex_lock(&g_mu); ldn_ctx *c = g_ctx; pthread_mutex_unlock(&g_mu); return c; }

// ---- socket policy (hle_net.c)
int dm_ldn_lan_on(void) { return atomic_load_explicit(&g_lan_used, memory_order_relaxed); }
int dm_ldn_lan_route(uint32_t ip) {
    pthread_mutex_lock(&g_mu);
    int ok = g_ctx ? ldn_on_lan(g_ctx, ip) : 0;
    pthread_mutex_unlock(&g_mu);
    if (ok) return 1;
    uint32_t me = atomic_load(&g_ip), m = atomic_load(&g_mask), v = atomic_load(&g_virtual_ip);
    if (ip == 0xffffffffu || (ip >> 24) == 127) return 1;
    if (m && ((ip & m) == (me & m) || ip == (me | ~m))) return 1;
    if (v && ip == v) return 1;
    for (int i = 0; i < g_npeers; i++) if (g_peers[i] == ip) return 1;
    return 0;
}
uint32_t dm_ldn_lan_bind(uint32_t ip) { uint32_t v = atomic_load(&g_virtual_ip); return v && ip == v ? 0 : ip; }
static void lan_opened(void) {
    if (!atomic_exchange(&g_lan_used, 1)) LOG("[ldn] local play opened: the game's sockets may reach the local network from now on (Horizon socket semantics)");
}


// ---- DM_NET_TRACE=1 (or DM_LDN_TRACE=1): one line per ldn call - entry point, result, the caller in the game - and, in hle_net.c, per socket
// call. Repeats of the same call / result / caller are folded ("x N more") so polling does not flood the log. Off: nothing is computed.
int dm_net_trace(void) {
    static int t = -1;
    if (t < 0) { const char *a = getenv("DM_NET_TRACE"), *b = getenv("DM_LDN_TRACE"); t = (a && *a == '1') || (b && *b == '1'); }
    return t;
}
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
static int tr_read(uint64_t a, void *o, size_t n) {   // a bad pointer gives EFAULT, never a fault
    if (a < 0x10000 || a >= (1ull << 48)) return 0;
    struct iovec l = {o, n}, r = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == (ssize_t)n;
}
// The frame-pointer chain from here into the game (its own frame records): who asked for a close / DestroyNetwork
static void tr_backtrace(const char *why) {
    char line[700]; int off = 0; uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    for (int i = 0; i < 16 && off < (int)sizeof line - 40; i++) {
        uint64_t rec[2]; if (!tr_read(fp, rec, sizeof rec)) break;
        module_t *m = module_of((const void *)(uintptr_t)rec[1]);
        if (m) off += snprintf(line + off, sizeof line - (size_t)off, "%s+%#lx ", m->name, (unsigned long)((const uint8_t *)(uintptr_t)rec[1] - m->base));
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    line[off] = 0; LOG("[trace] %s: game backtrace %s", why, line);
}
#ifdef DM_GAME_TL2
// [TL2] Pia's session object (nn::pia::session::Session, pointer at main+0x92c74e0 - GOT main+0x23a35e8): +0xd4 / +0xd8 its states,
// +0xdc the Result it keeps, +0x158 the reason; LdnBackgroundProcessJob::WaitCreateNetworkEvent (main 0x1bd2990) fails on them (main
// 0x1c7ca30: 5 / 7 / 8 -> 0xc406 / 0xc420 / 0xc421). Logged whenever they change while tracing.
static void tr_pia_session(void) {
    static module_t *mm; static uint32_t last[4]; static int have;
    if (!mm) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    if (!mm) return;
    uint64_t so = 0; uint32_t v[4];
    if (!tr_read((uint64_t)(uintptr_t)(mm->base + 0x92c74e0), &so, 8) || !so) return;
    if (!tr_read(so + 0xd4, v, 12) || !tr_read(so + 0x158, &v[3], 4)) return;
    if (have && !memcmp(v, last, sizeof v)) return;
    have = 1; memcpy(last, v, sizeof v);
    LOG("[trace] pia session %#llx: +0xd4 %u +0xd8 %u +0xdc result %#x +0x158 %u", (unsigned long long)so, v[0], v[1], v[2], v[3]);
}
#endif
void dm_net_trace_line(const char *what, long r, int err, const void *ra) {
#ifdef DM_GAME_TL2
    tr_pia_session();
#endif
    if (!strncmp(what, "close(", 6) || !strcmp(what, "ldn_destroy_network") || !strcmp(what, "ldn_disconnect")) tr_backtrace(what);
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static char last[200]; static unsigned rep; static unsigned lines;
    char cur[200]; module_t *m = module_of(ra);
    snprintf(cur, sizeof cur, "%s -> %ld (%#lx)%s%s <- %s+%#lx", what, r, (unsigned long)r, err ? " errno " : "", err ? strerror(err) : "",
             m ? m->name : "host", m ? (unsigned long)((const uint8_t *)ra - m->base) : 0ul);
    pthread_mutex_lock(&mu);
    if (!strcmp(cur, last)) { rep++; pthread_mutex_unlock(&mu); return; }
    if (rep) LOG("[trace] ... x %u more", rep);
    rep = 0; snprintf(last, sizeof last, "%s", cur);
    if (lines++ < 20000) LOG("[trace] %s", cur);
    pthread_mutex_unlock(&mu);
}
static uint32_t ltr(const char *fn, uint32_t r, const void *ra) { if (dm_net_trace()) dm_net_trace_line(fn, (long)(int32_t)r, 0, ra); return r; }
#define LTRV(fn, expr) ltr(fn, (uint32_t)(expr), __builtin_return_address(0))

// ---- nn::ldn
#define LDNF(m, cname, ret, ...) HLE_FORCE(m, "[D3] [TL2] local wireless over the LAN (hle_ldn.c; DM_LDN=0: unavailable)", ret, cname, __VA_ARGS__)
LDNF("_ZN2nn3ldn10InitializeEv", h_ldn_Init, Result, void) {
    if (!ldn_mode()) { LOG("ldn::Initialize: local wireless unavailable (DM_LDN=0)"); return RES_FAIL; }
    pthread_mutex_lock(&g_mu);
    Result r = 0;
    if (!g_ctx) {
        ldn_cfg cfg; memset(&cfg, 0, sizeof cfg);
        const char *e;
        uint32_t real_ip = 0, real_mask = 0; char ifn[32] = "";
        int have_if = ldn_pick_interface(&real_ip, &real_mask, ifn, sizeof ifn) == 0;
        if ((e = getenv("DM_LDN_IP")) && *e && parse_ip(e, &cfg.ip, &cfg.mask) == 0) {
            if (!have_if || cfg.ip != real_ip) atomic_store(&g_virtual_ip, cfg.ip);
        }
        if ((e = getenv("DM_LDN_PEERS")) && *e) {
            char b[256]; snprintf(b, sizeof b, "%s", e);
            for (char *t = strtok(b, ", "); t && cfg.npeers < 8; t = strtok(NULL, ", ")) if (parse_ip(t, &cfg.peers[cfg.npeers], NULL) == 0) cfg.npeers++;
            memcpy(g_peers, cfg.peers, sizeof g_peers); g_npeers = cfg.npeers;
        }
        if ((e = getenv("DM_LDN_PORT")) && atoi(e) > 0) cfg.port = (uint16_t)atoi(e);
        if ((e = getenv("DM_LDN_SCAN_MS")) && atoi(e) > 0) cfg.scan_ms = atoi(e);
        cfg.log = ldn_log; cfg.notify = ldn_notify;
        g_ctx = ldn_create(&cfg);
        if (g_ctx) { atomic_store(&g_ip, cfg.ip ? cfg.ip : real_ip); atomic_store(&g_mask, cfg.ip ? cfg.mask : real_mask); }
        else { LOG("ldn::Initialize: no Wi-Fi / hotspot / LAN interface with an IPv4 address: local wireless unavailable (as with the radio off)"); r = LDN_R_DEVICE_NOT_AVAILABLE; }
    }
    pthread_mutex_unlock(&g_mu);
    // The game's own sockets follow Horizon from here, not only from OpenAccessPoint: D3's local session creates its UDP transport socket
    // right after Initialize (main 0xc031b0 -> 0xc08040: Socket(2, 2, 0), Fcntl(F_SETFL, flags | 0x800)), and the offline FreeBSD
    // translation read 0x800 as "blocking" (phone test 13.11: the socket at boot came out blocking)
    if (!r) lan_opened();
    return LTRV("Initialize", r);
}
LDNF("_ZN2nn3ldn8FinalizeEv", h_ldn_Fini, void, void) {
    LTRV("Finalize", 0);
    pthread_mutex_lock(&g_mu); ldn_ctx *c = g_ctx; g_ctx = NULL; pthread_mutex_unlock(&g_mu);
    if (c) ldn_destroy(c);
}
LDNF("_ZN2nn3ldn8GetStateEv", h_ldn_State, int, void) { ldn_ctx *c = ctx(); return (int)LTRV("GetState", c ? ldn_state(c) : 0); }
LDNF("_ZN2nn3ldn19GetDisconnectReasonEv", h_ldn_Reason, int, void) { ldn_ctx *c = ctx(); return (int)LTRV("GetDisconnectReason", c ? ldn_disconnect_reason(c) : 0); }
LDNF("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", h_ldn_Attach, void, void *ev) {
    hle_bind_system_event(ev, 0); atomic_store(&g_event, ev); LTRV("AttachStateChangeEvent", 0);
}
#define C_OR_FAIL ldn_ctx *c = ctx(); if (!c) return LTRV(__func__, RES_FAIL)
LDNF("_ZN2nn3ldn15OpenAccessPointEv", h_ldn_OpenAp, Result, void) { C_OR_FAIL; Result r = LTRV("ldn_open_ap", ldn_open_ap(c)); if (!r) lan_opened(); return r; }
LDNF("_ZN2nn3ldn16CloseAccessPointEv", h_ldn_CloseAp, Result, void) { C_OR_FAIL; return LTRV("ldn_close_ap", ldn_close_ap(c)); }
LDNF("_ZN2nn3ldn11OpenStationEv", h_ldn_OpenSta, Result, void) { C_OR_FAIL; Result r = LTRV("ldn_open_station", ldn_open_station(c)); if (!r) lan_opened(); return r; }
LDNF("_ZN2nn3ldn12CloseStationEv", h_ldn_CloseSta, Result, void) { C_OR_FAIL; return LTRV("ldn_close_station", ldn_close_station(c)); }
LDNF("_ZN2nn3ldn10DisconnectEv", h_ldn_Disc, Result, void) { C_OR_FAIL; return LTRV("ldn_disconnect", ldn_disconnect(c)); }
LDNF("_ZN2nn3ldn14DestroyNetworkEv", h_ldn_Destroy, Result, void) { C_OR_FAIL; return LTRV("ldn_destroy_network", ldn_destroy_network(c)); }
LDNF("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", h_ldn_Scan, Result, ldn_network *out, int *n, int max, const ldn_scan_filter *f, int ch) {
    (void)ch; ldn_ctx *c = ctx(); if (!c) { if (n) *n = 0; return LTRV(__func__, RES_FAIL); }
    return LTRV("ldn_scan", ldn_scan(c, out, n, max, f));
}
LDNF("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", h_ldn_ScanPriv, Result, ldn_network *out, int *n, int max, const ldn_scan_filter *f, int ch) {
    (void)ch; ldn_ctx *c = ctx(); if (!c) { if (n) *n = 0; return LTRV(__func__, RES_FAIL); }
    return LTRV("ldn_scan", ldn_scan(c, out, n, max, f));
}
LDNF("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", h_ldn_Create, Result,
     const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_user_config *uc) { C_OR_FAIL; return LTRV("ldn_create_network", ldn_create_network(c, nc, sc, uc, NULL)); }
LDNF("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
     h_ldn_CreatePriv, Result, const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_sec_param *sp, const ldn_user_config *uc, int naddr, const ldn_addr_entry *addr) {
    (void)naddr; (void)addr;   // the address table is for the console Wi-Fi's own addressing; LAN addresses stay what they are
    C_OR_FAIL; return LTRV("ldn_create_network", ldn_create_network(c, nc, sc, uc, sp));
}
LDNF("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", h_ldn_Connect, Result,
     const ldn_network *net, const ldn_sec_config *sc, const ldn_user_config *uc, int version, int opt) { (void)sc; (void)opt; C_OR_FAIL; return LTRV("ldn_connect", ldn_connect(c, net, uc, version)); }
LDNF("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
     h_ldn_ConnectPriv, Result, const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_sec_param *sp, const ldn_user_config *uc, int version, int opt) {
    (void)sc; (void)opt; C_OR_FAIL; return LTRV("ldn_connect_private", ldn_connect_private(c, nc, sp, uc, version));
}
LDNF("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoEPNS0_16NodeLatestUpdateEi", h_ldn_NetInfoU, Result, ldn_network *o, ldn_node_update *u, int n) {
    C_OR_FAIL; return LTRV("ldn_get_network", ldn_get_network(c, o, u, n));
}
LDNF("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", h_ldn_NetInfo, Result, ldn_network *o) { C_OR_FAIL; return LTRV("ldn_get_network", ldn_get_network(c, o, NULL, 0)); }
LDNF("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", h_ldn_Ip, Result, uint32_t *a, uint32_t *m) {
    ldn_ctx *c = ctx(); if (!c) { if (a) *a = 0; if (m) *m = 0; return LTRV(__func__, RES_FAIL); }
    return LTRV("ldn_get_ipv4", ldn_get_ipv4(c, a, m));
}
LDNF("_ZN2nn3ldn16SetAdvertiseDataEPKvm", h_ldn_Adv, Result, const void *d, uint64_t n) { C_OR_FAIL; return LTRV("ldn_set_advertise", ldn_set_advertise(c, d, (size_t)n)); }
LDNF("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", h_ldn_SecPrm, Result, ldn_sec_param *o) { C_OR_FAIL; return LTRV("ldn_get_security_param", ldn_get_security_param(c, o)); }
LDNF("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", h_ldn_NetCfg, Result, ldn_net_config *o) { C_OR_FAIL; return LTRV("ldn_get_network_config", ldn_get_network_config(c, o)); }
LDNF("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", h_ldn_Policy, Result, int p) { C_OR_FAIL; return LTRV("ldn_set_accept_policy", ldn_set_accept_policy(c, p)); }
LDNF("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", h_ldn_Reject, Result, uint32_t ip) { C_OR_FAIL; return LTRV("ldn_reject", ldn_reject(c, ip)); }
#endif
