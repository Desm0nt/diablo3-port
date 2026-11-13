#!/usr/bin/env python3
"""A PC-side peer of the ports' local wireless play over the LAN (dm/host/hle_ldn_core.c, ldn_mitm's LAN protocol), to test one phone
from a PC on the same Wi-Fi without a second phone or the game on the PC.

  ldn_peer.py scan  [--peer IP ...]                 list the networks hosts answer with (a phone with the game's local game open)
  ldn_peer.py join  [--peer IP] [--name NAME]        join the first network found as a station and print every sync (nodes, advertise)
  ldn_peer.py host  --local-id HEX [--name NAME]     pretend to host a network of that game (D3 01001b300b9be000, TL2 010090400d366000):
                                                     a phone's search lists it; a phone that joins appears as node 1

Only the LDN layer is exercised: a phone that joins this fake host then tries to reach the game's own server on the PC and gives up,
and this station never opens the game's sockets. Port 11452 (--port). Python 3.8+, standard library only.
Copyright (c) 2026 Desm0nt. MIT License.
"""
import argparse, os, socket, struct, sys, time

MAGIC = 0x11451400
SCAN, SCAN_RESP, CONNECT, SYNC = 0, 1, 2, 3
NET_SIZE, NODE_SIZE = 0x480, 0x40


def zcompress(b):
    out = bytearray(); i = 0
    while i < len(b):
        if b[i]: out.append(b[i]); i += 1; continue
        run = 1
        while i + run < len(b) and not b[i + run] and run < 256: run += 1
        out += bytes((0, run - 1)); i += run
    return bytes(out)


def zdecompress(b):
    out = bytearray(); i = 0
    while i < len(b):
        if b[i]: out.append(b[i]); i += 1; continue
        out += bytes(b[i + 1] + 1); i += 2
    return bytes(out)


def pkt(t, payload=b''):
    z = zcompress(payload) if payload else b''
    if payload and len(z) < len(payload):
        return struct.pack('<IBBHH2x', MAGIC, t, 1, len(z), len(payload)) + z
    return struct.pack('<IBBHH2x', MAGIC, t, 0, len(payload), len(payload)) + payload


def unpkt(b):
    if len(b) < 12: return None
    magic, t, comp, size, raw = struct.unpack_from('<IBBHH', b)
    if magic != MAGIC or len(b) < 12 + size: return None
    pl = b[12:12 + size]
    return t, (zdecompress(pl) if comp else pl), 12 + size


def ip_s(v): return socket.inet_ntoa(struct.pack('>I', v))


def describe(n):
    local_id, scene = struct.unpack_from('<Q2xH', n, 0)
    ch, = struct.unpack_from('<H', n, 0x48)
    maxn, cnt = n[0x66], n[0x67]
    adv_n, = struct.unpack_from('<H', n, 0x26a)
    lines = ['network local id %016x scene %d channel %d, %d/%d nodes, advertise %d bytes: %s' %
             (local_id, scene, ch, cnt, maxn, adv_n, n[0x26c:0x26c + min(adv_n, 48)].hex())]
    for i in range(8):
        o = 0x68 + i * NODE_SIZE
        ip, = struct.unpack_from('<I', n, o)
        if n[o + 11]:
            name = n[o + 12:o + 45].split(b'\0')[0].decode('utf-8', 'replace')
            ver, = struct.unpack_from('<H', n, o + 0x2e)
            lines.append('  node %d: %s "%s" version %d' % (i, ip_s(ip), name, ver))
    return '\n'.join(lines)


def my_ip(target='10.255.255.255'):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try: s.connect((target, 9)); return s.getsockname()[0]
    finally: s.close()


def do_scan(a, quiet=False):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1); s.bind(('', 0))
    me = my_ip(); base = me.rsplit('.', 1)[0]
    targets = ['255.255.255.255', base + '.255'] + (a.peer or []) + ['%s.%d' % (base, h) for h in range(1, 255)]
    for t in targets:
        try: s.sendto(pkt(SCAN), (t, a.port))
        except OSError: pass
    found = {}; end = time.time() + a.wait
    while time.time() < end:
        s.settimeout(max(0.05, end - time.time()))
        try: b, src = s.recvfrom(4096)
        except socket.timeout: break
        r = unpkt(b)
        if r and r[0] == SCAN_RESP and len(r[1]) == NET_SIZE: found[r[1][0x20:0x26] + r[1][0x10:0x20]] = (src, r[1])
    if not quiet:
        print('scan from %s: %d network(s)' % (me, len(found)))
        for src, n in found.values(): print('from %s:%d\n%s' % (src[0], src[1], describe(n)))
    return [n for _, n in found.values()]


def node_bytes(ip, name, version=0):
    mac = bytes((2, 0)) + socket.inet_aton(ip)
    nm = name.encode()[:32].ljust(33, b'\0')
    return struct.pack('<I6sbB33sBH16x', struct.unpack('>I', socket.inet_aton(ip))[0], mac, 0, 1, nm, 0, version)


def do_join(a):
    nets = do_scan(a)
    if not nets: sys.exit('nothing to join')
    n = nets[0]; host, = struct.unpack_from('<I', n, 0x68); ver, = struct.unpack_from('<H', n, 0x68 + 0x2e)
    c = socket.create_connection((ip_s(host), a.port), timeout=5)
    me = c.getsockname()[0]
    c.sendall(pkt(CONNECT, node_bytes(me, a.name, ver)))
    print('joined %s as %s, waiting for syncs (Ctrl+C to leave)' % (ip_s(host), me))
    buf = b''; c.settimeout(None)
    while True:
        d = c.recv(65536)
        if not d: print('host closed the link'); return
        buf += d
        while True:
            r = unpkt(buf)
            if not r: break
            buf = buf[r[2]:]
            if r[0] == SYNC: print(time.strftime('%H:%M:%S'), 'sync\n' + describe(r[1]))


def do_host(a):
    me = my_ip()
    n = bytearray(NET_SIZE)
    struct.pack_into('<QxxH', n, 0, int(a.local_id, 16), a.scene)
    n[0x10:0x20] = os.urandom(16)
    n[0x20:0x26] = bytes((2, 0)) + socket.inet_aton(me)
    n[0x26] = 32; n[0x27:0x47] = os.urandom(16).hex().encode()
    struct.pack_into('<HbB', n, 0x48, 6, 3, 2)
    n[0x50:0x60] = os.urandom(16); struct.pack_into('<H', n, 0x60, 1)
    n[0x66] = a.max; n[0x67] = 1
    for i in range(8): n[0x68 + i * NODE_SIZE + 10] = i
    n[0x68:0x68 + NODE_SIZE] = node_bytes(me, a.name, a.version)
    adv = bytes.fromhex(a.advertise) if a.advertise else b'ldn_peer.py'
    struct.pack_into('<H', n, 0x26a, len(adv)); n[0x26c:0x26c + len(adv)] = adv
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); u.bind(('', a.port))
    t = socket.socket(); t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); t.bind(('', a.port)); t.listen(8)
    print('hosting at %s:%d\n%s' % (me, a.port, describe(bytes(n))))
    import select
    peers = {}
    while True:
        r, _, _ = select.select([u, t] + list(peers), [], [], 1)
        for s in r:
            if s is u:
                b, src = u.recvfrom(4096); p = unpkt(b)
                if p and p[0] == SCAN: u.sendto(pkt(SCAN_RESP, bytes(n)), src); print('scan from %s:%d answered' % src)
            elif s is t:
                c, src = t.accept(); peers[c] = [b'', None]; print('connection from %s:%d' % src)
            else:
                d = s.recv(65536); st = peers[s]
                if not d:
                    if st[1] is not None:
                        n[0x68 + st[1] * NODE_SIZE:0x68 + (st[1] + 1) * NODE_SIZE] = bytes(NODE_SIZE); n[0x68 + st[1] * NODE_SIZE + 10] = st[1]; n[0x67] -= 1
                        print('node %d left' % st[1])
                        for o in peers:
                            if o is not s: o.sendall(pkt(SYNC, bytes(n)))
                    del peers[s]; s.close(); continue
                st[0] += d
                p = unpkt(st[0])
                if p and p[0] == CONNECT and st[1] is None:
                    st[0] = st[0][p[2]:]
                    slot = next((i for i in range(1, a.max) if not n[0x68 + i * NODE_SIZE + 11]), None)
                    if slot is None: print('full: refused'); s.close(); del peers[s]; continue
                    node = bytearray(p[1]); node[10] = slot; node[11] = 1
                    n[0x68 + slot * NODE_SIZE:0x68 + (slot + 1) * NODE_SIZE] = node; n[0x67] += 1; st[1] = slot
                    print('node %d joined:\n%s' % (slot, describe(bytes(n))))
                    for o in peers: o.sendall(pkt(SYNC, bytes(n)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['scan', 'join', 'host'])
    ap.add_argument('--port', type=int, default=11452)
    ap.add_argument('--peer', action='append', help='extra unicast scan target')
    ap.add_argument('--wait', type=float, default=1.0)
    ap.add_argument('--name', default='PC peer')
    ap.add_argument('--local-id', default='01001b300b9be000')
    ap.add_argument('--scene', type=int, default=0)
    ap.add_argument('--max', type=int, default=4)
    ap.add_argument('--version', type=int, default=0)
    ap.add_argument('--advertise', help='hex bytes')
    a = ap.parse_args()
    {'scan': do_scan, 'join': do_join, 'host': do_host}[a.mode](a)


if __name__ == '__main__':
    main()
