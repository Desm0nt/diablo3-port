// Host run of the on-phone ASTC cache builder (dm/android/astc_convert.cpp), for tests and timing on a PC (WSL):
//   astc_convert_host <data dir with romfs/ [aoc/]> [threads]   (DM_ASTC_CONV_LIMIT=<n>: only n textures)
// Build (from dm/android/tests): see the command in docs/d3/D3_PORT.md ("Кэш текстур ASTC на телефоне").
#include "../astc_convert.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <data dir> [threads]\n", argv[0]); return 2; }
    if (astc_conv_start(argv[1], argc > 2 ? atoi(argv[2]) : 0)) return 1;
    int64_t p[ASTC_CONV_FIELDS];
    for (;;) { usleep(200000); astc_conv_progress(p); if (p[0] >= ASTC_CONV_DONE) break; }
    printf("state %lld: %lld/%lld textures, %.1f Mpx, %.1f s, %.1f MB, failed %lld\n", (long long)p[0], (long long)p[1], (long long)p[2], p[4] / 1e6, p[5] / 1000.0, p[8] / 1048576.0, (long long)p[11]);
    return p[0] == ASTC_CONV_DONE ? 0 : 1;
}
