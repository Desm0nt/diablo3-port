// Local wireless (nn::ldn) over the LAN: the core, shared by the D3 and TL2 ports (same file in both trees; port-common module
// `ldn-lan`). Plain C + POSIX sockets, no host / guest dependency, so tools/../tests can run it on Linux (two instances on loopback).
//
// What a Switch does: nn::ldn makes a private Wi-Fi network between consoles (one access point, up to 7 stations), gives every node an
// IPv4 address on it and lists the nodes in NetworkInfo; the game then talks to the other nodes with ordinary BSD sockets (nn::socket)
// on those addresses. Here the phones are already on one network (home Wi-Fi or one phone's hotspot), so - like Atmosphere's ldn_mitm
// and Ryujinx's "ldn_mitm" mode - the LDN network is only *described*: discovery by UDP broadcast, membership over one TCP connection
// per station to the host, each node's address = its real LAN address, and the game's sockets use the LAN directly (hle_net.c lets
// them reach LAN addresses while local play is on).
//
// Wire format (compatible with ldn_mitm's LAN protocol): 12-byte header {u32 magic 0x11451400, u8 type, u8 compressed, u16 size,
// u16 decompressed size, u8 reserved[2]} + payload. Types: 0 scan (no payload, UDP broadcast to port 11452), 1 scan response
// (NetworkInfo, UDP unicast back), 2 connect (NodeInfo, station -> host over TCP 11452), 3 sync (NetworkInfo, host -> stations).
// Compression: a zero byte followed by n stands for n + 1 zero bytes; other bytes are literal.
//
// Structure layouts follow the SDK ABI (sizes checked below; same layouts as libnx ldn.h / Ryujinx's Ldn types, MIT).
// Copyright (c) 2026 Desm0nt. MIT License (the repository's LICENSE).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t local_id; uint8_t r0[2]; uint16_t scene; uint8_t r1[4]; } ldn_intent;                       // IntentId 0x10
typedef struct { ldn_intent intent; uint8_t session[16]; } ldn_network_id;                                           // NetworkId 0x20
typedef struct { uint8_t len; char text[33]; } ldn_ssid;                                                              // Ssid 0x22
typedef struct { uint32_t ip; uint8_t mac[6]; int8_t node_id; uint8_t connected; char name[33]; uint8_t r0;
                 uint16_t version; uint8_t r1[16]; } ldn_node;                                                        // NodeInfo 0x40
typedef struct {
    ldn_network_id id;                                                    // 0x000
    uint8_t bssid[6]; ldn_ssid ssid; uint16_t channel; int8_t link; uint8_t type; uint8_t r0[4];   // 0x020 CommonNetworkInfo
    uint8_t security_param[16];                                           // 0x050 LdnNetworkInfo
    uint16_t security_mode; uint8_t accept_policy; uint8_t r1; uint8_t r2[2];
    uint8_t max_nodes; uint8_t node_count;                                // 0x066 / 0x067
    ldn_node nodes[8];                                                    // 0x068
    uint8_t r3[2]; uint16_t adv_size; uint8_t adv[384];                   // 0x26a / 0x26c
    uint8_t r4[140]; uint64_t auth_id;                                    // 0x478
} ldn_network;                                                            // NetworkInfo 0x480
typedef struct { ldn_intent intent; uint16_t channel; uint8_t max_nodes; uint8_t r0; uint16_t version; uint8_t r1[10]; } ldn_net_config;  // NetworkConfig 0x20
typedef struct { uint16_t mode, pass_size; uint8_t pass[64]; } ldn_sec_config;                                       // SecurityConfig 0x44
typedef struct { char name[33]; uint8_t r[15]; } ldn_user_config;                                                     // UserConfig 0x30
typedef struct { uint8_t random[16]; uint8_t session[16]; } ldn_sec_param;                                            // SecurityParameter 0x20
typedef struct { ldn_network_id id; uint32_t type; uint8_t bssid[6]; ldn_ssid ssid; uint8_t r[16]; uint32_t flags; } ldn_scan_filter;  // ScanFilter 0x60
typedef struct { uint32_t ip; uint8_t mac[6]; uint8_t r[2]; } ldn_addr_entry;                                         // AddressEntry 0xc
typedef struct { uint8_t change; uint8_t r[7]; } ldn_node_update;                                                     // NodeLatestUpdate 8
_Static_assert(sizeof(ldn_node) == 0x40, "NodeInfo");
_Static_assert(sizeof(ldn_network) == 0x480, "NetworkInfo");
_Static_assert(offsetof(ldn_network, bssid) == 0x20 && offsetof(ldn_network, channel) == 0x48 && offsetof(ldn_network, security_param) == 0x50, "CommonNetworkInfo");
_Static_assert(offsetof(ldn_network, max_nodes) == 0x66 && offsetof(ldn_network, nodes) == 0x68 && offsetof(ldn_network, adv_size) == 0x26a, "LdnNetworkInfo");
_Static_assert(offsetof(ldn_network, adv) == 0x26c && offsetof(ldn_network, auth_id) == 0x478, "advertise / auth");
_Static_assert(sizeof(ldn_net_config) == 0x20 && sizeof(ldn_sec_config) == 0x44 && sizeof(ldn_user_config) == 0x30, "configs");
_Static_assert(sizeof(ldn_scan_filter) == 0x60 && offsetof(ldn_scan_filter, flags) == 0x5c && sizeof(ldn_sec_param) == 0x20, "filter / param");

// nn::ldn::State
enum { LDN_NONE = 0, LDN_INITIALIZED = 1, LDN_AP = 2, LDN_AP_CREATED = 3, LDN_STATION = 4, LDN_STATION_CONNECTED = 5, LDN_ERROR = 6 };
// nn::ldn::DisconnectReason
enum { LDN_DR_NONE = 0, LDN_DR_USER = 1, LDN_DR_SYSTEM = 2, LDN_DR_DESTROYED_BY_USER = 3, LDN_DR_DESTROYED_BY_SYSTEM = 4,
       LDN_DR_REJECTED = 5, LDN_DR_SIGNAL_LOST = 6 };
// Results: module 203 (ldn)
#define LDN_RES(d) ((uint32_t)(203u | (uint32_t)(d) << 9))
#define LDN_R_INVALID_STATE    LDN_RES(32)
#define LDN_R_NODE_NOT_FOUND   LDN_RES(48)
#define LDN_R_CONNECT_FAILURE  LDN_RES(64)
#define LDN_R_CONNECT_NOT_FOUND LDN_RES(65)
#define LDN_R_CONNECT_TIMEOUT  LDN_RES(66)
#define LDN_R_CONNECT_REJECTED LDN_RES(67)
#define LDN_R_INVALID_ARGUMENT LDN_RES(96)
#define LDN_R_TOO_MANY_PLAYERS LDN_RES(144)
#define LDN_R_DEVICE_NOT_AVAILABLE LDN_RES(16)

enum { LDN_MAGIC = 0x11451400, LDN_PORT = 11452, LDN_PKT_SCAN = 0, LDN_PKT_SCAN_RESP = 1, LDN_PKT_CONNECT = 2, LDN_PKT_SYNC = 3 };
// Packet helpers (exported for the tests): encode into out (header + payload, compressed when that is shorter), returns its size or -1;
// decode a whole packet, returns the payload size or -1.
int ldn_pkt_encode(uint8_t *out, size_t cap, unsigned type, const void *payload, size_t size);
int ldn_pkt_decode(const uint8_t *pkt, size_t size, unsigned *type, void *payload, size_t cap);

typedef struct {
    uint32_t ip, mask;           // own address / mask, host order (0 = pick the Wi-Fi / hotspot interface)
    uint32_t bind_ip;            // address the sockets bind to (0 = ip; differs only for a "virtual" ip behind NAT, e.g. an emulator)
    uint32_t peers[8]; int npeers;   // extra unicast scan targets (networks without broadcast, VPN, emulator host), host order
    uint16_t port;               // 0 = 11452
    int scan_ms;                 // how long a scan listens for answers (0 = 600)
    int connect_ms;              // TCP connect + first sync timeout (0 = 4000)
    int no_sweep;                // 0: a scan also asks every address of a /24 (or smaller) subnet directly, for Wi-Fi that drops broadcasts
    void (*log)(const char *line);                   // one line, no newline
    void (*notify)(void *opaque); void *opaque;      // state / node change (nn::ldn state change event); called without the lock held
} ldn_cfg;

typedef struct ldn_ctx ldn_ctx;
ldn_ctx *ldn_create(const ldn_cfg *cfg);     // the Initialize: picks the interface, opens no socket; NULL = no usable interface
void ldn_destroy(ldn_ctx *c);                // the Finalize: closes everything (a station disconnects, a host's network goes away)
int ldn_state(ldn_ctx *c);
int ldn_disconnect_reason(ldn_ctx *c);
uint32_t ldn_open_ap(ldn_ctx *c);
uint32_t ldn_close_ap(ldn_ctx *c);
uint32_t ldn_open_station(ldn_ctx *c);
uint32_t ldn_close_station(ldn_ctx *c);
uint32_t ldn_create_network(ldn_ctx *c, const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_user_config *uc,
                            const ldn_sec_param *sp /* NULL = new session */);
uint32_t ldn_destroy_network(ldn_ctx *c);
uint32_t ldn_set_advertise(ldn_ctx *c, const void *data, size_t size);
uint32_t ldn_scan(ldn_ctx *c, ldn_network *out, int *count, int max, const ldn_scan_filter *f);
uint32_t ldn_connect(ldn_ctx *c, const ldn_network *net, const ldn_user_config *uc, int version);
uint32_t ldn_connect_private(ldn_ctx *c, const ldn_net_config *nc, const ldn_sec_param *sp, const ldn_user_config *uc, int version);
uint32_t ldn_disconnect(ldn_ctx *c);
uint32_t ldn_get_network(ldn_ctx *c, ldn_network *out, ldn_node_update *upd, int n);
uint32_t ldn_get_ipv4(ldn_ctx *c, uint32_t *ip, uint32_t *mask);
uint32_t ldn_get_security_param(ldn_ctx *c, ldn_sec_param *out);
uint32_t ldn_get_network_config(ldn_ctx *c, ldn_net_config *out);
uint32_t ldn_set_accept_policy(ldn_ctx *c, int policy);
uint32_t ldn_reject(ldn_ctx *c, uint32_t ip);
int ldn_on_lan(ldn_ctx *c, uint32_t ip);     // 1: ip (host order) is on the local network the LDN session uses (socket policy)
// Picks the interface ldn_create would use: 0 and *ip / *mask set, or -1. Exported for the host's one-line report.
int ldn_pick_interface(uint32_t *ip, uint32_t *mask, char *name, size_t name_cap);
