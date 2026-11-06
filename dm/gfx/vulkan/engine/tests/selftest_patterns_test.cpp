// The GPU self-test's compute module (xbg_selftest.comp -> xbg_selftest_spv.h) must hold the patterns each rewrite looks for,
// or its check would compare a module with itself. Calls the production rewrites on it (host build, e.g.
// cl /std:c++17 /EHsc selftest_patterns_test.cpp ../xbg_spirv_peep.cpp ../xbg_spirv_ldc.cpp).
#include "../xbg_selftest_spv.h"
#include "../xbg_spirv_ldc.h"
#include "../xbg_spirv_peep.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const std::vector<uint32_t> cs(xbg_selftest_cs, xbg_selftest_cs + sizeof(xbg_selftest_cs) / 4);
    int bad = 0;
    // FClamp(x, 0.0, 1.0) (GLSL.std.450 43 with constants 0 and 0x3f800000): what DM_GFX_NAN_SAT keeps
    {
        uint32_t glsl = 0; size_t sat = 0, clamps = 0; std::vector<std::pair<uint32_t, uint32_t>> fc;
        for (size_t i = 5; i < cs.size();) {
            uint32_t op = cs[i] & 0xffff, n = cs[i] >> 16; if (!n) break;
            if (op == 43 && n == 4) fc.push_back({cs[i + 2], cs[i + 3]});
            if (op == 11 && n >= 5 && !memcmp(&cs[i + 2], "GLSL.std.450", 13)) glsl = cs[i + 1];
            if (op == 12 && n >= 8 && cs[i + 3] == glsl && cs[i + 4] == 43) {
                ++clamps; uint32_t lo = ~0u, hi = ~0u;
                for (auto &c : fc) { if (c.first == cs[i + 6]) lo = c.second; if (c.first == cs[i + 7]) hi = c.second; }
                if (lo == 0 && hi == 0x3f800000u) ++sat;
            }
            i += n;
        }
        printf("FClamp %zu, of them saturates %zu (want 3 and 1)\n", clamps, sat); bad += !(clamps == 3 && sat == 1);
    }
    std::vector<uint32_t> out;
    size_t f2i = xbg_spirv_f2i_notrunc(cs.data(), cs.size(), out);
    printf("f2i_notrunc rewrites %zu (want 2)\n", f2i); bad += f2i != 2;
    size_t merges = 0, canon = xbg_spirv_ldc_canon(cs.data(), cs.size(), out, &merges);
    printf("ldc_canon shifts %zu merges %zu (want 2 and 1)\n", canon, merges); bad += !(canon == 2 && merges == 1);
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad ? 1 : 0;
}
