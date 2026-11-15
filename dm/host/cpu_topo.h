// [cpu-place] CPU topology and the thread placement plan by core capacity (port-common/modules/cpu-place, KB APP-05 / APP-14).
// From DOOM 2016's android/cpu_topo.c (APP-05: ranks the cores by sysfs cpu_capacity, else cpuinfo_max_freq weighted by the core
// type, else the MIDR core type; equal scores = one cluster; the little cluster = the slowest, at most 60% of the fastest or in-order
// cores) with two more roles for the game's own threads and three player modes:
//   - translate: the thread that translates NVN and presents (nvn-translate; the guest thread itself with DM_NVN_SYNC=1);
//   - main: the game's main guest thread (the busiest one in D3);
//   - jobs: the game's job workers (D3 "JobWorker0/1", DM_D3_JOBS pool);
//   - guest: every other guest thread; record / present: DOOM's dm-record / dm-present (kept so DOOM's tree can move to this file).
// A mask of 0 = no affinity (the scheduler decides). Modes (gear "CPU cores", DM_CPU_CORES):
//   auto (0): DOOM's plan for translate / record / present / guest; main = the strong cores (every core outside the little cluster)
//             but the translator's own core when that leaves two or more, else all strong cores; jobs = the strong cores but the
//             translator's when that leaves three or more, else every core but the translator's (never pinned to one core);
//   big  (1): every role on the strong cores (no translator-only core); without a little cluster = no affinity at all;
//   all  (2): no affinity for anything (the scheduler decides).
// DM_CPU_TOPO=index (A/B with builds before the module): the old vulkan_present.cpp rule, translate = the cores with the highest
// cpuinfo_max_freq, nothing else pinned. The pure part (dm_cpu_plan_compute / dm_cpu_plan_mode / dm_cpu_topo_read with a fake
// root) has a host test: port-common/modules/cpu-place/test/t_cpu_place.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DM_CPU_MAX 32
enum { DM_CPU_MODE_AUTO = 0, DM_CPU_MODE_BIG = 1, DM_CPU_MODE_ALL = 2 };

typedef struct {
    int ncpu;                        // CPUs 0..ncpu-1 (_SC_NPROCESSORS_CONF / the cpuN directories, at most 32)
    uint32_t capacity[DM_CPU_MAX];   // /sys/devices/system/cpu/cpuN/cpu_capacity (0 = unreadable)
    uint32_t max_khz[DM_CPU_MAX];    // cpuN/cpufreq/cpuinfo_max_freq (0 = unreadable)
    uint32_t midr[DM_CPU_MAX];       // implementer << 12 | part number (0 = unknown)
} dm_cpu_topo_in_t;

typedef struct {
    int legacy;                      // DM_CPU_TOPO=index: translate is the old rule's, nothing else pinned
    int mode;                        // DM_CPU_MODE_*
    const char *source;              // what ranked the cores: "capacity", "freq", "midr", "none"
    uint32_t all;                    // (1 << ncpu) - 1
    int nclusters;                   // fastest first
    uint32_t cluster[DM_CPU_MAX], cluster_score[DM_CPU_MAX];
    uint32_t little;                 // the little cluster (0 = none)
    uint32_t strong;                 // all & ~little
    uint32_t translate;              // nvn-translate (caller of present); 0 = no pin
    uint32_t record, present;        // DOOM's dm-record / dm-present (unused in D3)
    uint32_t guest;                  // other guest threads; 0 = free
    uint32_t main;                   // the game's main guest thread; 0 = free
    uint32_t jobs;                   // the game's job workers; 0 = free
    uint32_t idx_translate, idx_record, idx_present;   // the old rule's masks, for the log and the test
} dm_cpu_plan_t;

// DOOM's function: mode auto (record / present / guest / translate exactly as DOOM's cpu_topo.c)
void dm_cpu_plan_compute(const dm_cpu_topo_in_t *in, int legacy, dm_cpu_plan_t *out);
void dm_cpu_plan_mode(const dm_cpu_topo_in_t *in, int mode, int legacy, dm_cpu_plan_t *out);
int dm_cpu_mode_parse(const char *s);           // "auto" / "big" / "all" (or 0 / 1 / 2); -1 = not one of them
const char *dm_cpu_mode_name(int mode);
// "[cpu] topology" / "[cpu] plan" text (no prefix); returns the length written
size_t dm_cpu_topo_describe(const dm_cpu_topo_in_t *in, const dm_cpu_plan_t *p, char *buf, size_t n);
size_t dm_cpu_plan_describe(const dm_cpu_plan_t *p, char *buf, size_t n);
size_t dm_cpu_mask_text(uint32_t m, char *buf, size_t n);   // "0-3,7"; "free" for 0
const char *dm_cpu_part_name(uint32_t midr);
// Reads the topology under root ("" = the real /sys and /proc; a test passes a fake tree). Returns ncpu.
int dm_cpu_topo_read(const char *root, dm_cpu_topo_in_t *in);

// ---- runtime (hle_cpu_place.c, Android): placement of the live threads, re-applied after onResume and checked every
// DM_CPU_CHECK_MS (2000) because the system (cpuset moves, HyperOS / MIUI, vendor boosts) rewrites thread masks.
void dm_cpu_place_start(void);                  // once at boot, after the env files: reads the topology, logs, starts "dm-cpu"
const dm_cpu_plan_t *dm_cpu_plan(void);         // the current plan (reads the topology on first use)
void dm_cpu_place_translator(int tid);          // from the presenting thread at every present (cheap: compares a word)
void dm_cpu_place_markers(int on);              // vulkan_present.cpp's own pin markers are set: the translator is left to them
void dm_cpu_place_set_mode(int mode);           // the gear, live (DmActivity.nativeCpuCores)
void dm_cpu_place_focus(int ev);                // nativeFocus: 0 unfocused, 1 background, 2 foreground, 3 active (onResume)

#ifdef __cplusplus
}
#endif
