// [nonadreno] D3 ASTC texture cache (issue #20; tools/d3_astc_cache.py writes it, setup puts it in Download/Diablo3Mobile/astc).
// GPUs without BC (Mali, PowerVR, Xclipse) used to decode every BC texture to RGBA8: 4x (BC2 / BC3) to 8x (BC1) the memory. The PC
// setup encodes each BC texture of the game's CPKs once to ASTC (BC2 / BC3 -> 4x4, BC1 -> 6x6, 5x5 or 4x4, the largest block that
// keeps mip 0 at >= 42 dB against the decoded BC) into one pack. The engine recognises a texture by its storage as the game put it in
// memory (CRC-32 and Adler-32 of the bytes, their count, NVN format, size, levels) when it creates the Vulkan image, creates the image
// in the entry's ASTC format and uploads the cached blocks; anything the pack does not have stays on the BC decoder.
// Pure functions (no device calls) so that engine/tests/astc_cache_test.cpp runs the production code.
//
// File 'D3AC' v1, little-endian: header (64 bytes: magic, version, entries, flags, index offset u64, data offset u64, data bytes u64),
// the blocks of every entry (level 0 first; a level is tight rows of 16-byte blocks, ceil(w / bw) x ceil(h / bh)), then the index:
// 48-byte entries sorted by (crc, adler, bytes, format, width, height, levels) - the tool's key tuple order.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct XbgAcHeader { char magic[4]; uint32_t version, entries, flags; uint64_t index_off, data_off, data_bytes; uint8_t pad[24]; };
struct XbgAcEntry {
    uint32_t crc, adler, bytes; uint16_t format, levels, width, height; uint8_t bw, bh; uint16_t flags;
    uint64_t data_off; uint32_t data_bytes, psnr_x100; uint8_t pad[8];
};
static_assert(sizeof(XbgAcHeader) == 64 && sizeof(XbgAcEntry) == 48, "D3AC layout");

// 0 = usable header for a file of file_bytes, else -1 with *why.
static inline int xbg_ac_header_check(const XbgAcHeader &h, uint64_t file_bytes, const char **why) {
    if (memcmp(h.magic, "D3AC", 4)) { *why = "not a D3AC pack"; return -1; }
    if (h.version != 1) { *why = "pack version is not 1 (run setup again)"; return -1; }
    if (h.index_off < 64 || h.index_off + uint64_t(h.entries) * sizeof(XbgAcEntry) != file_bytes) { *why = "index does not end the file (incomplete copy?)"; return -1; }
    if (h.data_off != 64 || h.data_off + h.data_bytes != h.index_off) { *why = "data range does not match the index"; return -1; }
    return 0;
}
// Bytes of level `level` of an entry (tight 16-byte blocks).
static inline uint64_t xbg_ac_level_bytes(const XbgAcEntry &e, unsigned level) {
    const uint32_t w = e.width >> level ? e.width >> level : 1, h = e.height >> level ? e.height >> level : 1;
    return uint64_t((w + e.bw - 1) / e.bw) * ((h + e.bh - 1) / e.bh) * 16;
}
// The entry is self-consistent: its levels add up to data_bytes, inside the data range, a block size the engine knows.
static inline bool xbg_ac_entry_ok(const XbgAcEntry &e, const XbgAcHeader &h) {
    if (!((e.bw == 4 && e.bh == 4) || (e.bw == 5 && e.bh == 5) || (e.bw == 6 && e.bh == 6)) || !e.levels || e.levels > 16 || !e.width || !e.height) return false;
    uint64_t n = 0;
    for (unsigned l = 0; l < e.levels; l++) n += xbg_ac_level_bytes(e, l);
    return n == e.data_bytes && e.data_off >= h.data_off && e.data_off + e.data_bytes <= h.data_off + h.data_bytes;
}
static inline bool xbg_ac_less(const XbgAcEntry &a, uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {
    if (a.crc != crc) return a.crc < crc;
    if (a.adler != adler) return a.adler < adler;
    if (a.bytes != bytes) return a.bytes < bytes;
    if (a.format != format) return a.format < format;
    if (a.width != w) return a.width < w;
    if (a.height != h) return a.height < h;
    return a.levels < levels;
}
// Index of the entry with this key in the sorted index, or -1.
static inline int64_t xbg_ac_find(const XbgAcEntry *idx, uint32_t n, uint32_t crc, uint32_t adler, uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (xbg_ac_less(idx[mid], crc, adler, bytes, format, levels, w, h)) lo = mid + 1; else hi = mid; }
    if (lo < n) { const XbgAcEntry &e = idx[lo];
        if (e.crc == crc && e.adler == adler && e.bytes == bytes && e.format == format && e.levels == levels && e.width == w && e.height == h) return lo; }
    return -1;
}
// The shape part of the key, for a cheap "can the pack have it" test before hashing guest memory.
static inline uint64_t xbg_ac_shape(uint32_t bytes, uint16_t format, uint16_t levels, uint16_t w, uint16_t h) {
    return (uint64_t(bytes) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(format) << 48 | uint64_t(levels) << 40 | uint64_t(w) << 20 | uint64_t(h));
}
// sRGB-ness of an NVN BC format (BC1-BC3 sRGB 0x45-0x48, BC7 sRGB 0x4e).
static inline bool xbg_ac_srgb(uint32_t nvn) { return (nvn >= 0x45 && nvn <= 0x48) || nvn == 0x4e; }
// The VkFormat of an entry's blocks with the sRGB-ness of `nvn` (the texture's or a view's format).
static inline VkFormat xbg_ac_format(uint8_t bw, uint8_t bh, uint32_t nvn) {
    const bool s = xbg_ac_srgb(nvn);
    if (bw == 4 && bh == 4) return s ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    if (bw == 5 && bh == 5) return s ? VK_FORMAT_ASTC_5x5_SRGB_BLOCK : VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    if (bw == 6 && bh == 6) return s ? VK_FORMAT_ASTC_6x6_SRGB_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    return VK_FORMAT_UNDEFINED;
}
// KB TEX-04: an all-zero ASTC block is the error block (Mali: error colour, bloom / TAA spread it). Each one becomes the LDR void
// extent of transparent black. Returns the number replaced (the tool already does this; 0 expected).
static inline unsigned xbg_ac_sanitize(uint8_t *blocks, uint64_t bytes) {
    static const uint8_t void_black[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0;
    for (uint64_t o = 0; o + 16 <= bytes; o += 16) {
        uint64_t a, b; memcpy(&a, blocks + o, 8); memcpy(&b, blocks + o + 8, 8);
        if (!(a | b)) { memcpy(blocks + o, void_black, 16); ++n; }
    }
    return n;
}
