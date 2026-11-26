// [pace-vote] The rules of the display-rate vote and of the ADPF target as plain functions (no Android, no I/O), so a host test can drive
// them (port-common/modules/pace-vote/test/t_pace_vote.c). perf_android.c owns the window, the hint session and the clocks; PaceVote.java
// owns the window's display mode request.
// KB PNH-C03 (a 30 fps cap asks the panel for 60 Hz) and PNH-C05 (the ADPF target is the game's cap period, not the present period capped
// at 33.3 ms). Rules adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: vka_present.c frame_rate (the window rate held at
// its highest for a few seconds), android/.../display/RunningDisplayPolicy.java mode (a 60 Hz mode of the same resolution for a 30 cap),
// vka_dev.c vka_floor_period_ns + vka_cap_period_clamp (the target is the game's shortest period, never shorter than its cap's).
#pragma once
#include <math.h>
#include <stdint.h>

// ---- the panel rate for a frame cap (DM_VOTE_HZ=cap)
// The lowest display rate that is a whole multiple of the cap (within 1 Hz: 59.94 counts for 30 / 60) and at least max(cap, 60) Hz, and
// not above the marker vote (gfx-frame-rate, default 120: the old fixed vote is the ceiling = "the maximum"). 120 Hz panel with 60 / 120:
// cap 30 -> 60, cap 60 -> 60, cap 40 -> 120, cap 90 -> 120 (no 90 mode), cap 120 -> 120; a 60 / 90 / 120 panel: cap 90 -> 90.
// modes = the refresh rates of the display's modes at the current resolution (any order; n <= 0: not known yet, taken as {60, marker}).
// cap <= 0 (uncapped) or no such rate -> the marker. marker <= 0 (marker file says 0 = no vote) -> 0.
static inline float pv_vote_hz(double cap_hz, const float *modes, int n, float marker) {
    if (!(marker > 0)) return 0;
    if (!(cap_hz > 0)) return marker;
    const float guess[2] = {60.0f, marker};
    if (!modes || n <= 0) { modes = guess; n = 2; }
    const double low = cap_hz > 60 ? cap_hz : 60;
    float best = 0;
    for (int i = 0; i < n; i++) {
        const double r = modes[i];
        if (!(r > 0) || r + 1.0 < low || r > marker + 1.0) continue;
        const double k = floor(r / cap_hz + 0.5);
        if (k < 1 || fabs(r - k * cap_hz) > 1.0) continue;
        if (best == 0 || r < best) best = (float)r;
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
