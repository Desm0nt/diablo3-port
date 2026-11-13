// Test of the LDN-over-LAN core (hle_ldn_core.c) without a phone: one host and two stations in one process, each on its own loopback
// address (127.0.0.2 host, .3 / .4 stations), port 21452. Linux / WSL / macOS:
//   cc -O1 -pthread -I.. -o /tmp/t_ldn_lan t_ldn_lan.c ../hle_ldn_core.c && /tmp/t_ldn_lan
// (macOS: `sudo ifconfig lo0 alias 127.0.0.2 up` etc. first; Linux routes all of 127/8 to lo already.)
// Checks: packet compression round trip; scan by local communication id (and a filter that must not match); advertise data; connect;
// NodeLatestUpdate connect / disconnect bits on both sides; max nodes (a third node refused when the host allows 2); reject; private
// connect by session id; the host going away (station back to "station", reason signal lost); socket policy (ldn_on_lan).
#include "hle_ldn_core.h"
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("ok   " __VA_ARGS__), printf("\n"); } while (0)
static void lg(const char *l) { printf("     | %s\n", l); }
static _Atomic int g_notes[3];
static void note(void *o) { atomic_fetch_add(&g_notes[(int)(intptr_t)o], 1); }
static uint32_t ip4(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return ntohl(a.s_addr); }
static ldn_ctx *mk(const char *ip, int who) {
    ldn_cfg c; memset(&c, 0, sizeof c);
    c.ip = ip4(ip); c.mask = 0xffffff00u;   // /24: the scan also sweeps the subnet c.bind_ip = c.ip; c.port = 21452; c.scan_ms = 300; c.connect_ms = 2000;
    c.peers[0] = ip4("127.0.0.2"); c.npeers = 1; c.log = lg; c.notify = note; c.opaque = (void *)(intptr_t)who;
    return ldn_create(&c);
}
static int count_updates(const ldn_node_update *u, int bit) { int k = 0; for (int i = 0; i < 8; i++) if (u[i].change & bit) k++; return k; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    {   // packets
        ldn_network n; memset(&n, 0, sizeof n); n.id.intent.local_id = 0x0100abcdef000000ull; n.nodes[0].ip = 0x7f000002; strcpy(n.nodes[0].name, "host");
        uint8_t p[2048]; int len = ldn_pkt_encode(p, sizeof p, LDN_PKT_SYNC, &n, sizeof n);
        ldn_network back; unsigned type = 99; int got = ldn_pkt_decode(p, (size_t)len, &type, &back, sizeof back);
        CHECK(len > 12 && len < (int)sizeof n && got == (int)sizeof n && type == LDN_PKT_SYNC && !memcmp(&n, &back, sizeof n), "packet round trip (compressed %d -> %d bytes)", (int)sizeof n, len);
        len = ldn_pkt_encode(p, sizeof p, LDN_PKT_SCAN, NULL, 0);
        CHECK(len == 12 && ldn_pkt_decode(p, 12, &type, &back, sizeof back) == 0 && type == LDN_PKT_SCAN, "empty scan packet");
    }
    ldn_ctx *h = mk("127.0.0.2", 0), *s1 = mk("127.0.0.3", 1), *s2 = mk("127.0.0.4", 2);
    CHECK(h && s1 && s2, "three contexts");
    if (!h || !s1 || !s2) return 1;
    CHECK(ldn_state(h) == LDN_INITIALIZED, "initialized");
    ldn_network tmp; CHECK(ldn_get_network(h, &tmp, NULL, 0) == LDN_R_INVALID_STATE, "no network before create");

    ldn_net_config nc; memset(&nc, 0, sizeof nc); nc.intent.local_id = 0x01001b300b9be000ull; nc.intent.scene = 7; nc.max_nodes = 2; nc.version = 3;
    ldn_sec_config sc; memset(&sc, 0, sizeof sc); sc.mode = 1;
    ldn_user_config uh; memset(&uh, 0, sizeof uh); strcpy(uh.name, "Host"); ldn_user_config us = uh; strcpy(us.name, "Guest");
    CHECK(ldn_open_ap(h) == 0 && ldn_state(h) == LDN_AP, "open access point");
    CHECK(ldn_set_advertise(h, "D3-ADVERT", 10) == 0, "advertise before create");
    CHECK(ldn_create_network(h, &nc, &sc, &uh, NULL) == 0 && ldn_state(h) == LDN_AP_CREATED, "create network");
    ldn_node_update u[8]; ldn_network hn;
    CHECK(ldn_get_network(h, &hn, u, 8) == 0 && hn.node_count == 1 && count_updates(u, 1) == 1 && u[0].change == 1, "host sees itself (node 0 connect bit)");
    uint32_t ip = 0, mask = 0; CHECK(ldn_get_ipv4(h, &ip, &mask) == 0 && ip == ip4("127.0.0.2"), "host address");

    CHECK(ldn_open_station(s1) == 0 && ldn_state(s1) == LDN_STATION, "open station");
    ldn_scan_filter f; memset(&f, 0, sizeof f); f.id.intent.local_id = nc.intent.local_id; f.flags = 1;
    ldn_network found[4]; int cnt = -1;
    CHECK(ldn_scan(s1, found, &cnt, 4, &f) == 0 && cnt == 1, "scan finds the network (%d)", cnt);
    CHECK(cnt == 1 && found[0].adv_size == 10 && !memcmp(found[0].adv, "D3-ADVERT", 10) && found[0].nodes[0].ip == ip4("127.0.0.2") && !strcmp(found[0].nodes[0].name, "Host"),
          "scan result: advertise data, host address and name");
    ldn_scan_filter f2 = f; f2.id.intent.local_id ^= 1; int cnt2 = -1;
    CHECK(ldn_scan(s1, found + 1, &cnt2, 3, &f2) == 0 && cnt2 == 0, "other game's filter finds nothing");

    CHECK(ldn_connect(s1, &found[0], &us, 3) == 0 && ldn_state(s1) == LDN_STATION_CONNECTED, "station connects");
    usleep(150000);
    CHECK(ldn_get_network(h, &hn, u, 8) == 0 && hn.node_count == 2 && u[1].change == 1 && hn.nodes[1].ip == ip4("127.0.0.3") && !strcmp(hn.nodes[1].name, "Guest"),
          "host: node 1 joined (connect bit, address, name)");
    ldn_network sn; CHECK(ldn_get_network(s1, &sn, u, 8) == 0 && sn.node_count == 2 && u[0].change == 1 && u[1].change == 1, "station sees both nodes");
    CHECK(ldn_get_ipv4(s1, &ip, &mask) == 0 && ip == ip4("127.0.0.3"), "station address = its node's address");
    CHECK(ldn_on_lan(s1, ip4("127.0.0.2")) && ldn_on_lan(s1, ip4("127.0.0.255")) && !ldn_on_lan(s1, ip4("8.8.8.8")), "socket policy: LAN yes, internet no");

    CHECK(ldn_set_advertise(h, "UPDATED", 8) == 0, "advertise update");
    usleep(150000);
    CHECK(ldn_get_network(s1, &sn, NULL, 0) == 0 && sn.adv_size == 8 && !memcmp(sn.adv, "UPDATED", 8), "station got the advertise update (sync)");

    CHECK(ldn_open_station(s2) == 0, "second station");
    cnt = 0; ldn_scan(s2, found, &cnt, 4, &f);
    CHECK(cnt == 1 && found[0].node_count == 2, "second station sees a full network (2 of 2)");
    CHECK(cnt == 1 && ldn_connect(s2, &found[0], &us, 3) != 0 && ldn_state(s2) == LDN_STATION, "third node refused (max 2)");

    CHECK(ldn_disconnect(s1) == 0 && ldn_state(s1) == LDN_STATION && ldn_disconnect_reason(s1) == LDN_DR_USER, "station disconnects");
    usleep(200000);
    CHECK(ldn_get_network(h, &hn, u, 8) == 0 && hn.node_count == 1 && u[1].change == 2, "host: node 1 left (disconnect bit)");

    ldn_sec_param sp; CHECK(ldn_get_security_param(h, &sp) == 0, "security parameter");
    CHECK(ldn_connect_private(s2, &nc, &sp, &us, 3) == 0 && ldn_state(s2) == LDN_STATION_CONNECTED, "private connect by session id");
    usleep(150000);
    CHECK(ldn_get_network(h, &hn, u, 8) == 0 && hn.node_count == 2 && hn.nodes[1].ip == ip4("127.0.0.4"), "host: the private node joined");
    CHECK(ldn_reject(h, ip4("127.0.0.4")) == 0, "host rejects it");
    usleep(300000);
    CHECK(ldn_state(s2) == LDN_STATION && ldn_get_network(s2, &sn, NULL, 0) != 0, "rejected station is back to 'station'");

    CHECK(ldn_connect(s1, &found[0], &us, 3) == 0, "station reconnects");
    usleep(150000);
    int before = atomic_load(&g_notes[1]);
    CHECK(ldn_destroy_network(h) == 0 && ldn_state(h) == LDN_AP, "host destroys the network");
    usleep(300000);
    CHECK(ldn_state(s1) == LDN_STATION && ldn_disconnect_reason(s1) == LDN_DR_SIGNAL_LOST && atomic_load(&g_notes[1]) > before, "station lost the host (state change notified)");
    CHECK(ldn_close_station(s1) == 0 && ldn_close_ap(h) == 0 && ldn_state(h) == LDN_INITIALIZED, "close");
    ldn_destroy(s2); ldn_destroy(s1); ldn_destroy(h);
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
