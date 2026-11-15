// [adpf-boost] The decisions of perf_android.c's hint session as plain functions (no Android, no I/O), so a host test can drive them
// (port-common/modules/adpf-boost/test/t_adpf_boost.c). perf_android.c owns the session, the clocks and the /proc reads.
#pragma once
#include <stdint.h>

// ---- workload-increase hint: when to ask for more CPU
// Per present (presenting thread). A load-up is due when `frames` frames in a row did more CPU work than the target, or when the frames
// of a whole second came on average more than 15% slower than the target (fps under the game's pace); at most one per `min_ns`.
// A hold (load screen, pause, background) clears the run and the second.
typedef struct { int over; unsigned win_n; int64_t win_t, last_up; } dm_boost_t;
enum { DM_BOOST_NONE, DM_BOOST_WORK, DM_BOOST_SLOW, DM_BOOST_LIMITED };
static inline void dm_boost_clear(dm_boost_t *s) { s->over = 0; s->win_t = 0; s->win_n = 0; }
static inline int dm_boost_decide(dm_boost_t *s, int64_t t, int64_t work, int64_t iv, int held, int64_t target, int frames, int64_t min_ns) {
    if (held || target <= 0) { dm_boost_clear(s); return DM_BOOST_NONE; }
    s->over = work > target ? s->over + 1 : 0;
    int slow = 0;
    if (iv > 0) { s->win_t += iv; s->win_n++; }
    if (s->win_t >= 1000000000LL) { slow = s->win_n && s->win_t / (int64_t)s->win_n > target + target * 15 / 100; s->win_t = 0; s->win_n = 0; }
    if (s->over < frames && !slow) return DM_BOOST_NONE;
    if (s->last_up && t - s->last_up < min_ns) return DM_BOOST_LIMITED;
    s->last_up = t;
    return slow ? DM_BOOST_SLOW : DM_BOOST_WORK;
}

// ---- session members: who is in the hint session after a scan
// cand: the process's threads with their CPU ticks since the previous scan (u), seen on that scan too (was), allowed (ok) and the game's
// role (1 = its main thread, 2 = a job worker, 0 = other). members / low_in: the session now and each member's run of idle scans.
// Order: the presenting thread (self); the game's main thread (from the first scan; out after two scans below `low`); the other members
// (out after two scans below `low`); then free places: the game's job workers with >= `low` first, then the busiest others with >= `need`.
// Returns the count written to pick / plow (at most max, max <= 8).
typedef struct { int32_t tid; uint64_t u; int was, ok, role; } dm_hint_cand_t;
static inline int dm_hint_pick(const dm_hint_cand_t *c, int nc, int32_t self, int32_t main_tid, const int32_t *members, const uint8_t *low_in, int nmem,
                               uint64_t need, uint64_t low, int first, int max, int32_t *pick, uint8_t *plow) {
    int n = 0;
    if (max < 1) max = 1;
    if (max > 8) max = 8;
    pick[n] = self; plow[n++] = 0;
    #define DM_TAKEN(t) ({ int _k = 0; for (int _j = 0; _j < n; _j++) _k |= pick[_j] == (t); _k; })
    #define DM_FIND(t) ({ int _f = -1; for (int _i = 0; _i < nc; _i++) if (c[_i].tid == (t)) { _f = _i; break; } _f; })
    if (main_tid > 0 && n < max && !DM_TAKEN(main_tid)) {
        int k = DM_FIND(main_tid);
        if (k >= 0) {
            int was_low = 0; for (int m = 0; m < nmem; m++) if (members[m] == main_tid) was_low = low_in[m];
            int l = !first && c[k].u < low ? was_low + 1 : 0;
            if (l < 2) { pick[n] = main_tid; plow[n++] = (uint8_t)l; }
        }
    }
    if (!first) {
        for (int m = 0; m < nmem && n < max; m++) {   // members stay unless idle on two scans in a row
            int tid = members[m], k; if (DM_TAKEN(tid) || tid == main_tid) continue;
            if ((k = DM_FIND(tid)) < 0 || !c[k].ok) continue;   // the thread ended
            int l = c[k].u < low ? low_in[m] + 1 : 0;
            if (l >= 2) continue;
            pick[n] = tid; plow[n++] = (uint8_t)l;
        }
        while (n < max) {   // the game's job workers first (>= low), then the busiest others (>= need)
            int best = -1, brole = 0; uint64_t bu = 0;
            for (int i = 0; i < nc; i++) {
                if (!c[i].ok || !c[i].was || DM_TAKEN(c[i].tid)) continue;
                int game = c[i].role != 0;
                if (c[i].u == 0 || c[i].u < (game ? low : need)) continue;
                if (best < 0 || game > brole || (game == brole && c[i].u > bu)) { bu = c[i].u; best = i; brole = game; }
            }
            if (best < 0) break;
            pick[n] = c[best].tid; plow[n++] = 0;
        }
    }
    #undef DM_TAKEN
    #undef DM_FIND
    return n;
}
