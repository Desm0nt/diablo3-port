// [pace-vote] The rules of the display-rate vote and of the ADPF target as plain functions (no Android, no I/O), so a host test can drive
// them (port-common/modules/pace-vote/test/t_pace_vote.c). perf_android.c owns the window, the hint session and the clocks; PaceVote.java
// owns the window's display mode request.
// KB PNH-C03 (a 30 fps cap asks the panel for 60 Hz; 20 / 15 too, every other cap keeps the maximum) and PNH-C05 (the ADPF target is the game's cap period, not the present period capped
// at 33.3 ms). Rules adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: vka_present.c frame_rate (the window rate held at
// its highest for a few seconds), android/.../display/RunningDisplayPolicy.java mode (a 60 Hz mode of the same resolution for a 30 cap),
// vka_dev.c vka_floor_period_ns + vka_cap_period_clamp (the target is the game's shortest period, never shorter than its cap's).
#pragma once
#include <math.h>
#include <stdint.h>

// ---- the panel rate for a frame cap (DM_VOTE_HZ=cap)
// Owner's rule (26.11): the vote leaves the maximum (the marker, gfx-frame-rate, default 120) only for a cap that is a whole fraction of
// 60 at or below 30 fps (30, 20, 15, 12, 10; 29.97 counts): those frames are held for 2, 3, 4 ... refreshes of a 60 Hz panel and for 4, 6,
// 8 ... of a 120 Hz one, and the long holds on the fast panel are what peanuts measured as uneven (30 fps on 120 Hz came as 25.3 / 42.2 ms
// pairs; a 60 Hz mode of the same resolution gave 630 of 630 intervals at 33.4 ms). Such a cap votes 60 Hz (the display's mode within 1 Hz
// of 60, 59.94 included; never below 60). Every other cap keeps the maximum: 60 and above (no hold longer than 2 refreshes at 120 Hz;
// dropping a 60 cap to a 60 Hz panel was tried and turned down: the owner keeps 120 there), 40 / 45 / 24 / 25 (60 is not a multiple of
// them: a 60 Hz panel would judder them, 120 shows 40 / 24 evenly), uncapped.
// modes = the refresh rates of the display's modes at the current resolution (any order; n <= 0: not known yet = a 60 Hz mode assumed).
// No 60 Hz mode, or the marker below 60 -> the marker. marker <= 0 (marker file says 0 = no vote) -> 0.
static inline float pv_vote_hz(double cap_hz, const float *modes, int n, float marker) {
    if (!(marker > 0)) return 0;
    if (!(cap_hz >= 9.5) || cap_hz > 30.5) return marker;
    const double k = floor(60.0 / cap_hz + 0.5);
    if (k < 2 || fabs(60.0 - k * cap_hz) > 1.0) return marker;   // not a whole fraction of 60
    if (marker + 1.0f < 60.0f) return marker;
    if (!modes || n <= 0) return 60.0f;
    float best = 0;
    for (int i = 0; i < n; i++) {
        const float r = modes[i];
        if (!(r > 0) || fabsf(r - 60.0f) > 1.0f || r > marker + 1.0f) continue;
        if (best == 0 || fabsf(r - 60.0f) < fabsf(best - 60.0f)) best = r;
    }
    return best > 0 ? best : marker;
}

// ---- the thermal step (unchanged rule of perf_android.c vote_for, as a function): at SEVERE or worse the lowest multiple of the game's
// present rate that is at least 60 Hz, never above the vote it lowers.
static inline float pv_thermal_hz(float vote, double period_s) {
    if (!(vote > 0)) return vote;
    double fps = period_s > 0 ? 1 / period_s : 60; if (fps < 1) fps = 60;
    double v = fps; while (v < 59.5) v += fps;
    return v < vote ? (float)v : vote;
}

// ---- the high-water hold of the vote (peanuts vka_present.c frame_rate, DM_VK_FRAME_RATE_HOLD): up at once, down only after the lower
// rate held for hold_ns (0 = follow every change). A cap that steps down for a moment (a dip of a port without a cap hook, a thermal
// spell) does not send the panel down and back (each switch of an LTPO panel costs frames at the old rate).
typedef struct { float top; int64_t top_t; } pv_hold_t;
static inline float pv_hold(pv_hold_t *h, float hz, int64_t now, int64_t hold_ns) {
    if (hz >= h->top || hold_ns <= 0 || now - h->top_t > hold_ns) { h->top = hz; h->top_t = now; }
    return h->top;
}

// ---- the ADPF target (DM_ADPF_TARGET=game)
// The period of the frame cap the player / the game chose (60 -> 16.67 ms, 30 -> 33.33), not the period it presents at now: a game that
// slow frames pushed down to 30 still aims at 60, so the session keeps the clocks it needs to step back up (the 30 fps trap: a 33.3 ms
// target holds the clocks right under 30). Never shorter than one screen refresh (a 120 cap on a 60 Hz screen aims at 16.67).
// cap_hz <= 0 (the port has no cap hook, or the game is uncapped): the shortest present period seen in this session, at most 16.67 ms
// (peanuts' rule), so an uncapped game aims at what it reached and nothing is capped by the target.
static inline int64_t pv_adpf_target_ns(double cap_hz, double screen_hz, int64_t shortest_ns) {
    int64_t t;
    if (cap_hz >= 1 && cap_hz <= 1000) t = (int64_t)(1e9 / cap_hz + 0.5);
    else t = shortest_ns >= 4000000 && shortest_ns < 16666667 ? shortest_ns : 16666667;
    if (screen_hz >= 20 && screen_hz <= 1000) { const int64_t s = (int64_t)(1e9 / screen_hz + 0.5); if (t < s) t = s; }
    return t;
}

// ---- the window mode request (PaceVote.java mirrors this on the Java side; kept here for the host test)
// The mode to ask for: the index of the mode whose rate is the vote (within 0.5 Hz), but only when it is below the fastest rate of the
// list (at the fastest there is nothing to restrict: -1 = release, the system and the window vote decide, as before the module).
static inline int pv_mode_pick(float vote, const float *modes, int n) {
    if (!(vote > 0) || !modes || n <= 0) return -1;
    float top = 0; int at = -1;
    for (int i = 0; i < n; i++) { if (modes[i] > top) top = modes[i]; if (fabsf(modes[i] - vote) < 0.5f && (at < 0 || modes[i] < modes[at])) at = i; }
    return at >= 0 && modes[at] + 0.5f < top ? at : -1;
}
