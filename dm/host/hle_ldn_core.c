// Local wireless (nn::ldn) over the LAN: the core. See hle_ldn_core.h for the design and the wire format.
// Copyright (c) 2026 Desm0nt. MIT License (the repository's LICENSE).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // pthread_setname_np (glibc; bionic has it anyway)
#endif
#include "hle_ldn_core.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- packets
typedef struct { uint32_t magic; uint8_t type, compressed; uint16_t size, raw_size; uint8_t r[2]; } pkt_hdr;
_Static_assert(sizeof(pkt_hdr) == 12, "LAN packet header");
enum { PKT_MAX = 2048 };

static int zcompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (o + 2 > cap) return -1;
        if (in[i]) { out[o++] = in[i++]; continue; }
        size_t run = 1; while (i + run < n && !in[i + run] && run < 256) run++;
        out[o++] = 0; out[o++] = (uint8_t)(run - 1); i += run;
    }
    return (int)o;
}
static int zdecompress(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i]) { if (o >= cap) return -1; out[o++] = in[i]; continue; }
        if (++i >= n) return -1;
        size_t run = (size_t)in[i] + 1; if (o + run > cap) return -1;
        memset(out + o, 0, run); o += run;
    }
    return (int)o;
}
int ldn_pkt_encode(uint8_t *out, size_t cap, unsigned type, const void *payload, size_t size) {
    if (cap < sizeof(pkt_hdr) || size > 0xffff) return -1;
    pkt_hdr h = {LDN_MAGIC, (uint8_t)type, 0, (uint16_t)size, (uint16_t)size, {0, 0}};
    uint8_t tmp[PKT_MAX]; int z = size ? zcompress(payload, size, tmp, sizeof tmp) : -1;
    if (z >= 0 && (size_t)z < size) { h.compressed = 1; h.size = (uint16_t)z; }
    if (sizeof h + h.size > cap) return -1;
    memcpy(out, &h, sizeof h);
    if (h.size) memcpy(out + sizeof h, h.compressed ? tmp : payload, h.size);
    return (int)(sizeof h + h.size);
}
int ldn_pkt_decode(const uint8_t *pkt, size_t size, unsigned *type, void *payload, size_t cap) {
    pkt_hdr h; if (size < sizeof h) return -1;
    memcpy(&h, pkt, sizeof h);
    if (h.magic != LDN_MAGIC || sizeof h + h.size > size) return -1;
    *type = h.type;
    if (!h.compressed) { if (h.size > cap) return -1; memcpy(payload, pkt + sizeof h, h.size); return h.size; }
    int n = zdecompress(pkt + sizeof h, h.size, payload, cap);
    return n == h.raw_size ? n : -1;
}

// ---------------------------------------------------------------- context
typedef struct { int fd, node; uint32_t peer; int n; uint8_t buf[PKT_MAX]; } conn;   // node -1: connected, no connect packet yet
enum { MAXC = 12 };
struct ldn_ctx {
    ldn_cfg cfg; pthread_mutex_t mu;
    int state, reason, policy;
    uint32_t ip, mask, bind_ip; uint8_t mac[6]; uint16_t port; char ifname[32];
    ldn_network net; int have_net;            // host: the network it runs; station: the last sync
    ldn_net_config ncfg; ldn_user_config ucfg;
    uint8_t seen[8]; uint32_t seen_ip[8];     // what the last GetNetworkInfo reported (NodeLatestUpdate)
    int udp, lsn;                             // host: discovery responder, TCP listener
    conn cs[MAXC];                            // host: stations (and not yet identified connections)
    conn up;                                  // station: the connection to the host
    pthread_t thr; int thr_on, wake[2], quit;
    int notify_pending;
};

static void clog_(ldn_ctx *c, const char *fmt, ...) {
    if (!c->cfg.log) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    c->cfg.log(b);
}
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void rnd(void *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); ssize_t r = fd >= 0 ? read(fd, p, n) : -1; if (fd >= 0) close(fd);
    if (r != (ssize_t)n) { uint8_t *b = p; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rand() ^ (int)mono_ms()); }
}
static const char *ipstr(uint32_t ip, char *b) { snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); return b; }
static void sin_of(struct sockaddr_in *s, uint32_t ip, uint16_t port) { memset(s, 0, sizeof *s); s->sin_family = AF_INET; s->sin_addr.s_addr = htonl(ip); s->sin_port = htons(port); }
static void wake(ldn_ctx *c) { char x = 1; if (c->wake[1] >= 0 && write(c->wake[1], &x, 1) < 0) {} }
static void mark_changed(ldn_ctx *c) { c->notify_pending = 1; }
static void flush_notify(ldn_ctx *c) {   // call without the lock
    pthread_mutex_lock(&c->mu); int n = c->notify_pending; c->notify_pending = 0; pthread_mutex_unlock(&c->mu);
    if (n && c->cfg.notify) c->cfg.notify(c->cfg.opaque);
}
// shutdown first: another thread may sit in poll() on this descriptor, and close() alone would neither wake it nor send the FIN
// until that poll returns (the peer would see the node leave up to a second late)
static void conn_close(conn *k) { if (k->fd >= 0) { shutdown(k->fd, SHUT_RDWR); close(k->fd); } k->fd = -1; k->node = -1; k->n = 0; k->peer = 0; }

int ldn_pick_interface(uint32_t *ip, uint32_t *mask, char *name, size_t cap) {
    struct ifaddrs *all = NULL, *best = NULL; int best_score = -1;
    if (getifaddrs(&all) != 0) return -1;
    for (struct ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        const char *n = a->ifa_name ? a->ifa_name : "";
        uint32_t v = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
        int priv = (v >> 24) == 10 || (v >> 20) == 0xac1 || (v >> 16) == 0xc0a8 || (v >> 16) == 0xa9fe;
        int s = 0;
        if (!strncmp(n, "wlan", 4) || !strncmp(n, "swlan", 5) || !strncmp(n, "ap", 2) || !strncmp(n, "softap", 6) || !strncmp(n, "wifi", 4)) s = 4;
        else if (!strncmp(n, "eth", 3) || !strncmp(n, "en", 2)) s = 3;
        else if (!strncmp(n, "rmnet", 5) || !strncmp(n, "ccmni", 5) || !strncmp(n, "dummy", 5) || !strncmp(n, "r_rmnet", 7)) s = 0;
        else if (priv && (a->ifa_flags & IFF_BROADCAST)) s = 2;
        else s = 1;   // tun (VPN, Tailscale) and the rest
        if (priv && s > 0) s += 1;
        if (s > best_score) { best_score = s; best = a; }
    }
    int ok = -1;
    if (best && best_score > 0) {
        *ip = ntohl(((struct sockaddr_in *)best->ifa_addr)->sin_addr.s_addr);
        *mask = best->ifa_netmask ? ntohl(((struct sockaddr_in *)best->ifa_netmask)->sin_addr.s_addr) : 0xffffff00u;
        if (name && cap) snprintf(name, cap, "%s", best->ifa_name ? best->ifa_name : "?");
        ok = 0;
    }
    freeifaddrs(all);
    return ok;
}

// ---------------------------------------------------------------- sending
static int send_all(int fd, const uint8_t *b, int n) {
    while (n > 0) {
        ssize_t r = send(fd, b, (size_t)n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { struct pollfd p = {fd, POLLOUT, 0}; if (poll(&p, 1, 1000) <= 0) return -1; continue; }
        if (r <= 0) return -1;
        b += r; n -= (int)r;
    }
    return 0;
}
static void host_recount(ldn_ctx *c) {
    int k = 0; for (int i = 0; i < 8; i++) if (c->net.nodes[i].connected) k++;
    c->net.node_count = (uint8_t)k;
}
static void host_sync_locked(ldn_ctx *c) {   // the network to every station
    uint8_t p[PKT_MAX]; int n = ldn_pkt_encode(p, sizeof p, LDN_PKT_SYNC, &c->net, sizeof c->net);
    if (n < 0) return;
    for (int i = 0; i < MAXC; i++) if (c->cs[i].fd >= 0 && c->cs[i].node > 0 && send_all(c->cs[i].fd, p, n)) {
        char b[16]; clog_(c, "[ldn] host: node %d (%s) lost while syncing", c->cs[i].node, ipstr(c->cs[i].peer, b));
        int nd = c->cs[i].node; memset(&c->net.nodes[nd], 0, sizeof c->net.nodes[nd]); c->net.nodes[nd].node_id = (int8_t)nd;
        conn_close(&c->cs[i]); host_recount(c); mark_changed(c);
    }
}

// ---------------------------------------------------------------- the thread (host: discovery + stations; station: the host link)
static void host_drop(ldn_ctx *c, conn *k, const char *why) {
    char b[16];
    if (k->node > 0) {
        clog_(c, "[ldn] host: node %d (%s) left: %s", k->node, ipstr(k->peer, b), why);
        memset(&c->net.nodes[k->node], 0, sizeof c->net.nodes[k->node]); c->net.nodes[k->node].node_id = (int8_t)k->node;
        conn_close(k); host_recount(c); host_sync_locked(c); mark_changed(c);
    } else conn_close(k);
}
static void host_packet(ldn_ctx *c, conn *k, unsigned type, const uint8_t *pl, int n) {
    char b[16], b2[16];
    if (type != LDN_PKT_CONNECT || k->node > 0) { clog_(c, "[ldn] host: unexpected packet %u from %s", type, ipstr(k->peer, b)); return; }
    if (n != (int)sizeof(ldn_node)) { host_drop(c, k, "bad connect packet"); return; }
    ldn_node in; memcpy(&in, pl, sizeof in);
    if (c->state != LDN_AP_CREATED || c->policy == 1) { host_drop(c, k, "not accepting"); return; }
    int slot = -1, maxn = c->net.max_nodes ? c->net.max_nodes : 8; if (maxn > 8) maxn = 8;
    for (int i = 1; i < maxn; i++) if (!c->net.nodes[i].connected) { slot = i; break; }
    if (slot < 0) { clog_(c, "[ldn] host: %s refused: network full (%d nodes)", ipstr(k->peer, b), maxn); host_drop(c, k, "full"); return; }
    ldn_node *nd = &c->net.nodes[slot];
    *nd = in; nd->ip = in.ip ? in.ip : k->peer; nd->node_id = (int8_t)slot; nd->connected = 1; nd->name[32] = 0;
    k->node = slot;
    host_recount(c);
    clog_(c, "[ldn] host: node %d joined: \"%s\" %s (connection from %s), version %u, %d of %d nodes", slot, nd->name, ipstr(nd->ip, b),
          ipstr(k->peer, b2), nd->version, c->net.node_count, maxn);
    host_sync_locked(c); mark_changed(c);
}
static void conn_read(ldn_ctx *c, conn *k, int host_side) {
    ssize_t r = recv(k->fd, k->buf + k->n, sizeof k->buf - (size_t)k->n, 0);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {
        if (host_side) host_drop(c, k, r == 0 ? "closed" : strerror(errno));
        else {   // the station lost its host
            clog_(c, "[ldn] station: host link %s: disconnected", r == 0 ? "closed" : strerror(errno));
            conn_close(k); c->state = LDN_STATION; c->reason = LDN_DR_SIGNAL_LOST; c->have_net = 0; memset(&c->net, 0, sizeof c->net); mark_changed(c);
        }
        return;
    }
    k->n += (int)r;
    for (;;) {
        pkt_hdr h; if (k->n < (int)sizeof h) break;
        memcpy(&h, k->buf, sizeof h);
        if (h.magic != LDN_MAGIC || sizeof h + h.size > sizeof k->buf) {
            if (host_side) host_drop(c, k, "bad packet"); else { conn_close(k); c->state = LDN_STATION; c->reason = LDN_DR_SYSTEM; c->have_net = 0; mark_changed(c); }
            return;
        }
        int len = (int)sizeof h + h.size; if (k->n < len) break;
        uint8_t pl[sizeof(ldn_network) + 16]; unsigned type; int pn = ldn_pkt_decode(k->buf, (size_t)len, &type, pl, sizeof pl);
        memmove(k->buf, k->buf + len, (size_t)(k->n - len)); k->n -= len;
        if (pn < 0) continue;
        if (host_side) { host_packet(c, k, type, pl, pn); if (k->fd < 0) return; }
        else if (type == LDN_PKT_SYNC && pn == (int)sizeof(ldn_network)) {
            memcpy(&c->net, pl, sizeof c->net); c->have_net = 1; mark_changed(c);
            int me = 0; for (int i = 0; i < 8; i++) if (c->net.nodes[i].connected && c->net.nodes[i].ip == c->ip) me = 1;
            if (!me) clog_(c, "[ldn] station: sync without this node (%d nodes)", c->net.node_count);
        }
    }
}
static void udp_scan_reply(ldn_ctx *c) {
    uint8_t in[PKT_MAX]; struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t r = recvfrom(c->udp, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
    if (r < (ssize_t)sizeof(pkt_hdr)) return;
    uint8_t pl[16]; unsigned type; if (ldn_pkt_decode(in, (size_t)r, &type, pl, sizeof pl) < 0 || type != LDN_PKT_SCAN) return;
    if (c->state != LDN_AP_CREATED) return;
    uint8_t out[PKT_MAX]; int n = ldn_pkt_encode(out, sizeof out, LDN_PKT_SCAN_RESP, &c->net, sizeof c->net);
    if (n > 0) sendto(c->udp, out, (size_t)n, MSG_NOSIGNAL, (struct sockaddr *)&from, fl);
    static int logged; char b[16];
    if (logged++ < 8) clog_(c, "[ldn] host: scan from %s:%u answered", ipstr(ntohl(from.sin_addr.s_addr), b), ntohs(from.sin_port));
}
static void host_accept(ldn_ctx *c) {
    struct sockaddr_in a; socklen_t al = sizeof a;
    int fd = accept(c->lsn, (struct sockaddr *)&a, &al); if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    for (int i = 0; i < MAXC; i++) if (c->cs[i].fd < 0) { c->cs[i].fd = fd; c->cs[i].node = -1; c->cs[i].n = 0; c->cs[i].peer = ntohl(a.sin_addr.s_addr); return; }
    close(fd);
}
static void *thread_main(void *arg) {
    ldn_ctx *c = arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "dm-ldn");
#endif
    for (;;) {
        struct pollfd p[MAXC + 4]; void *who[MAXC + 4]; int np = 0;
        pthread_mutex_lock(&c->mu);
        if (c->quit) { pthread_mutex_unlock(&c->mu); break; }
        p[np] = (struct pollfd){c->wake[0], POLLIN, 0}; who[np++] = NULL;
        if (c->udp >= 0) { p[np] = (struct pollfd){c->udp, POLLIN, 0}; who[np++] = &c->udp; }
        if (c->lsn >= 0) { p[np] = (struct pollfd){c->lsn, POLLIN, 0}; who[np++] = &c->lsn; }
        for (int i = 0; i < MAXC; i++) if (c->cs[i].fd >= 0) { p[np] = (struct pollfd){c->cs[i].fd, POLLIN, 0}; who[np++] = &c->cs[i]; }
        if (c->up.fd >= 0) { p[np] = (struct pollfd){c->up.fd, POLLIN, 0}; who[np++] = &c->up; }
        pthread_mutex_unlock(&c->mu);
        int r = poll(p, (nfds_t)np, 1000);
        if (r <= 0) continue;
        pthread_mutex_lock(&c->mu);
        for (int i = 0; i < np && !c->quit; i++) {
            if (!p[i].revents) continue;
            if (!who[i]) { char d[64]; while (read(c->wake[0], d, sizeof d) > 0) {} continue; }
            if (who[i] == &c->udp) { if (c->udp == p[i].fd) udp_scan_reply(c); }
            else if (who[i] == &c->lsn) { if (c->lsn == p[i].fd) host_accept(c); }
            else if (who[i] == &c->up) { if (c->up.fd == p[i].fd) conn_read(c, &c->up, 0); }
            else { conn *k = who[i]; if (k->fd == p[i].fd) conn_read(c, k, 1); }
        }
        pthread_mutex_unlock(&c->mu);
        flush_notify(c);
    }
    return NULL;
}
static int thread_start(ldn_ctx *c) {   // under the lock (the thread takes it only after it runs)
    if (c->thr_on) return 0;
    c->quit = 0; if (pthread_create(&c->thr, NULL, thread_main, c) == 0) c->thr_on = 1;
    return 0;
}

// ---------------------------------------------------------------- lifecycle
ldn_ctx *ldn_create(const ldn_cfg *cfg) {
    ldn_ctx *c = calloc(1, sizeof *c); if (!c) return NULL;
    c->cfg = *cfg; pthread_mutex_init(&c->mu, NULL);
    c->udp = c->lsn = c->up.fd = -1; c->wake[0] = c->wake[1] = -1;
    for (int i = 0; i < MAXC; i++) c->cs[i].fd = -1;
    c->port = cfg->port ? cfg->port : LDN_PORT;
    if (cfg->ip) { c->ip = cfg->ip; c->mask = cfg->mask ? cfg->mask : 0xffffff00u; snprintf(c->ifname, sizeof c->ifname, "configured"); }
    else if (ldn_pick_interface(&c->ip, &c->mask, c->ifname, sizeof c->ifname)) { free(c); return NULL; }
    c->bind_ip = cfg->bind_ip;
    c->mac[0] = 0x02; c->mac[1] = 0x00; c->mac[2] = (uint8_t)(c->ip >> 24); c->mac[3] = (uint8_t)(c->ip >> 16); c->mac[4] = (uint8_t)(c->ip >> 8); c->mac[5] = (uint8_t)c->ip;
    if (pipe(c->wake) == 0) { fcntl(c->wake[0], F_SETFL, O_NONBLOCK); fcntl(c->wake[1], F_SETFL, O_NONBLOCK); }
    c->state = LDN_INITIALIZED;
    char b[16], m[16]; clog_(c, "[ldn] initialized: %s %s/%s, port %u, %d extra scan targets", c->ifname, ipstr(c->ip, b), ipstr(c->mask, m), c->port, cfg->npeers);
    return c;   // no socket and no thread until the game opens an access point or a station
}
static void host_close_locked(ldn_ctx *c) {
    if (c->udp >= 0) { shutdown(c->udp, SHUT_RDWR); close(c->udp); }
    if (c->lsn >= 0) { shutdown(c->lsn, SHUT_RDWR); close(c->lsn); }
    c->udp = c->lsn = -1;
    for (int i = 0; i < MAXC; i++) conn_close(&c->cs[i]);
}
void ldn_destroy(ldn_ctx *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu); c->quit = 1; pthread_mutex_unlock(&c->mu); wake(c);
    if (c->thr_on) pthread_join(c->thr, NULL);
    host_close_locked(c); conn_close(&c->up);
    if (c->wake[0] >= 0) { close(c->wake[0]); }
    if (c->wake[1] >= 0) { close(c->wake[1]); }
    clog_(c, "[ldn] finalized");
    pthread_mutex_destroy(&c->mu); free(c);
}
int ldn_state(ldn_ctx *c) { pthread_mutex_lock(&c->mu); int s = c->state; pthread_mutex_unlock(&c->mu); return s; }
int ldn_disconnect_reason(ldn_ctx *c) { pthread_mutex_lock(&c->mu); int s = c->reason; pthread_mutex_unlock(&c->mu); return s; }
#define LOCKED(expr) do { pthread_mutex_lock(&c->mu); uint32_t r_ = (expr); pthread_mutex_unlock(&c->mu); flush_notify(c); return r_; } while (0)
static uint32_t set_state(ldn_ctx *c, int from_mask, int to) {
    if (!(from_mask >> c->state & 1)) return LDN_R_INVALID_STATE;
    if (c->state != to) { c->state = to; mark_changed(c); }
    return 0;
}
uint32_t ldn_open_ap(ldn_ctx *c) { LOCKED((thread_start(c), set_state(c, 1 << LDN_INITIALIZED | 1 << LDN_AP, LDN_AP))); }
uint32_t ldn_open_station(ldn_ctx *c) { LOCKED((thread_start(c), c->reason = 0, set_state(c, 1 << LDN_INITIALIZED | 1 << LDN_STATION, LDN_STATION))); }
static void forget_seen(ldn_ctx *c) { memset(c->seen, 0, sizeof c->seen); memset(c->seen_ip, 0, sizeof c->seen_ip); }
static uint32_t destroy_locked(ldn_ctx *c) {
    if (c->state != LDN_AP_CREATED) return LDN_R_INVALID_STATE;
    host_close_locked(c); c->have_net = 0; memset(&c->net, 0, sizeof c->net); forget_seen(c);
    c->state = LDN_AP; mark_changed(c); clog_(c, "[ldn] host: network destroyed");
    return 0;
}
uint32_t ldn_destroy_network(ldn_ctx *c) { LOCKED(destroy_locked(c)); }
uint32_t ldn_close_ap(ldn_ctx *c) {
    pthread_mutex_lock(&c->mu);
    uint32_t r = 0;
    if (c->state == LDN_AP_CREATED) destroy_locked(c);
    if (c->state == LDN_AP) { c->state = LDN_INITIALIZED; mark_changed(c); } else r = LDN_R_INVALID_STATE;
    pthread_mutex_unlock(&c->mu); flush_notify(c); return r;
}
static uint32_t disconnect_locked(ldn_ctx *c) {
    if (c->state != LDN_STATION_CONNECTED) return LDN_R_INVALID_STATE;
    conn_close(&c->up); c->have_net = 0; memset(&c->net, 0, sizeof c->net); forget_seen(c);
    c->state = LDN_STATION; c->reason = LDN_DR_USER; mark_changed(c); clog_(c, "[ldn] station: disconnected");
    return 0;
}
uint32_t ldn_disconnect(ldn_ctx *c) { LOCKED(disconnect_locked(c)); }
uint32_t ldn_close_station(ldn_ctx *c) {
    pthread_mutex_lock(&c->mu);
    uint32_t r = 0;
    if (c->state == LDN_STATION_CONNECTED) disconnect_locked(c);
    if (c->state == LDN_STATION) { c->state = LDN_INITIALIZED; mark_changed(c); } else r = LDN_R_INVALID_STATE;
    pthread_mutex_unlock(&c->mu); flush_notify(c); return r;
}

// ---------------------------------------------------------------- host
static int open_listen(ldn_ctx *c, int type) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    if (type == SOCK_DGRAM) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a; sin_of(&a, type == SOCK_DGRAM ? 0 : c->bind_ip, c->port);   // discovery on every address (broadcasts); TCP on bind_ip or any
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || (type == SOCK_STREAM && listen(fd, 8))) { int e = errno; close(fd); errno = e; return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}
static uint32_t create_locked(ldn_ctx *c, const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_user_config *uc, const ldn_sec_param *sp) {
    if (c->state != LDN_AP) return LDN_R_INVALID_STATE;
    int max = nc->max_nodes ? nc->max_nodes : 8; if (max > 8) max = 8;
    c->udp = open_listen(c, SOCK_DGRAM);
    int ue = errno;
    c->lsn = open_listen(c, SOCK_STREAM);
    if (c->udp < 0 || c->lsn < 0) {
        clog_(c, "[ldn] host: cannot open port %u (udp %s, tcp %s)", c->port, c->udp < 0 ? strerror(ue) : "ok", c->lsn < 0 ? strerror(errno) : "ok");
        host_close_locked(c); return LDN_R_DEVICE_NOT_AVAILABLE;
    }
    uint8_t keep_adv[384]; uint16_t keep_n = c->net.adv_size <= 384 ? c->net.adv_size : 0; memcpy(keep_adv, c->net.adv, keep_n);
    memset(&c->net, 0, sizeof c->net);
    c->net.id.intent = nc->intent;
    if (sp) { memcpy(c->net.id.session, sp->session, 16); memcpy(c->net.security_param, sp->random, 16); }
    else { rnd(c->net.id.session, 16); rnd(c->net.security_param, 16); }
    memcpy(c->net.bssid, c->mac, 6);
    { uint8_t r[16]; rnd(r, sizeof r); c->net.ssid.len = 32; for (int i = 0; i < 32; i++) c->net.ssid.text[i] = "0123456789abcdef"[r[i / 2] >> (i & 1 ? 0 : 4) & 15]; }
    c->net.channel = nc->channel ? nc->channel : 6; c->net.link = 3; c->net.type = 2;
    c->net.security_mode = sc ? sc->mode : 1; c->net.accept_policy = (uint8_t)c->policy;
    c->net.max_nodes = (uint8_t)max;
    for (int i = 0; i < 8; i++) c->net.nodes[i].node_id = (int8_t)i;
    ldn_node *me = &c->net.nodes[0];
    me->ip = c->ip; memcpy(me->mac, c->mac, 6); me->node_id = 0; me->connected = 1; me->version = nc->version;
    if (uc) { memcpy(me->name, uc->name, 32); me->name[32] = 0; }
    c->net.node_count = 1;
    c->net.adv_size = keep_n; memcpy(c->net.adv, keep_adv, keep_n);
    rnd(&c->net.auth_id, sizeof c->net.auth_id);
    c->ncfg = *nc; if (uc) c->ucfg = *uc;
    c->have_net = 1; forget_seen(c);
    c->state = LDN_AP_CREATED; mark_changed(c);
    char b[16];
    clog_(c, "[ldn] host: network created by \"%s\" at %s: local id %016llx scene %u, channel %u, %d nodes max, version %u, advertise %u bytes%s",
          me->name, ipstr(c->ip, b), (unsigned long long)nc->intent.local_id, nc->intent.scene, c->net.channel, max, nc->version, keep_n, sp ? " (private: given session)" : "");
    wake(c);
    return 0;
}
uint32_t ldn_create_network(ldn_ctx *c, const ldn_net_config *nc, const ldn_sec_config *sc, const ldn_user_config *uc, const ldn_sec_param *sp) {
    if (!nc) return LDN_R_INVALID_ARGUMENT;
    LOCKED(create_locked(c, nc, sc, uc, sp));
}
uint32_t ldn_set_advertise(ldn_ctx *c, const void *data, size_t size) {
    if (size > 384) return LDN_R_INVALID_ARGUMENT;
    pthread_mutex_lock(&c->mu);
    memset(c->net.adv, 0, sizeof c->net.adv); if (size) memcpy(c->net.adv, data, size); c->net.adv_size = (uint16_t)size;
    if (c->state == LDN_AP_CREATED) host_sync_locked(c);
    pthread_mutex_unlock(&c->mu); flush_notify(c);
    return 0;
}
uint32_t ldn_set_accept_policy(ldn_ctx *c, int policy) {
    pthread_mutex_lock(&c->mu); c->policy = policy; c->net.accept_policy = (uint8_t)policy; pthread_mutex_unlock(&c->mu);
    return 0;
}
uint32_t ldn_reject(ldn_ctx *c, uint32_t ip) {
    pthread_mutex_lock(&c->mu);
    uint32_t r = LDN_R_NODE_NOT_FOUND;
    if (c->state == LDN_AP_CREATED)
        for (int i = 0; i < MAXC; i++) if (c->cs[i].fd >= 0 && c->cs[i].node > 0 && c->net.nodes[c->cs[i].node].ip == ip) { host_drop(c, &c->cs[i], "rejected by the host"); r = 0; }
    pthread_mutex_unlock(&c->mu); flush_notify(c);
    return r;
}

// ---------------------------------------------------------------- station
static int filter_ok(const ldn_network *n, const ldn_scan_filter *f) {
    if (!f) return 1;
    uint32_t fl = f->flags;
    if ((fl & 1) && n->id.intent.local_id != f->id.intent.local_id) return 0;
    if ((fl & 2) && memcmp(n->id.session, f->id.session, 16)) return 0;
    if ((fl & 4) && n->type != f->type) return 0;
    if ((fl & 8) && memcmp(n->bssid, f->bssid, 6)) return 0;
    if ((fl & 16) && (n->ssid.len != f->ssid.len || memcmp(n->ssid.text, f->ssid.text, n->ssid.len < 33 ? n->ssid.len : 33))) return 0;
    if ((fl & 32) && n->id.intent.scene != f->id.intent.scene) return 0;
    return 1;
}
uint32_t ldn_scan(ldn_ctx *c, ldn_network *out, int *count, int max, const ldn_scan_filter *f) {
    if (count) *count = 0;
    int st = ldn_state(c);
    if (st == LDN_NONE || st == LDN_ERROR) return LDN_R_INVALID_STATE;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return LDN_R_DEVICE_NOT_AVAILABLE;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in me; sin_of(&me, c->bind_ip, 0); bind(fd, (struct sockaddr *)&me, sizeof me);
    uint8_t pkt[PKT_MAX]; int pn = ldn_pkt_encode(pkt, sizeof pkt, LDN_PKT_SCAN, NULL, 0);
    uint32_t tg[12]; int nt = 0;
    tg[nt++] = 0xffffffffu;
    if (c->mask && c->mask != 0xffffffffu) tg[nt++] = c->ip | ~c->mask;
    for (int i = 0; i < c->cfg.npeers && nt < 12; i++) tg[nt++] = c->cfg.peers[i];
    int found = 0, answers = 0, wait = c->cfg.scan_ms > 0 ? c->cfg.scan_ms : 600;
    ldn_network got[16]; int ngot = 0;
    uint64_t t0 = mono_ms(), resend = t0 + (uint64_t)wait / 3;
    for (int k = 0; k < nt; k++) { struct sockaddr_in a; sin_of(&a, tg[k], c->port); sendto(fd, pkt, (size_t)pn, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a); }
    // Some access points and phones drop broadcasts (power save, "client isolation" of multicast): ask every address of a small subnet once
    // too (254 datagrams of 12 bytes); the host answers whichever reaches it first, duplicates are merged below
    if (!c->cfg.no_sweep && c->mask >= 0xffffff00u && c->mask != 0xffffffffu) {
        uint32_t net = c->ip & c->mask, bc = c->ip | ~c->mask;
        for (uint32_t h = net + 1; h < bc; h++) if (h != c->ip) { struct sockaddr_in a; sin_of(&a, h, c->port); sendto(fd, pkt, (size_t)pn, MSG_NOSIGNAL | MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a); }
    }
    for (;;) {
        uint64_t now = mono_ms(); if (now >= t0 + (uint64_t)wait) break;
        if (resend && now >= resend) { resend = 0; for (int k = 0; k < nt; k++) { struct sockaddr_in a; sin_of(&a, tg[k], c->port); sendto(fd, pkt, (size_t)pn, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a); } }
        struct pollfd p = {fd, POLLIN, 0}; int left = (int)(t0 + (uint64_t)wait - now); if (resend && (int)(resend - now) < left) left = (int)(resend - now);
        if (poll(&p, 1, left > 0 ? left : 1) <= 0) continue;
        uint8_t in[PKT_MAX]; ssize_t r = recv(fd, in, sizeof in, 0); if (r <= 0) continue;
        ldn_network n; unsigned type; if (ldn_pkt_decode(in, (size_t)r, &type, &n, sizeof n) != (int)sizeof n || type != LDN_PKT_SCAN_RESP) continue;
        answers++;
        int dup = 0; for (int i = 0; i < ngot; i++) if (!memcmp(got[i].bssid, n.bssid, 6) && !memcmp(got[i].id.session, n.id.session, 16)) { got[i] = n; dup = 1; }
        if (!dup && ngot < 16) got[ngot++] = n;
    }
    close(fd);
    for (int i = 0; i < ngot; i++) if (filter_ok(&got[i], f)) { if (found < max && out) out[found] = got[i]; found++; }
    if (found > max) found = max;
    if (count) *count = found;
    char b[16];
    clog_(c, "[ldn] scan: %d network(s) answered, %d match the filter (flags %#x, local id %016llx)%s%s", ngot, found, f ? f->flags : 0,
          f ? (unsigned long long)f->id.intent.local_id : 0ull, found ? ": host " : "", found ? ipstr(out[0].nodes[0].ip, b) : "");
    (void)answers;
    return 0;
}
static uint32_t connect_to(ldn_ctx *c, const ldn_network *net, const ldn_user_config *uc, int version) {
    if (ldn_state(c) != LDN_STATION) return LDN_R_INVALID_STATE;
    uint32_t host = net->nodes[0].ip; char b[16];
    int wait = c->cfg.connect_ms > 0 ? c->cfg.connect_ms : 4000;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) return LDN_R_CONNECT_FAILURE;
    if (c->bind_ip) { struct sockaddr_in me; sin_of(&me, c->bind_ip, 0); bind(fd, (struct sockaddr *)&me, sizeof me); }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a; sin_of(&a, host, c->port);
    uint64_t t0 = mono_ms();
    if (connect(fd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { clog_(c, "[ldn] station: connect %s: %s", ipstr(host, b), strerror(errno)); close(fd); return LDN_R_CONNECT_FAILURE; }
    struct pollfd p = {fd, POLLOUT, 0}; int e = 0; socklen_t el = sizeof e;
    if (poll(&p, 1, wait) <= 0) { clog_(c, "[ldn] station: connect %s:%u timed out", ipstr(host, b), c->port); close(fd); return LDN_R_CONNECT_TIMEOUT; }
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
    if (e) { clog_(c, "[ldn] station: connect %s:%u: %s", ipstr(host, b), c->port, strerror(e)); close(fd); return LDN_R_CONNECT_FAILURE; }
    ldn_node me; memset(&me, 0, sizeof me);
    me.ip = c->ip; memcpy(me.mac, c->mac, 6); me.connected = 1; me.version = (uint16_t)version;
    if (uc) { memcpy(me.name, uc->name, 32); me.name[32] = 0; }
    uint8_t pkt[PKT_MAX]; int pn = ldn_pkt_encode(pkt, sizeof pkt, LDN_PKT_CONNECT, &me, sizeof me);
    if (send_all(fd, pkt, pn)) { close(fd); return LDN_R_CONNECT_FAILURE; }
    conn k = {fd, 0, host, 0, {0}};   // first sync read here, the thread takes the link after
    ldn_network n; int have = 0;
    while (!have) {
        int left = (int)(t0 + (uint64_t)wait - mono_ms());
        if (left <= 0) { clog_(c, "[ldn] station: no answer from host %s (refused or full)", ipstr(host, b)); close(fd); return LDN_R_CONNECT_REJECTED; }
        p = (struct pollfd){fd, POLLIN, 0}; if (poll(&p, 1, left) <= 0) continue;
        ssize_t r = recv(fd, k.buf + k.n, sizeof k.buf - (size_t)k.n, 0);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (r <= 0) { clog_(c, "[ldn] station: host %s closed the connection (refused, full or not accepting)", ipstr(host, b)); close(fd); return LDN_R_CONNECT_REJECTED; }
        k.n += (int)r;
        while (k.n >= (int)sizeof(pkt_hdr)) {
            pkt_hdr h; memcpy(&h, k.buf, sizeof h);
            if (h.magic != LDN_MAGIC) { close(fd); return LDN_R_CONNECT_FAILURE; }
            int len = (int)sizeof h + h.size; if (k.n < len) break;
            unsigned type; int dn = ldn_pkt_decode(k.buf, (size_t)len, &type, &n, sizeof n);
            memmove(k.buf, k.buf + len, (size_t)(k.n - len)); k.n -= len;
            if (type == LDN_PKT_SYNC && dn == (int)sizeof n) { have = 1; break; }
        }
    }
    pthread_mutex_lock(&c->mu);
    if (c->state != LDN_STATION) { pthread_mutex_unlock(&c->mu); close(fd); return LDN_R_INVALID_STATE; }
    c->up = k; c->net = n; c->have_net = 1; c->reason = 0; forget_seen(c);
    c->ucfg = uc ? *uc : c->ucfg;
    memset(&c->ncfg, 0, sizeof c->ncfg); c->ncfg.intent = n.id.intent; c->ncfg.channel = n.channel; c->ncfg.max_nodes = n.max_nodes; c->ncfg.version = n.nodes[0].version;
    c->state = LDN_STATION_CONNECTED; mark_changed(c);
    int me_at = -1; for (int i = 0; i < 8; i++) if (n.nodes[i].connected && n.nodes[i].ip == c->ip) me_at = i;
    clog_(c, "[ldn] station: connected to \"%s\" at %s: %d nodes, this node %d, version %d (host %u)", n.nodes[0].name, ipstr(host, b), n.node_count, me_at, version, n.nodes[0].version);
    pthread_mutex_unlock(&c->mu);
    wake(c); flush_notify(c);
    return 0;
}
uint32_t ldn_connect(ldn_ctx *c, const ldn_network *net, const ldn_user_config *uc, int version) {
    if (!net || !net->nodes[0].ip) return LDN_R_INVALID_ARGUMENT;
    return connect_to(c, net, uc, version);
}
uint32_t ldn_connect_private(ldn_ctx *c, const ldn_net_config *nc, const ldn_sec_param *sp, const ldn_user_config *uc, int version) {
    if (!nc || !sp) return LDN_R_INVALID_ARGUMENT;
    ldn_scan_filter f; memset(&f, 0, sizeof f);
    f.id.intent = nc->intent; memcpy(f.id.session, sp->session, 16); f.flags = 1 | 2;
    ldn_network n[4]; int cnt = 0;
    for (int tries = 0; tries < 3 && !cnt; tries++) ldn_scan(c, n, &cnt, 4, &f);
    if (!cnt) { clog_(c, "[ldn] station: private connect: no network with that session"); return LDN_R_CONNECT_NOT_FOUND; }
    return connect_to(c, &n[0], uc, version);
}

// ---------------------------------------------------------------- queries
uint32_t ldn_get_network(ldn_ctx *c, ldn_network *out, ldn_node_update *upd, int n) {
    pthread_mutex_lock(&c->mu);
    if ((c->state != LDN_AP_CREATED && c->state != LDN_STATION_CONNECTED) || !c->have_net) { pthread_mutex_unlock(&c->mu); return LDN_R_INVALID_STATE; }
    if (out) *out = c->net;
    if (upd && n > 0) {
        for (int i = 0; i < n; i++) memset(&upd[i], 0, sizeof upd[i]);
        for (int i = 0; i < 8; i++) {
            int now = c->net.nodes[i].connected != 0; uint32_t ip = c->net.nodes[i].ip; uint8_t ch = 0;
            if (now && !c->seen[i]) ch = 1;
            else if (!now && c->seen[i]) ch = 2;
            else if (now && ip != c->seen_ip[i]) ch = 3;   // left and another node took the slot between two reads
            if (i < n) upd[i].change = ch;
            c->seen[i] = (uint8_t)now; c->seen_ip[i] = now ? ip : 0;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return 0;
}
uint32_t ldn_get_ipv4(ldn_ctx *c, uint32_t *ip, uint32_t *mask) {
    int st = ldn_state(c);
    if (st != LDN_AP_CREATED && st != LDN_STATION_CONNECTED) { if (ip) *ip = 0; if (mask) *mask = 0; return LDN_R_INVALID_STATE; }
    if (ip) *ip = c->ip;
    if (mask) *mask = c->mask;
    return 0;
}
uint32_t ldn_get_security_param(ldn_ctx *c, ldn_sec_param *out) {
    pthread_mutex_lock(&c->mu);
    uint32_t r = c->have_net ? 0 : LDN_R_INVALID_STATE;
    if (!r) { memcpy(out->random, c->net.security_param, 16); memcpy(out->session, c->net.id.session, 16); }
    pthread_mutex_unlock(&c->mu); return r;
}
uint32_t ldn_get_network_config(ldn_ctx *c, ldn_net_config *out) {
    pthread_mutex_lock(&c->mu);
    uint32_t r = c->have_net ? 0 : LDN_R_INVALID_STATE;
    if (!r) *out = c->ncfg;
    pthread_mutex_unlock(&c->mu); return r;
}
int ldn_on_lan(ldn_ctx *c, uint32_t ip) {
    if (!c) return 0;
    if (ip == 0xffffffffu || (ip >> 24) == 127) return 1;
    if (c->mask && (ip & c->mask) == (c->ip & c->mask)) return 1;
    if (c->mask && ip == (c->ip | ~c->mask)) return 1;
    for (int i = 0; i < c->cfg.npeers; i++) if (c->cfg.peers[i] == ip) return 1;
    int hit = 0;
    pthread_mutex_lock(&c->mu);
    if (c->have_net) for (int i = 0; i < 8; i++) if (c->net.nodes[i].connected && c->net.nodes[i].ip == ip) hit = 1;
    pthread_mutex_unlock(&c->mu);
    return hit;
}
