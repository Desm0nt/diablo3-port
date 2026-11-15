// [D3] Start check of the game's archives (GitHub issue #26): a *.cpk cut short by an interrupted copy to the phone (the player's
// romfs/CPKs/Act4.cpk had 459 of 1171 MB) let the game start and then die after the hero was chosen, when it loaded the world:
// "FORTIFY: pread: count 18446744072536612864 > SSIZE_MAX". The game derives the number of compressed sectors of an archive from the
// file's length; every table after that count (sector map, file names) is then read from the wrong place and it computes garbage sizes.
//
// The same arithmetic tells whether a file is complete, for any language pack and without a list of sizes: with the real length the
// archive's name table lines up and its first names hash (FNV-1a 64, lower case) to entries of its file table; with a shorter length the
// name table is read from a shifted place and nothing matches. Only the header region is read (tens of KB to a few MB per archive).
// Layout: tools/d3cpklib.py (CPKArchive._parse_header / _parse_tables), the reader this check follows step by step.
//
// d3_cpk_check(path, why, n): 0 = looks complete (or a layout this check does not know: never blocks on a doubt), 1 = damaged (why says
// how), -1 = cannot be read (why says why; the caller decides). Self-contained so tools/test_cpk_check.c can run it on a PC.
#ifndef D3_CPK_CHECK_H
#define D3_CPK_CHECK_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t d3cpk_bits(const uint8_t *buf, uint64_t bit_pos, unsigned n) {   // MSB-first, n <= 64 (d3cpklib._read_bits)
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) { uint64_t b = bit_pos + i; v = (v << 1) | ((buf[b >> 3] >> (7 - (b & 7))) & 1); }
    return v;
}
static uint64_t d3cpk_fnv(const char *s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *s; s++) { uint8_t c = (uint8_t)*s; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 0x100000001B3ull; }
    return h;
}
static int d3cpk_read(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) { ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}

static int d3_cpk_check_len(const char *path, uint64_t as_len, char *why, size_t n) {   // as_len: 0 = the real length (tests pass a shorter one)
    why[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { snprintf(why, n, "cannot be opened (errno %d)", errno); return -1; }
    struct stat st; if (fstat(fd, &st)) { snprintf(why, n, "cannot be read (errno %d)", errno); close(fd); return -1; }
    const uint64_t len = as_len && as_len < (uint64_t)st.st_size ? as_len : (uint64_t)st.st_size;
    int rc = 0; uint8_t h[72] = {0}; uint8_t *sfi = NULL;
    if (len < 72 || d3cpk_read(fd, h, 72, 0)) { snprintf(why, n, "only %llu bytes long", (unsigned long long)len); rc = 1; goto out; }
    int big;
    if (h[0] == 0xA1 && h[1] == 0xB2 && h[2] == 0xC3 && h[3] == 0xD4) big = 1;
    else if (h[0] == 0xD4 && h[1] == 0xC3 && h[2] == 0xB2 && h[3] == 0xA1) big = 0;
    else { snprintf(why, n, "not an archive (starts with %02x %02x %02x %02x: overwritten or never copied)", h[0], h[1], h[2], h[3]); rc = 1; goto out; }
#define U32(o) (big ? ((uint32_t)h[o] << 24 | (uint32_t)h[(o) + 1] << 16 | (uint32_t)h[(o) + 2] << 8 | h[(o) + 3]) \
                    : ((uint32_t)h[(o) + 3] << 24 | (uint32_t)h[(o) + 2] << 16 | (uint32_t)h[(o) + 1] << 8 | h[o]))
    const uint32_t ver = U32(4), fmt = U32(16), fc = U32(20), lc = U32(24), hs = U32(28);
    const uint64_t dfs = big ? ((uint64_t)U32(8) << 32 | U32(12)) : ((uint64_t)U32(12) << 32 | U32(8));
    const uint32_t fs_bc = U32(32), flc_bc = U32(36), fli_bc = U32(40), loc_bc = U32(44), cs_bc = U32(48), ds_bc = U32(52);
    uint64_t rs, cms, hsz;
    if (ver == 6) { rs = 0x10000; cms = 0x4000; hsz = 64; }
    else if (ver == 7) { rs = U32(60); cms = U32(64); hsz = 72; }
    else goto out;                                                       // another version: not ours to judge
#undef U32
    if (!fc || fc > 10000000u || dfs > 100000000000ull || !cms || !rs || fs_bc > 64 || flc_bc > 64 || fli_bc > 64 || loc_bc > 64 || cs_bc > 64 || ds_bc > 64) goto out;
    if (fmt == 0 && len - (uint64_t)hs * rs == dfs) goto out;            // raw cutscene archive (plain Ogg after the header): no tables
    const uint64_t eb = 64 + fs_bc + flc_bc + fli_bc, sfi_bytes = ((uint64_t)fc * eb + 7) / 8;
    uint64_t pos = hsz + sfi_bytes;
    pos += ((uint64_t)loc_bc * lc + 7) / 8;
    uint64_t first;
    if (fmt == 10) first = hs;
    else { first = (rs * hs) & 0xFFFF0000ull; if (first % rs) first += rs; }
    const uint64_t nsec = len + cms - 1 > first ? (cms + len - 1 - first) / cms : 0;   // what the game derives from the length
    pos += (nsec * cs_bc + 7) / 8;
    pos += (((dfs + cms - 1) / cms) * ds_bc + 7) / 8;
    const uint64_t names = pos + (uint64_t)fc * 4;
    if (names > len) { snprintf(why, n, "cut short: %llu MB, its tables alone need more", (unsigned long long)(len >> 20)); rc = 1; goto out; }
    sfi = malloc(sfi_bytes + 8);
    if (!sfi || d3cpk_read(fd, sfi, sfi_bytes, hsz)) { snprintf(why, n, "file table cannot be read"); rc = -1; goto out; }
    const unsigned k = fc < 16 ? fc : 16; unsigned match = 0, tried = 0;
    for (unsigned i = 0; i < k; i++) {
        uint8_t o4[4]; if (d3cpk_read(fd, o4, 4, pos + (uint64_t)i * 4)) break;
        const uint32_t off = big ? ((uint32_t)o4[0] << 24 | (uint32_t)o4[1] << 16 | (uint32_t)o4[2] << 8 | o4[3]) : ((uint32_t)o4[3] << 24 | (uint32_t)o4[2] << 16 | (uint32_t)o4[1] << 8 | o4[0]);
        tried++;
        if (names + off >= len) continue;
        char nm[300] = {0}; size_t want = len - (names + off) < sizeof nm - 1 ? (size_t)(len - (names + off)) : sizeof nm - 1;
        if (d3cpk_read(fd, nm, want, names + off)) continue;
        nm[sizeof nm - 1] = 0; if (!nm[0] || strlen(nm) >= sizeof nm - 1) continue;
        const uint64_t hv = d3cpk_fnv(nm);
        for (uint32_t j = 0; j < fc; j++) if (d3cpk_bits(sfi, (uint64_t)j * eb, 64) == hv) { match++; break; }
    }
    if (tried && match * 2 < tried) {
        snprintf(why, n, "cut short or damaged: %llu MB, its file names do not line up (%u of %u)", (unsigned long long)(len >> 20), match, tried); rc = 1; }
out:
    free(sfi); close(fd);
    return rc;
}
static int d3_cpk_check(const char *path, char *why, size_t n) { return d3_cpk_check_len(path, 0, why, n); }
#endif
