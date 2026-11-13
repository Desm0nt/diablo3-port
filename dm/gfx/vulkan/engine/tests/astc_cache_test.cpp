// [nonadreno] issue #20: the ASTC texture cache's pure functions (xbg_astc_cache.h) on synthetic data, and with arguments a real pack
// written by tools/d3_astc_cache.py: header, every entry, index order, no all-zero blocks; with a storage dump (name.st and the key
// the tool printed for it) the engine's lookup finds the tool's entry.
// Build and run (WSL, from this directory; Vulkan headers from the NDK sysroot or libvulkan-dev, zlib):
//   g++ -std=c++17 -O2 -Wall -I.. -I<vulkan include> -o astc_cache_test astc_cache_test.cpp -lz && ./astc_cache_test [pack [storage fmt w h levels]...]
#include "../xbg_astc_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <zlib.h>

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)) { if(failures++ < 30) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while(0)

static XbgAcEntry mk(uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t fmt, uint16_t lv, uint16_t w, uint16_t h, uint8_t b) {
    XbgAcEntry e{}; e.crc = crc; e.adler = adler; e.bytes = bytes; e.format = fmt; e.levels = lv; e.width = w; e.height = h; e.bw = e.bh = b; return e;
}

static void synthetic() {
    std::vector<XbgAcEntry> v = {mk(1, 5, 100, 0x42, 1, 64, 64, 6), mk(1, 6, 100, 0x42, 1, 64, 64, 6), mk(2, 1, 4096, 0x44, 7, 128, 128, 4), mk(9, 9, 1, 0x43, 1, 4, 4, 4)};
    CHECK(xbg_ac_find(v.data(), 4, 1, 6, 100, 0x42, 1, 64, 64) == 1, "find second");
    CHECK(xbg_ac_find(v.data(), 4, 2, 1, 4096, 0x44, 7, 128, 128) == 2, "find third");
    CHECK(xbg_ac_find(v.data(), 4, 2, 1, 4096, 0x44, 7, 128, 64) == -1, "other height");
    CHECK(xbg_ac_find(v.data(), 4, 0, 0, 0, 0, 0, 0, 0) == -1 && xbg_ac_find(v.data(), 4, 99, 0, 0, 0, 0, 0, 0) == -1, "outside");
    XbgAcEntry e = mk(0, 0, 0, 0x42, 3, 64, 30, 6);
    CHECK(xbg_ac_level_bytes(e, 0) == 11 * 5 * 16 && xbg_ac_level_bytes(e, 1) == 6 * 3 * 16 && xbg_ac_level_bytes(e, 2) == 3 * 2 * 16, "6x6 level bytes");
    e.bw = e.bh = 4; CHECK(xbg_ac_level_bytes(e, 6) == 16, "1x1 level");
    CHECK(xbg_ac_format(4, 4, 0x44) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK && xbg_ac_format(4, 4, 0x48) == VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "4x4 formats");
    CHECK(xbg_ac_format(6, 6, 0x42) == VK_FORMAT_ASTC_6x6_UNORM_BLOCK && xbg_ac_format(5, 5, 0x46) == VK_FORMAT_ASTC_5x5_SRGB_BLOCK && xbg_ac_format(8, 8, 0x42) == VK_FORMAT_UNDEFINED, "other formats");
    uint8_t blk[48] = {}; blk[20] = 1;
    CHECK(xbg_ac_sanitize(blk, 48) == 2 && blk[0] == 0xFC && blk[1] == 0xFD && blk[7] == 0xFF && blk[8] == 0 && blk[20] == 1 && blk[32] == 0xFC, "sanitize");
    XbgAcHeader h{}; memcpy(h.magic, "D3AC", 4); h.version = 1; h.entries = 2; h.index_off = 64 + 1000; h.data_off = 64; h.data_bytes = 1000;
    const char *why = nullptr;
    CHECK(xbg_ac_header_check(h, 64 + 1000 + 96, &why) == 0, "header ok");
    CHECK(xbg_ac_header_check(h, 64 + 1000 + 95, &why) == -1 && why, "truncated file");
    h.version = 2; CHECK(xbg_ac_header_check(h, 64 + 1000 + 96, &why) == -1, "version");
}

static std::vector<uint8_t> slurp(const char *p) {
    std::vector<uint8_t> v; FILE *f = fopen(p, "rb"); if(!f) return v;
    fseek(f, 0, SEEK_END); v.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET); if(fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); fclose(f); return v;
}

static void pack(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if(!f) {CHECK(0, "cannot open %s", argv[1]); return;}
    fseek(f, 0, SEEK_END); const uint64_t size = uint64_t(ftello(f)); fseek(f, 0, SEEK_SET);
    XbgAcHeader h{}; CHECK(fread(&h, sizeof h, 1, f) == 1, "header read");
    const char *why = nullptr; CHECK(xbg_ac_header_check(h, size, &why) == 0, "header: %s", why ? why : "");
    std::vector<XbgAcEntry> idx(h.entries); fseeko(f, off_t(h.index_off), SEEK_SET); CHECK(fread(idx.data(), sizeof(XbgAcEntry), h.entries, f) == h.entries, "index read");
    unsigned bad = 0, order = 0, zero = 0; uint64_t blocks = 0; std::vector<uint8_t> buf;
    for(uint32_t i = 0; i < h.entries; i++) {
        const XbgAcEntry &e = idx[i];
        if(!xbg_ac_entry_ok(e, h)) ++bad;
        if(i && !xbg_ac_less(idx[i - 1], e.crc, e.adler, e.bytes, e.format, e.levels, e.width, e.height)) ++order;
        buf.resize(e.data_bytes); fseeko(f, off_t(e.data_off), SEEK_SET);
        if(fread(buf.data(), 1, buf.size(), f) != buf.size()) {++bad; continue;}
        zero += xbg_ac_sanitize(buf.data(), buf.size()); blocks += buf.size() / 16;
    }
    CHECK(!bad && !order && !zero, "pack: %u bad entries, %u out of order, %u zero blocks", bad, order, zero);
    printf("pack %s: %u entries, %llu blocks, %u bad, %u out of order, %u all-zero blocks\n", argv[1], h.entries, (unsigned long long)blocks, bad, order, zero);
    for(int a = 2; a + 4 < argc; a += 5) {   // storage dump + fmt w h levels: the lookup the engine does
        std::vector<uint8_t> st = slurp(argv[a]);
        const uint32_t crc = uint32_t(crc32(0, st.data(), uInt(st.size()))), adler = uint32_t(adler32(1, st.data(), uInt(st.size())));
        const int64_t i = xbg_ac_find(idx.data(), h.entries, crc, adler, uint32_t(st.size()), uint16_t(strtoul(argv[a + 1], 0, 0)), uint16_t(atoi(argv[a + 4])), uint16_t(atoi(argv[a + 2])), uint16_t(atoi(argv[a + 3])));
        CHECK(i >= 0, "%s not found", argv[a]);
        if(i >= 0) printf("  %s -> entry %lld: ASTC %ux%u, %u bytes, mip 0 %.2f dB\n", argv[a], (long long)i, idx[size_t(i)].bw, idx[size_t(i)].bh, idx[size_t(i)].data_bytes, idx[size_t(i)].psnr_x100 / 100.0);
    }
    fclose(f);
}

// Round 4: the partial cache's journal (only complete lines, only cached textures, only inside the part file).
static void journal() {
    const char *j = "D3AC-phone 1 123\n#total 5\n"
        "0000000a 00000001 4096 44 64 64 1 4 4300 64 4096 0 1\n"      // 4x4: 16x16 blocks = 4096 bytes
        "0000000b 00000002 2048 42 64 64 1 0 3800 0 0 2 5\n"          // stays BC
        "0000000c 00000003 2048 42 64 64 1 6 4500 4160 1936 0 1\n"    // 6x6: 11x11 blocks = 1936 bytes
        "0000000d 00000004 2048 42 64 64 1 6 4500 99999 1936 0 1\n"   // past the part file
        "0000000e 00000005 2048 42 64 64 1 6 45";                     // torn tail
    std::vector<XbgAcEntry> e; uint64_t total = 0;
    CHECK(xbg_ac_journal_parse(j, strlen(j), 64 + 4096 + 1936, e, &total) == 0 && total == 5, "journal parse");
    CHECK(e.size() == 2 && e[0].crc == 0xa && e[1].crc == 0xc && e[1].bw == 6 && e[1].data_off == 4160, "journal entries: %zu", e.size());
    CHECK(xbg_ac_journal_parse("garbage\n", 8, 0, e, &total) == -1, "not a journal");
}

int main(int argc, char **argv) {
    synthetic();
    journal();
    if (argc == 4 && !strcmp(argv[1], "--journal")) {   // a real part file + journal: what the engine would serve
        std::vector<uint8_t> t = slurp(argv[3]); FILE *f = fopen(argv[2], "rb"); uint64_t n = 0;
        if (f) { fseek(f, 0, SEEK_END); n = uint64_t(ftello(f)); fclose(f); }
        std::vector<XbgAcEntry> e; uint64_t total = 0;
        CHECK(xbg_ac_journal_parse((const char *)t.data(), t.size(), n, e, &total) == 0, "real journal");
        printf("partial: %zu of %llu textures committed, part file %llu bytes\n", e.size(), (unsigned long long)total, (unsigned long long)n);
        printf(failures ? "astc_cache_test: %d FAILURES\n" : "astc_cache_test: all passed\n", failures);
        return failures != 0;
    }
    if(argc > 1) pack(argc, argv);
    printf(failures ? "astc_cache_test: %d FAILURES\n" : "astc_cache_test: all passed\n", failures);
    return failures != 0;
}
