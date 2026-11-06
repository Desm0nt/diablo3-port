// [d3-perf][nonadreno] GPU self-test of the exact optimisations D3 turns on by default (see xbg_selftest.cpp). They were
// verified only on Adreno; on any other GPU the engine runs this once at initialization, on the device itself, and turns off
// each one whose check fails.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

enum XbgSelftestCheck { XBG_ST_REPORT_INPASS = 0, XBG_ST_LAZY_CLEAR, XBG_ST_NAN_SAT, XBG_ST_LDC_CANON, XBG_ST_F2I_NOTRUNC, XBG_ST_COUNT };

struct XbgSelftestIn {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;       // used only during the call, while nothing else submits (engine initialization)
    uint32_t queue_family = 0;
    bool precise_occlusion = false;       // occlusionQueryPrecise enabled on the device
    uint32_t timestamp_bits = 0;          // the queue family's timestampValidBits
    uint32_t force_fail = 0;              // bit per XbgSelftestCheck (DM_GFX_SELFTEST_FORCE_FAIL)
    uint64_t timeout_ns = 1000000000ull;  // GPU wait; a timeout fails every check
    // The engine's DM_GFX_NAN_MINMAX swap (xbg_game.cpp nan_minmax_swap): FMin / FMax / FClamp -> NMin / NMax / NClamp, with sat
    // keeping FClamp(x, 0, 1) (DM_GFX_NAN_SAT); *kept receives how many it kept.
    size_t (*nan_swap)(std::vector<uint32_t> &words, bool sat, size_t *kept) = nullptr;
};

struct XbgSelftestOut {
    bool ran = false;                     // the GPU work was submitted and finished
    int pass[XBG_ST_COUNT] = {};          // 1 PASS, 0 FAIL (also when the test itself could not run)
    char note[XBG_ST_COUNT][200] = {};
    bool copy_nowait_ok = true;           // vkCmdCopyQueryPoolResults without WAIT_BIT gave the waited results
    char query_note[96] = {};
    char error[160] = {};                 // the first Vulkan failure or timeout
    double ms = 0;
};

const char *xbg_selftest_name(int check);
// "report_inpass,lazy_clear" / "all" (commas or spaces) -> bit mask of XbgSelftestCheck
uint32_t xbg_selftest_parse(const char *list);
void xbg_selftest_run(const XbgSelftestIn &in, XbgSelftestOut &out);
