package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

// [D3] The ASTC texture cache made on the phone (issue #20, owner round 3; native side dm/android/astc_convert.cpp). GPUs without BC
// textures (Mali, PowerVR, Xclipse: no KGSL, or DM_GFX_SIM_GPU in dm/env) decode the game's BC textures to 4-8x their size; the cache
// (the same Download/Diablo3Mobile/astc folder the PC setup makes) keeps them about at the BC size. Offered:
//   - at the start, while the cache is missing or incomplete: a dialog (Prepare / Later / Don't ask again) before the game starts;
//   - as a pause-menu row (FrontMenu.Host.hasTextures).
// It runs only up front (owner 13.11: never alongside the game - CPU, stutters): from the start dialog before the game starts, or from
// the pause menu with the game held. A full-screen overlay shows percent, textures done of total, the time left and Cancel (finished
// textures stay, the next run continues). The thermal status lowers the threads (half at MODERATE, one from SEVERE on) with a note.
// Finished before the game started: the game uses the cache at once; from the pause menu a toast says it is used from the next start
// (the engine opens the pack once, when the game makes its first texture).
final class D3Textures {
    static native int nativeAstcStart(String dataDir, int threads);
    static native void nativeAstcLimit(int threads);
    static native void nativeAstcCancel();
    static native void nativeAstcProgress(long[] out);   // astc_convert.h ASTC_CONV_FIELDS
    static native int nativeAstcBigCores();
    static native int nativeAstcEstimateMinutes();
    static final int IDLE = 0, PLANNING = 1, ENCODING = 2, FINISHING = 3, DONE = 4, CANCELLED = 5, FAILED = 6;
    private static final String PREFS = "d3_textures";

    interface Game { boolean running(); void hold(boolean on); void log(String line); }
    private final Activity act; private final FrameLayout frame; private final Game game; private final File data;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private LinearLayout overlay; private TextView text, note; private ProgressBar bar;
    private Runnable afterStart;   // the start screen waits for the conversion: run it to start the game (done, cancelled)
    private boolean held;          // started from the pause menu: the game is held until it ends
    private boolean running; private int threads; private long[] p = new long[12];

    D3Textures(Activity a, FrameLayout f, Game g) {
        act = a; frame = f; game = g;
        data = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), DmActivity.DATA_DIR);
    }
    private String t(String ru, String en) { return Ui.t(act, ru, en); }

    // No BC textures on this GPU: no KGSL (Mali, PowerVR, Xclipse; the app's own detection), or a simulated family in dm/env.
    static boolean wanted(Context c) {
        if (!DmActivity.kgsl()) return true;
        File env = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), DmActivity.DATA_DIR + "/dm/env");
        try {
            for (String l : new String(Files.readAllBytes(env.toPath()), StandardCharsets.UTF_8).split("\n")) {
                l = l.trim(); if (!l.startsWith("DM_GFX_SIM_GPU=")) continue;
                String v = l.substring(15).trim().toLowerCase(java.util.Locale.ROOT);
                return !v.isEmpty() && !v.equals("0") && !v.equals("adreno");
            }
        } catch (Exception e) { /* no dm/env */ }
        return false;
    }
    // 2 = complete (manifest.json says so and the pack has its size), 1 = started (part file), 0 = none
    int cacheState() {
        File dir = new File(data, "astc"), pack = new File(dir, "d3-astc.pack");
        try {
            org.json.JSONObject m = new org.json.JSONObject(new String(Files.readAllBytes(new File(dir, "manifest.json").toPath()), StandardCharsets.UTF_8));
            if (m.optBoolean("complete") && pack.length() == m.optLong("pack_bytes", -1)) return 2;
        } catch (Exception e) { /* none or unreadable */ }
        return new File(dir, "d3-astc.pack.part").exists() ? 1 : 0;
    }
    boolean running() { return running; }
    boolean holdingStart() { return afterStart != null; }

    // The pause-menu row's text: its state.
    String label() {
        if (running) { nativeAstcProgress(p); return t("Подготовка текстур: ", "Preparing textures: ") + percent() + " %"; }
        int s = cacheState();
        return s == 2 ? t("Текстуры подготовлены ✓", "Textures prepared ✓") : s == 1 ? t("Подготовить текстуры (продолжить)", "Prepare textures (continue)") : t("Подготовить текстуры", "Prepare textures");
    }
    // The start: ask (unless the cache is complete or the player said "don't ask again"); `start` starts the game.
    boolean askAtStart(Runnable start) {
        SharedPreferences sp = act.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (!wanted(act) || sp.getBoolean("never", false) || cacheState() == 2) return false;
        int min = Math.max(5, nativeAstcEstimateMinutes()); String eta = min >= 90 ? t("около " + Math.round(min / 60f) + " ч", "about " + Math.round(min / 60f) + " h") : t("около " + min + " мин", "about " + min + " min");
        game.log("textures: cache state " + cacheState() + ", asking (estimate " + min + " min, " + nativeAstcBigCores() + " big cores)");
        new AlertDialog.Builder(act).setCancelable(false)
            .setTitle(t("Подготовить текстуры?", "Prepare textures?"))
            .setMessage(t("Снижает расход памяти и немного ускоряет игру на этом GPU (у него нет текстур BC). Займёт " + eta + " (" + (cacheState() == 1 ? "продолжит начатое; " : "")
                    + "телефон будет греться, игра подождёт), можно сделать позже из меню паузы. Нужно ~6 ГБ свободного места.",
                "Lowers RAM use and speeds the game up a little on this GPU (it has no BC textures). Takes " + eta + " (" + (cacheState() == 1 ? "continues the earlier run; " : "")
                    + "the phone gets warm; the game waits), can be done later from the pause menu. Needs ~6 GB of free space."))
            .setPositiveButton(t("Подготовить", "Prepare"), (d, w) -> { afterStart = start; begin(); })
            .setNeutralButton(t("Больше не спрашивать", "Don't ask again"), (d, w) -> { sp.edit().putBoolean("never", true).apply(); start.run(); })
            .setNegativeButton(t("Позже", "Later"), (d, w) -> start.run())
            .show();
        return true;
    }
    // The pause-menu row.
    void fromMenu() {
        if (running) { showOverlay(); return; }
        if (game.running()) { game.hold(true); held = true; }
        if (cacheState() == 2) {
            new AlertDialog.Builder(act).setMessage(t("Текстуры уже подготовлены. Сделать заново?", "The textures are prepared already. Make them again?"))
                .setPositiveButton(t("Заново", "Again"), (d, w) -> { new File(data, "astc/manifest.json").delete(); new File(data, "astc/d3-astc.pack").delete(); begin(); })
                .setNegativeButton(t("Отмена", "Cancel"), (d, w) -> release()).setOnCancelListener(d -> release()).show();
            return;
        }
        begin();
    }

    private void begin() {
        threads = Math.max(1, nativeAstcBigCores());
        int r = nativeAstcStart(data.getAbsolutePath(), threads);
        game.log("textures: start " + r + " with " + threads + " threads");
        running = true; showOverlay(); ui.post(tick);
    }
    private String percent() { return p[4] > 0 ? String.valueOf(Math.min(100, p[3] * 100 / p[4])) : "0"; }
    private final Runnable tick = new Runnable() { @Override public void run() {
        if (!running) return;
        nativeAstcProgress(p);
        int st = (int) p[0];
        // thermal and the game: fewer threads (the converter's workers run at nice 10 anyway)
        int lim = threads, thermal = 0; boolean slowed = false;
        PowerManager pm = (PowerManager) act.getSystemService(Context.POWER_SERVICE);
        if (pm != null) thermal = pm.getCurrentThermalStatus();
        if (thermal >= PowerManager.THERMAL_STATUS_SEVERE) { lim = 1; slowed = true; }
        else if (thermal >= PowerManager.THERMAL_STATUS_MODERATE) { lim = Math.max(1, threads / 2); slowed = true; }
        nativeAstcLimit(lim);
        if (text != null) {
            if (st == PLANNING) text.setText(t("Подготовка текстур: чтение архивов ", "Preparing textures: reading the archives ") + p[9] + " / " + p[10]);
            else {
                long el = p[5], eta = p[3] > 0 && p[4] > p[3] ? el * (p[4] - p[3]) / p[3] / 60000 : -1;
                text.setText(t("Подготовка текстур: ", "Preparing textures: ") + percent() + " % · " + p[1] + t(" из ", " of ") + p[2]
                    + (eta >= 0 ? t(" · осталось ~", " · ~") + (eta >= 90 ? (eta + 30) / 60 + t(" ч", " h left") : Math.max(1, eta) + t(" мин", " min left")) : ""));
            }
            if (bar != null) bar.setProgress(p[4] > 0 ? (int) (p[3] * 1000 / p[4]) : 0);
            if (note != null) { note.setVisibility(slowed ? View.VISIBLE : View.GONE);
                note.setText(t("Телефон нагрелся: подготовка замедлена (" + lim + " из " + threads + " потоков)", "The phone is hot: slowed down (" + lim + " of " + threads + " threads)")); }
        }
        if (overlay != null) overlay.bringToFront();
        if (st >= DONE) { finished(st); return; }
        ui.postDelayed(this, 1000);
    } };
    private void finished(int st) {
        running = false; hideOverlay();
        boolean live = game.running(); release();
        game.log("textures: finished state " + st + " (" + p[1] + "/" + p[2] + " textures, " + p[5] / 1000 + " s, failed " + p[11] + ")");
        String msg = st == DONE ? (live ? t("Текстуры готовы: будут использоваться со следующего запуска", "Textures ready: used from the next start")
                                        : t("Текстуры готовы", "Textures ready"))
            : st == CANCELLED ? t("Подготовка текстур остановлена: продолжится с того же места", "Texture preparation stopped: it continues where it stopped")
            : t("Подготовка текстур не удалась (подробности в boot.log)", "Texture preparation failed (details in boot.log)");
        Toast.makeText(act, msg, Toast.LENGTH_LONG).show();
        Runnable s = afterStart; afterStart = null; if (s != null) s.run();
    }

    private void release() { if (held) { held = false; game.hold(false); } }
    private void showOverlay() {
        if (overlay != null) { overlay.setVisibility(View.VISIBLE); return; }
        float dp = act.getResources().getDisplayMetrics().density;
        overlay = new LinearLayout(act); overlay.setOrientation(LinearLayout.VERTICAL); overlay.setBackgroundColor(0xE0101010);
        int pad = (int) (12 * dp); overlay.setPadding(pad, pad, pad, pad); overlay.setKeepScreenOn(true); overlay.setClickable(true);
        text = new TextView(act); text.setTextColor(Color.WHITE); text.setTextSize(16); overlay.addView(text);
        bar = new ProgressBar(act, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(1000); overlay.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        note = new TextView(act); note.setTextColor(0xFFFFC060); note.setTextSize(13); note.setVisibility(View.GONE); overlay.addView(note);
        LinearLayout row = new LinearLayout(act); row.setGravity(Gravity.END);
        Button cancel = new Button(act); cancel.setText(t("Отмена", "Cancel")); cancel.setOnClickListener(v -> { nativeAstcCancel(); cancel.setEnabled(false); }); row.addView(cancel);
        overlay.addView(row);
        overlay.setGravity(Gravity.CENTER);   // the whole screen: the game is not running (start) or held (pause menu)
        frame.addView(overlay, new FrameLayout.LayoutParams(-1, -1)); overlay.bringToFront();
    }
    private void hideOverlay() { if (overlay != null) { frame.removeView(overlay); overlay = null; text = null; note = null; bar = null; } }
}
