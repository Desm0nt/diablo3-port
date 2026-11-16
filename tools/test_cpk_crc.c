// PC test of d3_cpk_crc (dm/android/d3_cpk_check.h, issue #33): every archive given must pass its full data checksum; with
// --expect-bad every one must fail it (a copy with a flipped byte). Prints the time per archive.
// gcc -O2 -DD3CPK_NO_ZLIB -I dm/android tools/test_cpk_crc.c -o test_cpk_crc && ./test_cpk_crc <romfs>/CPKs/*.cpk <romfs>/Update/CPKs/*.cpk
#include <time.h>
#include "d3_cpk_check.h"
int main(int argc, char **argv) {
    int bad = 0, expect_bad = 0, i0 = 1;
    if (argc > 1 && !strcmp(argv[1], "--expect-bad")) { expect_bad = 1; i0 = 2; }
    for (int i = i0; i < argc; i++) {
        char why[256]; struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        const int r = d3_cpk_crc(argv[i], why, sizeof why);
        clock_gettime(CLOCK_MONOTONIC, &b);
        const double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        const int ok = expect_bad ? r == 1 : r == 0;
        printf("%-30s %s %d %.0f ms %s\n", nm, ok ? "ok  " : "FAIL", r, ms, why);
        if (!ok) bad++;
    }
    return bad != 0;
}
