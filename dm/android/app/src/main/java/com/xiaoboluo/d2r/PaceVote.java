package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.WindowManager;

// [pace-vote] The window's display mode follows the frame cap (port-common/modules/pace-vote, KB PNH-C03). The native side decides the rate
// (perf_android.c pace_vote_tick, pace_vote.h pv_vote_hz: the lowest display rate that is a multiple of the cap and >= 60 Hz, 30 -> 60) and
// votes it on the window (ANativeWindow_setFrameRate); this class gives the native side the display's refresh rates at the current
// resolution and asks the window for the mode of that rate (WindowManager.LayoutParams.preferredDisplayModeId) twice a second while the
// activity is in front. The content vote alone left a 120 Hz panel at 120 for a 30 fps cap, where frames came as 25 / 42 ms pairs; a
// window-local 60 Hz mode of the same resolution made them even (630 of 630 intervals at 33.4 ms). Idea adapted from peanuts-native 85eeb7a
// (igawa6) android/app/src/main/java/dev/peanuts/display/RunningDisplayMode.java + RunningDisplayPolicy.java, GPL-3.0-or-later.
// - At the display's fastest rate (cap above 60, uncapped) nothing is asked: the request goes back to what the window had before (the
//   port's own mode request, e.g. frontend-common GearDisplay.applyDisplayMode for a limit above 60, or none). A request someone else made
//   after ours is left alone.
// - Paused / in the background: released (the same rule). Global display settings are never touched.
// - DM_VOTE_HZ=max (native): nativeVote < 0, the class stops (old behaviour). Every change is a line in boot.log ("[perf] pace-vote (java)").
// Port-neutral: DmActivity calls install() once the game has started; the gear calls setCap() when its frame rate limit changes live.
final class PaceVote implements Application.ActivityLifecycleCallbacks, Runnable {
    static native float nativeVote(float[] modes);   // the rates in; the display rate the vote wants (Hz), 0 = no request, < 0 = off
    static native void nativeNote(String s);         // a line in boot.log
    static native void nativeSetCap(int hz);         // the gear's frame rate limit while the game runs (hle_nvn.c nvn_present_cap_set)

    private static PaceVote instance;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Activity activity;
    private final int base;            // the window's request before this class (the port's own; 0 = none)
    private int mine = -1;             // the mode id this class asked for last (-1 = none yet)
    private boolean running, off;
    private String lastNote = "";

    private PaceVote(Activity a) { activity = a; base = a.getWindow().getAttributes().preferredDisplayModeId; }

    // Once, on the UI thread, after the native library started the game; resumed = the activity is in front now.
    static void install(Activity a, boolean resumed) {
        if (instance != null) return;
        PaceVote p = new PaceVote(a);
        instance = p;
        a.getApplication().registerActivityLifecycleCallbacks(p);
        note("display mode follows the frame cap (window request " + (p.base != 0 ? "of the port: mode " + p.base : "none") + " before)");
        if (resumed) p.start();
    }
    // The gear's frame rate limit changed while the game runs (live): the pacer and the vote follow at the next present.
    static void setCap(int hz) {
        try { nativeSetCap(hz); } catch (UnsatisfiedLinkError e) { android.util.Log.i("dm", "[perf] pace-vote: no nativeSetCap (" + hz + " next start)"); }
    }
    private static void note(String s) { try { nativeNote(s); } catch (UnsatisfiedLinkError e) { android.util.Log.i("dm", "[perf] pace-vote: " + s); } }

    @SuppressWarnings("deprecation")
    private Display display() {
        Display d = null;
        if (android.os.Build.VERSION.SDK_INT >= 30) try { d = activity.getDisplay(); } catch (RuntimeException e) { /* not attached yet */ }
        if (d == null) d = activity.getWindowManager().getDefaultDisplay();   // Android 10 build
        return d;
    }

    private void start() { if (running || off) return; running = true; handler.post(this); }
    private void stop() { running = false; handler.removeCallbacks(this); release(); }

    @Override public void run() {
        if (!running) return;
        Display d = display();
        Display.Mode[] same = null; float[] rates = null;
        if (d != null) {
            Display.Mode cur = d.getMode(); Display.Mode[] all = d.getSupportedModes(); int n = 0;
            for (Display.Mode m : all) if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()) n++;
            same = new Display.Mode[n]; rates = new float[n]; n = 0;
            for (Display.Mode m : all) if (m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()) { same[n] = m; rates[n++] = m.getRefreshRate(); }
        }
        float want;
        try { want = nativeVote(rates); } catch (UnsatisfiedLinkError e) { note("nativeVote missing: off"); off = true; stop(); return; }
        if (want < 0) { note("DM_VOTE_HZ=max: no display mode request"); off = true; stop(); return; }
        int at = pick(want, rates);   // pace_vote.h pv_mode_pick
        apply(at >= 0 ? same[at].getModeId() : base, want, at >= 0 ? same[at] : null, rates);
        handler.postDelayed(this, 500);
    }
    // The mode whose rate is the vote (within 0.5 Hz), only when it is below the fastest rate of the list; -1 = no request.
    static int pick(float want, float[] rates) {
        if (!(want > 0) || rates == null || rates.length == 0) return -1;
        float top = 0; int at = -1;
        for (int i = 0; i < rates.length; i++) { if (rates[i] > top) top = rates[i]; if (Math.abs(rates[i] - want) < 0.5f && (at < 0 || rates[i] < rates[at])) at = i; }
        return at >= 0 && rates[at] + 0.5f < top ? at : -1;
    }
    private void release() { apply(base, 0, null, null); }
    private void apply(int id, float want, Display.Mode m, float[] rates) {
        WindowManager.LayoutParams lp = activity.getWindow().getAttributes();
        int now = lp.preferredDisplayModeId;
        if (now == id) return;
        if (id == base && mine >= 0 && now != mine) return;   // the request changed after ours: someone else's, leave it
        if (id == base && mine < 0) return;                    // nothing of ours to take back
        lp.preferredDisplayModeId = id; activity.getWindow().setAttributes(lp);
        mine = id == base ? -1 : id;
        StringBuilder r = new StringBuilder(); if (rates != null) for (float f : rates) r.append(' ').append(f);
        String s = m != null ? "window display mode " + id + " " + m.getPhysicalWidth() + "x" + m.getPhysicalHeight() + " @ " + m.getRefreshRate() + " Hz (vote " + want + " Hz; rates" + r + ")"
                             : "window display mode request back to " + (id != 0 ? "mode " + id : "none") + (want > 0 ? " (vote " + want + " Hz = the fastest rate)" : running ? "" : " (paused)");
        if (!s.equals(lastNote)) { note(s); lastNote = s; }
    }

    @Override public void onActivityResumed(Activity a) { if (a == activity) start(); }
    @Override public void onActivityPaused(Activity a) { if (a == activity) stop(); }
    @Override public void onActivityCreated(Activity a, Bundle b) {}
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
    @Override public void onActivityDestroyed(Activity a) {
        if (a != activity) return;
        stop(); if (instance == this) { a.getApplication().unregisterActivityLifecycleCallbacks(this); instance = null; }
    }
}
