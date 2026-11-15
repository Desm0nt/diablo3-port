// PC test of dm/android/d3_cpk_check.h (issue #26): every archive given must pass at its real length and fail when read as if it were
// 20 KB, 1 MB or 60 % shorter (archives without files cannot be judged and are reported as such).
// cc -O1 -I dm/android tools/test_cpk_check.c -o test_cpk_check && ./test_cpk_check <romfs>/CPKs/*.cpk <romfs>/Update/CPKs/*.cpk
#include "d3_cpk_check.h"
int main(int argc, char **argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char why[256]; struct stat st; if (stat(argv[i], &st)) { printf("%s: missing\n", argv[i]); bad++; continue; }
        const uint64_t L = (uint64_t)st.st_size;
        int full = d3_cpk_check_len(argv[i], 0, why, sizeof why);
        if (full) { printf("FAIL full %s: %d %s\n", argv[i], full, why); bad++; continue; }
        const uint64_t cut[3] = {L - 20000, L - (1u << 20), L * 4 / 10}; int caught = 0;
        for (int k = 0; k < 3; k++) if (cut[k] < L && d3_cpk_check_len(argv[i], cut[k], why, sizeof why) == 1) caught++;
        printf("%-28s ok, truncations caught %d/3%s\n", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i], caught, caught == 3 ? "" : " (archive without files?)");
        if (caught != 3) { char w2[256]; d3_cpk_check_len(argv[i], cut[0], w2, sizeof w2); printf("    last: %s\n", w2); }
    }
    return bad != 0;
}
