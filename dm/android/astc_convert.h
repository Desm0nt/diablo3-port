// [D3] On-phone ASTC texture cache builder (astc_convert.cpp): the C API the JNI layer (and the host test) calls.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { ASTC_CONV_IDLE = 0, ASTC_CONV_PLANNING = 1, ASTC_CONV_ENCODING = 2, ASTC_CONV_FINISHING = 3, ASTC_CONV_DONE = 4, ASTC_CONV_CANCELLED = 5, ASTC_CONV_FAILED = 6 };
// progress fields: state, textures done, textures total, pixels done, pixels total, elapsed ms, active thread limit, threads, bytes written,
// keys read, keys total, failed textures
enum { ASTC_CONV_FIELDS = 12 };
int astc_conv_big_cores(void);                    // cores whose cpufreq max is above the smallest cluster's (all when equal)
int astc_conv_estimate_minutes(void);             // the whole cache on this phone's big cores (no thermal throttling)
void astc_conv_set_log(void (*fn)(const char *)); // where "[astc] ..." lines go (stderr when unset)
int astc_conv_start(const char *data_dir, int threads);   // <data>/romfs, <data>/aoc -> <data>/astc; threads 0 = big cores; 1 = already running
void astc_conv_limit(int threads);                // run on at most this many threads (thermal)
void astc_conv_cancel(void);                      // stop; finished textures stay journaled, the next start resumes
void astc_conv_progress(int64_t out[ASTC_CONV_FIELDS]);
#ifdef __cplusplus
}
#endif
