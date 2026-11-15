package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.Application;
import android.app.GameManager;
import android.app.GameState;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

// [game-state] Tells the system what the game is doing (GameManager.setGameState, Android 13 / API 33): loading while the game's loading
// screen is up, gameplay (interruptible; uninterruptible once local play was opened) in a game, none in the front end. The system's game
// service passes it to the power HAL (a loading boost where the vendor implements one). The native side decides (perf_android.c
// nativeGameState: diag.c's loading screen, the touch layer's scene, hle_ldn.c) and logs every change to boot.log ("[perf] game state: ...");
// this class only asks it twice a second while the activity is in front and passes a change on. DM_GAME_STATE=0 (env file): never sent.
// Port-neutral (port-common/modules/adpf-boost): DmActivity calls install() once the game has started.
@android.annotation.TargetApi(33)   // every API 33 call is behind install()'s SDK check
final class GameStateHint implements Application.ActivityLifecycleCallbacks, Runnable {
    static native int nativeGameState();            // -2 off, -1 unknown, else loading (bit 0) | GameState mode << 4
    static native void nativeGameStateNote(String s);  // a line in boot.log

    private static GameStateHint instance;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final GameManager manager;
    private boolean running, off;
    private int last = -1;

    private GameStateHint(Activity a) { manager = a.getSystemService(GameManager.class); }

    // Once, on the UI thread, after the native library started the game; resumed = the activity is in front now.
    static void install(Activity a, boolean resumed) {
        if (instance != null) return;
        if (Build.VERSION.SDK_INT < 33) { note("Android " + Build.VERSION.SDK_INT + " < 13: no GameManager.setGameState"); return; }
        GameStateHint h = new GameStateHint(a);
        if (h.manager == null) { note("no GameManager service"); return; }
        instance = h;
        a.registerActivityLifecycleCallbacks(h);
        note("polling the native state twice a second while in front");
        if (resumed) h.start();
    }
    private static void note(String s) { try { nativeGameStateNote(s); } catch (UnsatisfiedLinkError e) { android.util.Log.i("dm", "[perf] game state: " + s); } }

    private void start() { if (running || off) return; running = true; last = -1; handler.post(this); }   // after a pause the state is sent again
    private void stop() { running = false; handler.removeCallbacks(this); }

    @Override public void run() {
        if (!running) return;
        int code;
        try { code = nativeGameState(); } catch (UnsatisfiedLinkError e) { note("nativeGameState missing: off"); off = true; stop(); return; }
        if (code == -2) { off = true; stop(); return; }
        if (code >= 0 && code != last && Build.VERSION.SDK_INT >= 33) {
            try { manager.setGameState(new GameState((code & 1) != 0, code >> 4)); last = code; }
            catch (RuntimeException e) { note("setGameState failed: " + e); off = true; stop(); return; }
        }
        handler.postDelayed(this, 500);
    }

    @Override public void onActivityResumed(Activity a) { start(); }
    @Override public void onActivityPaused(Activity a) { stop(); }
    @Override public void onActivityCreated(Activity a, Bundle b) {}
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
    @Override public void onActivityDestroyed(Activity a) { stop(); if (instance == this) { a.unregisterActivityLifecycleCallbacks(this); instance = null; } }
}
