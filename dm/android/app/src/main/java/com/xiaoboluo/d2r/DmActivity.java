// 安卓外壳：一个全屏横屏的 SurfaceView，原生层（libdm.so，dm/android/jni_android.c）跑宿主与游戏。
// 对应 iOS 的 main.m（启动链）与 front_ios.m（视图、触屏、生命周期）；本类只做系统对接，游戏逻辑与界面都在原生层。
// Android shell: a full-screen landscape SurfaceView; the native layer (libdm.so, dm/android/jni_android.c) runs the host and the game.
// Counterpart of iOS's main.m (startup chain) and front_ios.m (view, touch, lifecycle); this class only does system integration, game logic and UI are all in the native layer.
package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.text.Editable;
import android.text.InputType;
import android.text.SpanWatcher;
import android.text.Spannable;
import android.text.Spanned;
import android.text.TextWatcher;
import android.view.inputmethod.BaseInputConnection;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.content.SharedPreferences;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener, InputManager.InputDeviceListener {
    static final boolean ETERNAL = "eternal".equals(BuildConfig.DM_GAME);   // [ETERNAL] DOOM Eternal: the DOOM front end, own data folder
    static final boolean XC2 = "xc2".equals(BuildConfig.DM_GAME);   // [XC2] Xenoblade Chronicles 2: the DOOM front end, own data folder
    static final boolean D3 = "d3".equals(BuildConfig.DM_GAME);   // [D3] Diablo III: Eternal Collection: the DOOM front end, own data folder
    static final boolean DOOM = "doom".equals(BuildConfig.DM_GAME) || ETERNAL || XC2 || D3;
    static final String DATA_DIR = D3 ? "Diablo3Mobile" : XC2 ? "Xenoblade2Mobile" : ETERNAL ? "DOOM Eternal Mobile" : "DOOM Mobile";   // [DOOM] Download/<DATA_DIR>/{romfs,dm,drivers}
    private DoomPadView doomPad;
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeRefresh(float hz);
    static native void nativeFrameStats(float[] out);   // [DOOM] {fps, mean ms, max ms, GPU ms} over the last 0.5 s
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native void nativeSetDrs(boolean on);   // [XC2] dynamic resolution on / off, live (hle_xc2.c)
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // Platform services (jni_android.c): rumble needs the application Context; the motion sensors the display rotation; game controllers
    // (gamepad / joystick / d-pad sources) go to the native side, which answers whether the game took a key.
    static native void nativeContext(Object context);
    static native void nativeRotation(int rotation);
    static native boolean nativePadKey(int device, int keyCode, boolean down);
    static native void nativePadAxes(int device, float x, float y, float z, float rz, float lt, float rt, float hatX, float hatY);
    static native void nativePadRemoved(int device);
    static native String nativeDriverStatus();          // [D3] "<requested>\t<active>\t<reason>" (vk_driver.c), "" before the driver loads

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private static volatile int bootFailure;
    private AlertDialog inputDialog;

    private SurfaceView view;
    private FrameLayout gameFrame;
    private TouchOverlay touchOverlay;
    private boolean started;
    private boolean importing;
    private boolean libraryLoaded;
    private boolean resumed;
    private DisplayManager displays;
    private InputManager inputs;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        // 调试包：am start … --ez dmArtDump true 只把触屏控件底图写到 files/dm/artdump/ 就退出（dm/android/tools/art_compare.sh 用）
        // Debug builds: am start … --ez dmArtDump true only writes the touch control art to files/dm/artdump/ and exits (used by dm/android/tools/art_compare.sh)
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmArtDump", false)) {
            try { new TouchOverlay(this, null, null).dumpArt(new File(getFilesDir(), "dm/artdump")); } catch (IOException e) { android.util.Log.e("dm", "art dump: " + e); }
            finish(); return;
        }
        current = new WeakReference<>(this);
        displays = getSystemService(DisplayManager.class);
        if (displays != null) displays.registerDisplayListener(this, null);
        inputs = getSystemService(InputManager.class);
        if (inputs != null) inputs.registerInputDeviceListener(this, null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        gameFrame = new FrameLayout(this);
        gameFrame.addView(view, new FrameLayout.LayoutParams(-1, -1));
        if (XC2 || D3) { gameFrame.setBackgroundColor(0xff000000); gameFrame.addOnLayoutChangeListener((lv, l0, t0, r0, b0, ol, ot, or0, ob) -> { if (r0 - l0 != or0 - ol || b0 - t0 != ob - ot) applyAspectLayout(); }); }
        setContentView(gameFrame);
        immersive();
    }

    private void immersive() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }

    // 资源在玩家放的位置读（缺省 Download/S2X/D2R Mobile/data），要「所有文件访问」权限；没有就引导去系统设置页，回来再启动
    // Resources are read where the player put them (default Download/S2X/D2R Mobile/data), which needs "all files access"; without it, guide the player to the system settings page and start after returning
    private boolean ensureFileAccess() {
        if (Environment.isExternalStorageManager()) return true;
        TextView t = new TextView(this);
        t.setText(R.string.dmAndroidPermission);
        t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        t.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
        setContentView(t);
        return false;
    }

    // 导入包：玩家在电脑上用 dm/tools/d2rm_android.py 和自己的游戏文件做出 android 文件夹，拷到 Download/S2X/D2R Mobile/ 下；
    // 这里把它拷进内部目录 files/dm/（out、shaders、overlay、derived.tsv、pack_info.txt）。pack_info.txt 与已导入的逐字相同就不再拷。
    // 先拷到 files/dm/import.part/，齐了再逐项换上，pack_info.txt 最后放：中途被杀或空间不够，下次打开从头再导，不会用到半份。
    // 原生库加载之前做：不含游戏内容的发布包，几张表在库加载时就从 derived.tsv 读（dm/host/dm_derived_rt.h）。
    // Import pack: the player makes the android folder on a computer with dm/tools/d2rm_android.py and their own game files and copies it to
    // Download/S2X/D2R Mobile/; this copies it into the internal files/dm/ (out, shaders, overlay, derived.tsv, pack_info.txt). Nothing is copied
    // when pack_info.txt is the same as the imported one. It goes to files/dm/import.part/ first and replaces the items one by one once complete,
    // pack_info.txt last: if the app is killed or runs out of space, the next start imports again and half a copy is never used.
    // Done before the native library loads: in the release APK without game content, a few tables are read from derived.tsv at load time.
    private static final String[] IMPORT_ITEMS = {"out", "shaders", "overlay", "derived.tsv", "pack_info.txt"};

    private static String readText(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }

    private static String packProduct(String info) {
        if (info != null) for (String line : info.split("\n")) if (line.startsWith("PRODUCT=")) return line.substring(8).trim();
        return null;
    }

    // true = 可以接着启动；false = 正在导入或已显示错误
    // true = startup can continue; false = importing, or an error is shown
    private boolean ensureImported(File source) {
        if (importing) return false;
        File runtime = new File(getFilesDir(), "dm");
        String want = readText(new File(source, "pack_info.txt")), have = readText(new File(runtime, "pack_info.txt"));
        if (want != null && !want.equals(have)) {
            String product = packProduct(want);
            if (!BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
            importing = true;
            TextView t = new TextView(this);
            t.setTextSize(20); t.setPadding(64, 64, 64, 64); t.setText(getString(R.string.dmAndroidImporting, 0));
            setContentView(t);
            new Thread(() -> {
                String error = null;
                try { importPack(source, runtime, percent -> runOnUiThread(() -> t.setText(getString(R.string.dmAndroidImporting, percent)))); }
                catch (IOException e) { error = e.getMessage() == null ? e.toString() : e.getMessage(); }
                String failure = error;
                runOnUiThread(() -> {
                    importing = false;
                    if (failure != null) showStartError(getString(R.string.dmAndroidImportFailed, failure));
                    else startHost();
                });
            }, "dm-import").start();
            return false;
        }
        // 已导入的是给别的版本做的（App 升级了、导入包没重做）：同样要重做导入包。开发时直接推进来的没有 pack_info.txt，不核
        // The imported pack was made for another version (the app was updated, the pack was not remade): it has to be remade too.
        // Files pushed directly during development have no pack_info.txt and are not checked
        String product = packProduct(have);
        if (have != null && !BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
        return true;
    }

    private interface Progress { void percent(int p); }

    private static void importPack(File source, File runtime, Progress progress) throws IOException {
        for (String item : IMPORT_ITEMS) if (!new File(source, item).exists()) throw new IOException("missing " + item);
        File part = new File(runtime, "import.part");
        deleteTree(part);
        long sum = 0;
        for (String item : IMPORT_ITEMS) sum += treeSize(new File(source, item));
        final long total = sum;
        long[] done = {0}; int[] shown = {-1};
        byte[] buffer = new byte[1 << 20];
        for (String item : IMPORT_ITEMS) copyTree(new File(source, item), new File(part, item), buffer, bytes -> {
            done[0] += bytes;
            int p = (int) (done[0] * 100 / Math.max(1, total));
            if (p != shown[0]) { shown[0] = p; progress.percent(p); }
        });
        for (String item : IMPORT_ITEMS) {
            File from = new File(part, item), to = new File(runtime, item);
            deleteTree(to);
            if (!from.renameTo(to)) throw new IOException("rename " + item);
        }
        deleteTree(part);
    }

    private interface Copied { void bytes(long n); }

    private static long treeSize(File f) {
        if (!f.isDirectory()) return f.length();
        long n = 0; File[] list = f.listFiles();
        if (list != null) for (File c : list) n += treeSize(c);
        return n;
    }

    private static void copyTree(File from, File to, byte[] buffer, Copied copied) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to.getName());
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from.getName());
            for (File c : list) copyTree(c, new File(to, c.getName()), buffer, copied);
            return;
        }
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent.getName());
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) { out.write(buffer, 0, n); copied.bytes(n); }
        }
    }

    private static void deleteTree(File f) throws IOException {
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) for (File c : list) deleteTree(c);
        if (f.exists() && !f.delete()) throw new IOException("delete " + f.getName());
    }


    // [DOOM] Graphics options (gear button on the on-screen pad). Kept in SharedPreferences and handed to the native host as
    // environment variables before the library loads (hle_args.c turns them into the game's +set arguments, vulkan_present.cpp
    // reads the sharpening), so a change applies at the next start of the game.
    static final float[] RES_FRACTIONS = {1.0f, 0.85f, 0.75f, 0.67f};
    static final String[] RES_LABELS = {"100% (720p)", "85% (612p)", "75% (540p)", "67% (480p)"};
    static final String[] SHARPEN_STOPS = {"", "1.0", "0.5", "0.2"};
    static final String[] SHARPEN_LABELS = {"Выкл", "Слабая", "Средняя", "Сильная"};
    static final float[] AA_STRENGTHS = {0f, 0.5f, 1.0f, 1.5f, 2.0f};
    static final String[] AA_LABELS = {"Как на Switch", "0.5", "1", "1.5", "2"};
    static final String[] LANG_CODES = XC2 ? new String[] {"", "en-US", "ja", "fr", "de", "es", "it"} : new String[] {"", "ru", "en-US"};
    static final String[] LANG_LABELS = XC2 ? new String[] {"Как в системе", "English", "日本語", "Français", "Deutsch", "Español", "Italiano"} : new String[] {"Как в системе", "Русский", "English"};
    // [XC2] Picture shape on a screen wider than 16:9: 0 = the game's 16:9 with black side bars, 1 = 16:9 stretched to the screen,
    // 2 = widened (DM_XC2_WIDE: the 3D sees more at the sides, the 16:9 HUD centred; next launch)
    static final String[] ASPECT_LABELS = {"Нативное 16:9 (чёрные полосы по бокам)", "Нативное, растянуть на весь экран", "Расширенное (шире обзор, со следующего запуска)"};
    static final int ASPECT_DEFAULT = 2;
    private int startedAspect = -1;   // the aspect mode the running game was started with (widened needs a restart)
    static final float[] LOOK_GAINS = {0.6f, 0.8f, 1.0f, 1.3f, 1.7f};
    static final String[] LOOK_LABELS = {"0.6", "0.8", "1.0", "1.3", "1.7"};
    static final float[] DEADZONES = {0f, 0.08f, 0.15f, 0.25f};
    static final String[] DEADZONE_LABELS = {"Нет", "Малая", "Средняя", "Большая"};
    static int nearest(float[] v, float x) { int b = 0; for (int i = 1; i < v.length; i++) if (Math.abs(v[i] - x) < Math.abs(v[b] - x)) b = i; return b; }

    private SharedPreferences doomPrefs() { return getSharedPreferences("doom_graphics", MODE_PRIVATE); }

    private void applyDoomSettings() {
        SharedPreferences p = doomPrefs();
        try {
            if (XC2) {   // [XC2] the game's own DRS (live toggle: nativeSetDrs) and the picture shape; the DOOM cvars do not apply
                Os.setenv("DM_XC2_DRS", p.getBoolean("xc2_drs", true) ? "1" : "0", true);
                startedAspect = p.getInt("aspect", ASPECT_DEFAULT);
                if (startedAspect == 2) Os.setenv("DM_XC2_WIDE", "1", true); else Os.unsetenv("DM_XC2_WIDE");
            } else if (D3) {   // [D3] the DOOM cvars (rs_enable / rs_forceFraction*) do not apply
                Os.unsetenv("DM_DOOM_DRS"); Os.unsetenv("DM_DOOM_RES");
                // Frame rate limit (hle_nvn.c present_cap_hz): the largest integer fraction of the refresh rate not above it
                Os.setenv("DM_PRESENT_MAX_HZ", String.valueOf(d3FpsLimit()), true);
                // Resolution = the Switch operation mode the game is told (hle_oe.c GetOperationMode / GetPerformanceMode): docked
                // renders 1600x900, handheld the game's own 720p preset. dm/env still wins (it is applied after this).
                Os.setenv("DM_OPMODE", p.getBoolean("handheld", false) ? "0" : "1", true);
            } else {
            if (p.getBoolean("drs", false)) Os.setenv("DM_DOOM_DRS", "1", true); else Os.unsetenv("DM_DOOM_DRS");
            int res = p.getInt("res", 0);
            if (res > 0 && res < RES_FRACTIONS.length) Os.setenv("DM_DOOM_RES", String.valueOf(RES_FRACTIONS[res]), true); else Os.unsetenv("DM_DOOM_RES");
            }
            int sh = p.getInt("sharpen", 0);
            if (sh > 0 && sh < SHARPEN_STOPS.length) Os.setenv("DM_GFX_SHARPEN", SHARPEN_STOPS[sh], true); else Os.unsetenv("DM_GFX_SHARPEN");
            // Vulkan driver (vk_driver.c): adrenotools packages under <data>/drivers/<name>/ (meta.json + the library) are copied to
            // private storage (a driver must be loaded from there); the package chosen in the dialog sets DM_VK_DRIVER, which dm/env
            // can also set (DM_VK_DRIVER=<name>). The hooks adrenotools loads live in nativeLibraryDir.
            Os.setenv("DM_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            File drivers = new File(getFilesDir(), "drivers");
            Os.setenv("DM_DRIVERS_DIR", drivers.getAbsolutePath(), true);
            extractBundledDrivers(drivers);   // the APK's default driver (Turnip 27, assets/drivers/<name>/; as DOOM 2016)
            importDrivers(new File(Environment.getExternalStorageDirectory(), "Download/" + DATA_DIR + "/drivers"), drivers);
            // Game language (DOOM takes it from the system like on the Switch: hle_oe.c GetDesiredLanguage reads DM_LANG; jni sets
            // DM_LANG from the phone locale only when it is unset). "" = follow the phone.
            // Aim assist strength (hle_hid.c DM_AA_STRENGTH: the game's single-player assist scaled; 0 = the game's weak MP-method assist)
            if (!XC2 && !D3) Os.setenv("DM_AA_STRENGTH", String.valueOf(p.getFloat("aa", 1.0f)), true);
            // Rumble (haptics_android.c): off by default since 28.10 (owner: it only drains the battery); DM_RUMBLE=0 hides the vibration devices.
            Os.setenv("DM_RUMBLE", p.getBoolean("rumble", false) ? "1" : "0", true);
            String lang = p.getString("lang", "");
            if (!lang.isEmpty()) Os.setenv("DM_LANG", lang, true);
            String drv = p.getString("driver", defaultDriver());
            if (D3) drv = driverGuardCheck(p, drv);
            if (!drv.isEmpty()) Os.setenv("DM_VK_DRIVER", drv, true); else Os.unsetenv("DM_VK_DRIVER");
        } catch (ErrnoException e) { android.util.Log.e("dm", "doom settings: " + e); }
    }

    // [DOOM / XC2] Default Vulkan driver: the bundled Turnip package when the APK has it (vk_driver.c falls back to the system driver
    // when it does not load or sees no Vulkan 1.1 GPU, e.g. on non-Adreno phones); "" = the system driver.
    static final String DEFAULT_DRIVER = "turnip27";
    private String defaultDriver() { return new File(new File(getFilesDir(), "drivers"), DEFAULT_DRIVER).isDirectory() ? DEFAULT_DRIVER : ""; }
    // Copy assets/drivers/<name>/* to <dst>/<name>/ when missing or of another size.
    private void extractBundledDrivers(File dst) {
        try {
            String[] names = getAssets().list("drivers"); if (names == null) return;
            for (String n : names) {
                String[] files = getAssets().list("drivers/" + n); if (files == null || files.length == 0) continue;
                File out = new File(dst, n); out.mkdirs();
                for (String f : files) {
                    File o = new File(out, f);
                    try (java.io.InputStream in = getAssets().open("drivers/" + n + "/" + f)) {
                        long size = -1; try (android.content.res.AssetFileDescriptor fd = getAssets().openFd("drivers/" + n + "/" + f)) { size = fd.getLength(); } catch (java.io.IOException e) { /* compressed asset: size unknown */ }
                        if (o.isFile() && (size < 0 ? o.length() > 0 : o.length() == size)) continue;
                        try (java.io.OutputStream os = new java.io.FileOutputStream(o)) { byte[] buf = new byte[1 << 16]; int k; while ((k = in.read(buf)) > 0) os.write(buf, 0, k); }
                        android.util.Log.i("dm", "bundled driver " + n + ": " + f + " extracted");
                    }
                }
            }
        } catch (java.io.IOException e) { android.util.Log.e("dm", "bundled drivers: " + e); }
    }

    // Driver packages in private storage (imported at launch from Download/<DATA_DIR>/drivers), sorted by folder name.
    private String[] driverPackages() {
        File[] dirs = new File(getFilesDir(), "drivers").listFiles(); java.util.ArrayList<String> out = new java.util.ArrayList<>();
        if (dirs != null) for (File d : dirs) { File[] fs = d.isDirectory() ? d.listFiles() : null; if (fs == null) continue;
            for (File f : fs) if (f.getName().endsWith(".so")) { out.add(d.getName()); break; } }
        java.util.Collections.sort(out); return out.toArray(new String[0]);
    }
    // "<meta.json name> (<folder>)", or just the folder name when there is no readable meta.json.
    private String driverLabel(String name) {
        File m = new File(new File(new File(getFilesDir(), "drivers"), name), "meta.json");
        try (java.io.FileInputStream in = new java.io.FileInputStream(m)) {
            byte[] b = new byte[(int) Math.min(m.length(), 65536)]; int n = in.read(b);
            String t = new org.json.JSONObject(new String(b, 0, Math.max(n, 0), "UTF-8")).optString("name", "");
            if (!t.isEmpty()) return t + " (" + name + ")";
        } catch (Exception e) { /* no or unreadable meta.json */ }
        return name;
    }

    // Copy every driver package <src>/<name>/{meta.json, library} into <dst>/<name>/ when missing or of another size.
    private static void importDrivers(File src, File dst) {
        File[] dirs = src.listFiles(); if (dirs == null) return;
        for (File d : dirs) {
            if (D3 && d.isFile() && d.getName().endsWith(".zip")) {   // [D3] a package zip dropped as is: unpack to <dst>/<zip name>/ once (as DOOM 2016)
                String name = d.getName().substring(0, d.getName().length() - 4); File out = new File(dst, name);
                if (!hasDriverLib(out)) try (java.io.InputStream in = new java.io.FileInputStream(d)) { unzipDriver(in, out); android.util.Log.i("dm", "driver package " + name + ": unpacked from zip"); }
                catch (java.io.IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
                continue;
            }
            File[] files = d.isDirectory() ? d.listFiles() : null; if (files == null) continue;
            File out = new File(dst, d.getName()); out.mkdirs();
            for (File f : files) {
                File o = new File(out, f.getName());
                if (o.length() == f.length()) continue;
                try (java.io.InputStream in = new java.io.FileInputStream(f); java.io.OutputStream os = new java.io.FileOutputStream(o)) {
                    byte[] buf = new byte[1 << 16]; int n; while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    android.util.Log.i("dm", "driver package " + d.getName() + ": " + f.getName() + " copied");
                } catch (java.io.IOException e) { android.util.Log.e("dm", "driver package " + d.getName() + ": " + e); }
            }
        }
    }

    // [D3] (from the DOOM 2016 port) adrenotools package helpers: a folder with a .so; unpack a zip (meta.json + library, at the zip
    // root or in one folder) into out/.
    static boolean hasDriverLib(File d) { File[] fs = d.listFiles(); if (fs != null) for (File f : fs) if (f.getName().endsWith(".so")) return true; return false; }
    static void deleteDriverTree(File f) { File[] fs = f.listFiles(); if (fs != null) for (File c : fs) deleteDriverTree(c); f.delete(); }
    static void unzipDriver(java.io.InputStream src, File out) throws java.io.IOException {
        File tmp = new File(out.getPath() + ".tmp"); deleteDriverTree(tmp); tmp.mkdirs();
        try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(src)) {
            java.util.zip.ZipEntry e; byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = new File(e.getName()).getName(); if (!(n.endsWith(".so") || n.equals("meta.json"))) continue;
                try (java.io.OutputStream os = new java.io.FileOutputStream(new File(tmp, n))) { int k; while ((k = z.read(buf)) > 0) os.write(buf, 0, k); }
            }
        }
        if (!hasDriverLib(tmp)) { deleteDriverTree(tmp); throw new java.io.IOException("no driver library in the package"); }
        deleteDriverTree(out); if (!tmp.renameTo(out)) throw new java.io.IOException("rename failed: " + out);
    }

    // [D3] Driver crash guard (from the DOOM 2016 port): before a non-system driver is loaded its name goes to files/driver-trying
    // ("<name>\n<failed launches>"); the file is removed once the game has presented a frame or the native side reports it fell back to
    // the system driver (driverWatch). A file left over means the last launch died before its first frame with that driver: after
    // DRIVER_GUARD_STRIKES such launches in a row the system driver is chosen (saved, so the player sees it in the dialog) and a toast
    // says why. Two strikes, not one: a launch killed by hand before its first frame is not a crash.
    static final int DRIVER_GUARD_STRIKES = 2;
    private String driverNotice;   // shown once the game view is up
    private File driverGuard() { return new File(getFilesDir(), "driver-trying"); }
    private String driverGuardCheck(SharedPreferences p, String drv) {
        File guard = driverGuard(); String left = readText(guard);
        int strikes = 0;
        if (left != null) {
            String[] g = left.split("\n"); String name = g[0].trim();
            try { strikes = g.length > 1 ? Integer.parseInt(g[1].trim()) : 1; } catch (NumberFormatException e) { strikes = 1; }
            if (!name.equals(drv)) strikes = 0;   // about another driver: the player changed it since
            android.util.Log.w("dm", "driver guard: last launch with " + name + " ended before its first frame (" + strikes + " in a row)");
            if (strikes >= DRIVER_GUARD_STRIKES) {
                p.edit().putString("driver", "").apply(); drv = ""; strikes = 0;
                driverNotice = d3Ui().getString(R.string.d3_toast_driver_crashed, driverLabel(name));
            }
            guard.delete();
        }
        if (!drv.isEmpty()) {
            try (OutputStream os = new FileOutputStream(guard)) { os.write((drv + "\n" + (strikes + 1) + "\n").getBytes(StandardCharsets.UTF_8)); }
            catch (IOException e) { android.util.Log.e("dm", "driver guard: " + e); }
        }
        return drv;
    }
    // "<requested>\t<active>\t<reason>" from vk_driver.c, or null before the driver is loaded.
    private String[] driverStatus() {
        if (!libraryLoaded) return null;
        String s; try { s = nativeDriverStatus(); } catch (Throwable t) { return null; }
        String[] f = s == null ? new String[0] : s.split("\t", -1);
        return f.length >= 3 ? f : null;
    }
    // After the game starts: removes the driver crash guard once a frame was presented, and tells the player once when the chosen
    // driver did not start and the native side fell back to the system driver. Polls every second until both are settled.
    private boolean driverGuardDone, driverStatusDone;
    private final float[] guardStats = new float[4];
    private final Runnable driverWatch = new Runnable() { @Override public void run() {
        if (!started || isDestroyed()) return;
        String[] st = driverStatus();
        if (!driverStatusDone && st != null) {
            driverStatusDone = true;
            if (!st[0].isEmpty() && "system".equals(st[1])) {
                android.util.Log.w("dm", "driver " + st[0] + " did not start (" + st[2] + "): system driver");
                android.widget.Toast.makeText(DmActivity.this, d3Ui().getString(R.string.d3_toast_driver_fallback, driverLabel(st[0]), st[2]), android.widget.Toast.LENGTH_LONG).show();
                driverGuard().delete(); driverGuardDone = true;   // not a crash: the native side already chose the system driver
            }
        }
        if (!driverGuardDone) {
            try { nativeFrameStats(guardStats); } catch (Throwable t) { guardStats[0] = 0; }
            if (guardStats[0] > 0) { driverGuard().delete(); driverGuardDone = true; android.util.Log.i("dm", "driver guard: first frames presented, guard cleared"); }
        }
        if (!(driverGuardDone && driverStatusDone)) view.postDelayed(this, 1000);
    } };

    // [D3] Gear dialog of Diablo III (showD3Settings). Texts are string resources (res/values/d3_settings.xml English, values-ru
    // Russian); the dialog's own language follows the phone or the "Menu language" option (pref "ui_lang", applies at once: the
    // dialog reopens in it). Options and how they reach the game:
    //   next launch (SharedPreferences "doom_graphics" -> environment before the library loads, applyDoomSettings; dm/env still wins):
    //     game language (DM_LANG -> hle_oe.c GetDesiredLanguage), resolution = Switch mode (DM_OPMODE 1 docked 1600x900 / 0 handheld
    //     720p), frame rate limit (DM_PRESENT_MAX_HZ 30 / 60 -> hle_nvn.c), sharpening (DM_GFX_SHARPEN -> RCAS in vulkan_present.cpp),
    //     Vulkan driver (DM_VK_DRIVER -> vk_driver.c), vibration (DM_RUMBLE);
    //   immediately: picture shape (16:9 bars / stretched: the SurfaceView size, applyAspectLayout), and "doom_controls" read by
    //     DoomPadView.reloadSettings: FPS counter, stick dead-zone compensation, settings button look (normal / faded + opacity).
    static final int[] D3_FPS_LIMITS = {30, 60};   // above 60 is not offered: D3 runs its frames at 60 like on the Switch (present interval 1)
    static final String[] D3_LANG_CODES = {"", "ru", "en-US", "en-GB"};
    static final String[] D3_LANG_LABELS = {null /* d3_system */, "Русский", "English (US)", "English (UK)"};   // game languages keep their own names
    static final String[] D3_UI_LANGS = {"", "ru", "en"};
    static final String[] D3_UI_LANG_LABELS = {null /* d3_system */, "Русский", "English"};
    static final int[] D3_SHARPEN_LABELS = {R.string.d3_off, R.string.d3_low, R.string.d3_medium, R.string.d3_high};   // SHARPEN_STOPS
    static final int[] D3_DZ_LABELS = {R.string.d3_dz_none, R.string.d3_dz_small, R.string.d3_dz_medium, R.string.d3_dz_large};   // DEADZONES
    static final int[] D3_GEAR_LABELS = {R.string.d3_gear_normal, R.string.d3_gear_faded};   // DoomPadView.GEAR_*
    private int d3FpsLimit() { return doomPrefs().getInt("fps_limit", 60) == 30 ? 30 : 60; }
    // Resources in the dialog's language: the phone's, or the one picked in the dialog.
    private android.content.Context d3Ui() {
        String l = doomPrefs().getString("ui_lang", "");
        if (l.isEmpty()) return this;
        android.content.res.Configuration c = new android.content.res.Configuration(getResources().getConfiguration());
        c.setLocale(Locale.forLanguageTag(l));
        return createConfigurationContext(c);
    }
    private static int indexOf(String[] v, String x) { for (int i = 0; i < v.length; i++) if (v[i].equals(x)) return i; return 0; }
    private static String when(android.content.Context ui, int label, boolean now) {
        return ui.getString(R.string.d3_label_fmt, ui.getString(label), ui.getString(now ? R.string.d3_when_now : R.string.d3_when_next));
    }
    private TextView heading(LinearLayout box, String text) { TextView t = new TextView(this); t.setText(text); t.setPadding(0, 24, 0, 0); box.addView(t); return t; }
    private TextView note(LinearLayout box, String text) { TextView t = new TextView(this); t.setText(text); t.setAlpha(0.7f); box.addView(t); return t; }
    private RadioGroup radios(LinearLayout box, String[] labels, int idBase, int checked, boolean horizontal) {
        RadioGroup g = new RadioGroup(this); g.setOrientation(horizontal ? RadioGroup.HORIZONTAL : RadioGroup.VERTICAL);
        for (int i = 0; i < labels.length; i++) { RadioButton r = new RadioButton(this); r.setText(labels[i]); r.setId(idBase + i); g.addView(r); }
        g.check(idBase + Math.min(Math.max(0, checked), labels.length - 1)); box.addView(g); return g;
    }
    private static String[] texts(android.content.Context ui, int[] ids) { String[] s = new String[ids.length]; for (int i = 0; i < ids.length; i++) s[i] = ui.getString(ids[i]); return s; }
    private static int picked(RadioGroup g, int idBase, int n) { return Math.min(Math.max(0, g.getCheckedRadioButtonId() - idBase), n - 1); }

    void showD3Settings() {
        runOnUiThread(() -> {
            android.content.Context ui = d3Ui();
            SharedPreferences p = doomPrefs(), cp = getSharedPreferences("doom_controls", MODE_PRIVATE);
            LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
            heading(box, when(ui, R.string.d3_ui_lang, true));
            String[] uiLabels = D3_UI_LANG_LABELS.clone(); uiLabels[0] = ui.getString(R.string.d3_system);
            final String uiWas = p.getString("ui_lang", "");
            RadioGroup uiG = radios(box, uiLabels, 9100, indexOf(D3_UI_LANGS, uiWas), true);
            heading(box, when(ui, R.string.d3_game_lang, false));
            String[] langLabels = D3_LANG_LABELS.clone(); langLabels[0] = ui.getString(R.string.d3_system);
            RadioGroup langG = radios(box, langLabels, 5000, indexOf(D3_LANG_CODES, p.getString("lang", "")), true);
            note(box, ui.getString(R.string.d3_game_lang_note));
            heading(box, when(ui, R.string.d3_mode, false));
            RadioGroup modeG = radios(box, new String[] {ui.getString(R.string.d3_mode_docked), ui.getString(R.string.d3_mode_handheld)}, 1000, p.getBoolean("handheld", false) ? 1 : 0, false);
            note(box, ui.getString(R.string.d3_mode_note));
            heading(box, when(ui, R.string.d3_aspect, true));
            RadioGroup aspG = radios(box, new String[] {ui.getString(R.string.d3_aspect_bars), ui.getString(R.string.d3_aspect_stretch)}, 8000, p.getInt("aspect", 0), false);
            heading(box, when(ui, R.string.d3_sharpen, false));
            RadioGroup sh = radios(box, texts(ui, D3_SHARPEN_LABELS), 2000, p.getInt("sharpen", 0), true);
            heading(box, when(ui, R.string.d3_fps_limit, false));
            String[] limLabels = new String[D3_FPS_LIMITS.length]; int limAt = 0;
            for (int i = 0; i < limLabels.length; i++) { limLabels[i] = String.valueOf(D3_FPS_LIMITS[i]); if (D3_FPS_LIMITS[i] == d3FpsLimit()) limAt = i; }
            RadioGroup fpsLim = radios(box, limLabels, 8500, limAt, true);
            note(box, ui.getString(R.string.d3_fps_note));
            // Vulkan driver: the system one, or any adrenotools package under files/drivers/<name>/ (bundled Turnip 27, or imported from
            // Download/Diablo3Mobile/drivers), labelled with the name from its meta.json; below it, what the running game actually uses.
            heading(box, when(ui, R.string.d3_driver, false));
            final String[] drvNames = driverPackages();
            String[] drvLabels = new String[drvNames.length + 1]; drvLabels[0] = ui.getString(R.string.d3_driver_system);
            for (int i = 0; i < drvNames.length; i++) drvLabels[i + 1] = driverLabel(drvNames[i]);
            int drvAt = 0; { String cur = p.getString("driver", defaultDriver()); for (int i = 0; i < drvNames.length; i++) if (drvNames[i].equals(cur)) drvAt = i + 1; }
            RadioGroup drvG = radios(box, drvLabels, 7000, drvAt, false);
            String[] st = driverStatus();
            if (st != null) {
                boolean fell = !st[0].isEmpty() && "system".equals(st[1]);
                note(box, fell ? ui.getString(R.string.d3_driver_active_fallback, driverLabel(st[0]))
                               : ui.getString(R.string.d3_driver_active, st[0].isEmpty() || "system".equals(st[1]) ? ui.getString(R.string.d3_driver_system_short) : driverLabel(st[0])));
            }
            note(box, ui.getString(R.string.d3_driver_note));
            CheckBox rumble = new CheckBox(this); rumble.setText(when(ui, R.string.d3_rumble, false)); rumble.setChecked(p.getBoolean("rumble", false)); box.addView(rumble);
            CheckBox fps = new CheckBox(this); fps.setText(when(ui, R.string.d3_fps_counter, true)); fps.setChecked(cp.getBoolean("fps", true)); box.addView(fps);
            heading(box, when(ui, R.string.d3_deadzone, true));
            RadioGroup dz = radios(box, texts(ui, D3_DZ_LABELS), 4000, nearest(DEADZONES, cp.getFloat("move_dz", 0f)), true);
            // Settings button (DoomPadView.GEAR_*): normal / faded, with the faded opacity slider below (saved and shown on the pad while
            // sliding: the pad is visible behind the dialog).
            heading(box, when(ui, R.string.d3_gear, true));
            RadioGroup gearG = radios(box, texts(ui, D3_GEAR_LABELS), 9000, cp.getInt("gear_mode", DoomPadView.GEAR_NORMAL), true);
            note(box, ui.getString(R.string.d3_gear_note));
            final int alphaWas = Math.max(1, Math.min(100, cp.getInt("gear_alpha", DoomPadView.GEAR_ALPHA_DEFAULT)));
            final TextView alphaT = new TextView(this); alphaT.setText(ui.getString(R.string.d3_gear_alpha, alphaWas)); box.addView(alphaT);
            android.widget.SeekBar alphaS = new android.widget.SeekBar(this); alphaS.setMax(99); alphaS.setProgress(alphaWas - 1); box.addView(alphaS);
            alphaS.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
                public void onProgressChanged(android.widget.SeekBar sb, int v, boolean user) {
                    alphaT.setText(ui.getString(R.string.d3_gear_alpha, v + 1)); cp.edit().putInt("gear_alpha", v + 1).apply();
                    if (doomPad != null) doomPad.reloadSettings();
                }
                public void onStartTrackingTouch(android.widget.SeekBar sb) {}
                public void onStopTrackingTouch(android.widget.SeekBar sb) {}
            });
            android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
            new AlertDialog.Builder(this).setTitle(ui.getString(R.string.d3_title)).setView(scroll)
                .setPositiveButton(ui.getString(R.string.d3_save), (d, w) -> {
                    String uiNow = D3_UI_LANGS[picked(uiG, 9100, D3_UI_LANGS.length)];
                    int drvPick = picked(drvG, 7000, drvNames.length + 1);
                    p.edit().putString("ui_lang", uiNow).putString("lang", D3_LANG_CODES[picked(langG, 5000, D3_LANG_CODES.length)])
                        .putBoolean("handheld", picked(modeG, 1000, 2) == 1).putInt("aspect", picked(aspG, 8000, 2))
                        .putInt("sharpen", picked(sh, 2000, SHARPEN_STOPS.length)).putInt("fps_limit", D3_FPS_LIMITS[picked(fpsLim, 8500, D3_FPS_LIMITS.length)])
                        .putString("driver", drvPick > 0 ? drvNames[drvPick - 1] : "").putBoolean("rumble", rumble.isChecked()).apply();
                    cp.edit().putBoolean("fps", fps.isChecked()).putFloat("move_dz", DEADZONES[picked(dz, 4000, DEADZONES.length)])
                        .putInt("gear_mode", picked(gearG, 9000, D3_GEAR_LABELS.length)).apply();
                    if (doomPad != null) doomPad.reloadSettings();
                    applyAspectLayout();
                    if (!uiNow.equals(uiWas)) showD3Settings();   // the menu language applies at once: reopen in it
                })
                .setNegativeButton(ui.getString(R.string.d3_cancel), null).show();
        });
    }

    // [XC2] 16:9 with black side bars = the SurfaceView itself sized 16:9 and centred (the compositor still scales the game's
    // 1280x720 buffer, gfx-present-native); stretched and widened = the whole screen. A game started widened renders W x 720 at
    // the screen's aspect, so it fills the screen whatever the saved choice until the next launch.
    void applyAspectLayout() {
        if (!(XC2 || D3) || view == null || gameFrame == null) return;   // [D3] 0 = 16:9 with bars (default), 1 = stretched; the game's 1600x900 / 720p is 16:9
        runOnUiThread(() -> {
            int fw = gameFrame.getWidth(), fh = gameFrame.getHeight();
            int mode = XC2 && startedAspect == 2 ? 2 : doomPrefs().getInt("aspect", XC2 ? ASPECT_DEFAULT : 0);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-1, -1, android.view.Gravity.CENTER);
            if (mode == 0 && fw > 0 && fh > 0 && fw * 9 > fh * 16) lp.width = fh * 16 / 9;
            else if (mode == 0 && fw > 0 && fh > 0 && fw * 9 < fh * 16) lp.height = fw * 9 / 16;
            FrameLayout.LayoutParams cur = (FrameLayout.LayoutParams) view.getLayoutParams();
            if (cur == null || cur.width != lp.width || cur.height != lp.height || cur.gravity != lp.gravity) view.setLayoutParams(lp);
        });
    }

    void showDoomSettings() {
        if (D3) { showD3Settings(); return; }   // [D3] its own dialog (string resources, D3 options)
        runOnUiThread(() -> {
            SharedPreferences p = doomPrefs();
            LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); box.setPadding(48, 24, 48, 0);
            CheckBox drs = new CheckBox(this);
            if (XC2) { drs.setText("Динамическое разрешение (как на Switch; выкл = всегда полное, применяется сразу)"); drs.setChecked(p.getBoolean("xc2_drs", true)); }
            else { drs.setText("Динамическое разрешение DOOM (снижает 3D при нагрузке)"); drs.setChecked(p.getBoolean("drs", false)); }
            if (!D3) box.addView(drs);   // [D3] the DOOM cvars (DRS, 3D resolution, aim assist) do not apply
            RadioGroup aspG = new RadioGroup(this);
            if (XC2) {
                TextView aspL = new TextView(this); aspL.setText("Формат картинки:"); aspL.setPadding(0, 24, 0, 0); box.addView(aspL);
                for (int i = 0; i < ASPECT_LABELS.length; i++) { RadioButton r = new RadioButton(this); r.setText(ASPECT_LABELS[i]); r.setId(8000 + i); aspG.addView(r); }
                aspG.check(8000 + p.getInt("aspect", ASPECT_DEFAULT)); box.addView(aspG);
            }
            // Vulkan driver: the system one, or any adrenotools package found under Download/<DATA_DIR>/drivers/<name>/ (one folder
            // per package, e.g. Turnip builds for different Adreno generations), labelled with the name from its meta.json.
            TextView drvL = new TextView(this); drvL.setText("Драйвер Vulkan (со следующего запуска):"); drvL.setPadding(0, 24, 0, 0); box.addView(drvL);
            RadioGroup drvG = new RadioGroup(this); drvG.setOrientation(RadioGroup.VERTICAL);
            final String[] drvNames = driverPackages();
            { RadioButton r = new RadioButton(this); r.setText("Системный (драйвер производителя)"); r.setId(7000); drvG.addView(r); }
            for (int i = 0; i < drvNames.length; i++) { RadioButton r = new RadioButton(this); r.setText(driverLabel(drvNames[i])); r.setId(7001 + i); drvG.addView(r); }
            { String cur = p.getString("driver", defaultDriver()); int di = -1; for (int i = 0; i < drvNames.length; i++) if (drvNames[i].equals(cur)) di = i; drvG.check(7000 + di + 1); }
            box.addView(drvG);
            TextView langL = new TextView(this); langL.setText(D3 ? "Язык игры (со следующего запуска; без архивов этого языка в RomFS остаётся English):" : "Язык игры (тексты и озвучка, со следующего запуска):"); langL.setPadding(0, 24, 0, 0); box.addView(langL);
            RadioGroup langG = new RadioGroup(this); langG.setOrientation(XC2 ? RadioGroup.VERTICAL : RadioGroup.HORIZONTAL);
            for (int i = 0; i < LANG_CODES.length; i++) { RadioButton r = new RadioButton(this); r.setText(LANG_LABELS[i]); r.setId(5000 + i); langG.addView(r); }
            { String cur = p.getString("lang", ""); int li = 0; for (int i = 0; i < LANG_CODES.length; i++) if (LANG_CODES[i].equals(cur)) li = i; langG.check(5000 + li); }
            box.addView(langG);
            TextView aaL = new TextView(this); aaL.setText("Помощь в прицеливании (со следующего запуска):"); aaL.setPadding(0, 24, 0, 0); if (!XC2 && !D3) box.addView(aaL);
            RadioGroup aaG = new RadioGroup(this); aaG.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < AA_STRENGTHS.length; i++) { RadioButton r = new RadioButton(this); r.setText(AA_LABELS[i]); r.setId(6000 + i); aaG.addView(r); }
            aaG.check(6000 + nearest(AA_STRENGTHS, p.getFloat("aa", 1.0f))); if (!XC2 && !D3) box.addView(aaG);
            CheckBox rumble = new CheckBox(this); rumble.setText("Вибрация (со следующего запуска)"); rumble.setChecked(p.getBoolean("rumble", false)); box.addView(rumble);
            CheckBox fps = new CheckBox(this); fps.setText("Счётчик FPS и времени кадра");
            fps.setChecked(getSharedPreferences("doom_controls", MODE_PRIVATE).getBoolean("fps", true)); box.addView(fps);
            TextView rl = new TextView(this); rl.setText("Разрешение 3D (интерфейс остаётся 720p):"); rl.setPadding(0, 24, 0, 0); if (!XC2 && !D3) box.addView(rl);
            RadioGroup res = new RadioGroup(this);
            for (int i = 0; i < RES_LABELS.length; i++) { RadioButton r = new RadioButton(this); r.setText(RES_LABELS[i]); r.setId(1000 + i); res.addView(r); }
            res.check(1000 + p.getInt("res", 0)); if (!XC2 && !D3) box.addView(res);
            TextView sl = new TextView(this); sl.setText("Резкость (FSR RCAS):"); sl.setPadding(0, 24, 0, 0); box.addView(sl);
            RadioGroup sh = new RadioGroup(this); sh.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < SHARPEN_LABELS.length; i++) { RadioButton r = new RadioButton(this); r.setText(SHARPEN_LABELS[i]); r.setId(2000 + i); sh.addView(r); }
            sh.check(2000 + p.getInt("sharpen", 0)); box.addView(sh);
            TextView note = new TextView(this); note.setText(D3 ? "Драйвер, язык, вибрация и резкость — со следующего запуска; управление и счётчик FPS — сразу." : XC2 ? "Динамическое разрешение, формат 16:9 / растянуть и управление применяются сразу; драйвер, язык и расширенный формат — со следующего запуска." : "Графика применится при следующем запуске игры, управление — сразу."); note.setPadding(0, 24, 0, 0);
            SharedPreferences cp = getSharedPreferences("doom_controls", MODE_PRIVATE);
            TextView ll = new TextView(this); ll.setText(XC2 ? "Чувствительность камеры (свайп):" : "Чувствительность обзора (свайп и кнопка огня):"); ll.setPadding(0, 24, 0, 0); if (!D3) box.addView(ll);   // [D3] no camera to turn
            RadioGroup look = new RadioGroup(this); look.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < LOOK_GAINS.length; i++) { RadioButton r = new RadioButton(this); r.setText(LOOK_LABELS[i]); r.setId(3000 + i); look.addView(r); }
            look.check(3000 + nearest(LOOK_GAINS, cp.getFloat("look", 1.0f))); if (!D3) box.addView(look);
            TextView dl = new TextView(this); dl.setText(D3 ? "Экранные стики: компенсация мёртвой зоны игры:" : "Стик движения: компенсация мёртвой зоны игры:"); dl.setPadding(0, 24, 0, 0); box.addView(dl);
            RadioGroup dz = new RadioGroup(this); dz.setOrientation(RadioGroup.HORIZONTAL);
            for (int i = 0; i < DEADZONES.length; i++) { RadioButton r = new RadioButton(this); r.setText(DEADZONE_LABELS[i]); r.setId(4000 + i); dz.addView(r); }
            dz.check(4000 + nearest(DEADZONES, cp.getFloat("move_dz", 0f))); box.addView(dz);
            box.addView(note);
            android.widget.ScrollView scroll = new android.widget.ScrollView(this); scroll.addView(box);
            new AlertDialog.Builder(this).setTitle(D3 ? "Настройки" : "Графика").setView(scroll)
                .setPositiveButton("Сохранить", (d, w) -> {
                    p.edit().putBoolean(XC2 ? "xc2_drs" : "drs", drs.isChecked()).putString("driver", drvG.getCheckedRadioButtonId() > 7000 && drvG.getCheckedRadioButtonId() - 7001 < drvNames.length ? drvNames[drvG.getCheckedRadioButtonId() - 7001] : "").putBoolean("rumble", rumble.isChecked()).putFloat("aa", AA_STRENGTHS[Math.min(Math.max(0, aaG.getCheckedRadioButtonId() - 6000), AA_STRENGTHS.length - 1)]).putString("lang", LANG_CODES[Math.min(Math.max(0, langG.getCheckedRadioButtonId() - 5000), LANG_CODES.length - 1)])
                        .putInt("res", Math.max(0, res.getCheckedRadioButtonId() - 1000)).putInt("sharpen", Math.max(0, sh.getCheckedRadioButtonId() - 2000)).apply();
                    int li = Math.max(0, look.getCheckedRadioButtonId() - 3000), di = Math.max(0, dz.getCheckedRadioButtonId() - 4000);
                    getSharedPreferences("doom_controls", MODE_PRIVATE).edit().putBoolean("fps", fps.isChecked()).putFloat("look", LOOK_GAINS[Math.min(li, LOOK_GAINS.length - 1)])
                        .putFloat("move_dz", DEADZONES[Math.min(di, DEADZONES.length - 1)]).apply();
                    if (doomPad != null) doomPad.reloadSettings();
                    if (XC2) {   // live: the game's DRS step and the 16:9 bars / stretch layout (widened needs the next launch)
                        p.edit().putInt("aspect", Math.min(Math.max(0, aspG.getCheckedRadioButtonId() - 8000), ASPECT_LABELS.length - 1)).apply();
                        if (started) nativeSetDrs(drs.isChecked());
                        applyAspectLayout();
                    }
                })
                .setNegativeButton("Отмена", null).show();
        });
    }

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!DOOM && !libraryLoaded && !ensureImported(new File(downloads, "S2X/D2R Mobile/android"))) return;
        if (gameFrame.getParent() == null) setContentView(gameFrame);
        if (!libraryLoaded) {
            // 与 jni_android.c 选运行目录同一条规则：内部目录有主程序就用内部目录，否则用外部私有目录
            // Same rule as jni_android.c's choice of runtime directory: internal storage when it has the main module, otherwise external private storage
            File ext = getExternalFilesDir(null), internal = new File(getFilesDir(), "dm");
            File runtime = new File(internal, "out/main.patched.bin").canRead() || ext == null ? internal : new File(ext, "dm");
            try { Os.setenv("DM_DERIVED", new File(runtime, "derived.tsv").getAbsolutePath(), true); } catch (ErrnoException e) { showStartError(getString(R.string.dmAndroidStorage)); return; }
            if (DOOM) applyDoomSettings();
            try { System.loadLibrary("dm"); libraryLoaded = true; }
            catch (UnsatisfiedLinkError error) { showStartError(getString(R.string.dmAndroidLibrary)); return; }
            nativeContext(getApplicationContext());
        }
        if (DOOM && doomPad == null) { doomPad = new DoomPadView(this); gameFrame.addView(doomPad, new FrameLayout.LayoutParams(-1, -1)); updatePadVisibility(); }   // [DOOM] on-screen controller
        if (touchOverlay == null && !DOOM) {   // [DOOM] the D2R touch layout does not apply; a DOOM layout comes later
            int[] gameIds = {R.string.dmAndroidAttack, R.string.dmAndroidSkill1, R.string.dmAndroidSkill2, R.string.dmAndroidSkill3, R.string.dmAndroidSkill4,
                R.string.dmAndroidPortal, R.string.dmAndroidPotion1, R.string.dmAndroidPotion2, R.string.dmAndroidPotion3, R.string.dmAndroidPotion4,
                R.string.dmAndroidMenu, R.string.dmAndroidMap, R.string.dmAndroidMove, R.string.dmAndroidSwap, R.string.dmAndroidDifficulty};
            int[] navIds = {R.string.dmConfirm, R.string.dmAndroidBack, R.string.dmAndroidAction, R.string.dmAndroidBind, R.string.dmAndroidNext};
            String[] gameLabels = new String[gameIds.length], navLabels = new String[navIds.length];
            for (int i = 0; i < gameIds.length; i++) gameLabels[i] = getString(gameIds[i]);
            for (int i = 0; i < navIds.length; i++) navLabels[i] = getString(navIds[i]);
            touchOverlay = new TouchOverlay(this, gameLabels, navLabels);
            touchOverlay.setOnApplyWindowInsetsListener((v, insets) -> {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.displayCutout());
                touchOverlay.setSafeInsets(safe.left, safe.right, safe.bottom);
                return insets;
            });
            gameFrame.addView(touchOverlay, new FrameLayout.LayoutParams(-1, -1));
            touchOverlay.requestApplyInsets();
        }
        if (!view.getHolder().getSurface().isValid() || view.getWidth() <= 0 || view.getHeight() <= 0) return;
        File ext = getExternalFilesDir(null);
        if (ext == null) { showStartError(getString(R.string.dmAndroidStorage)); return; }
        nativeSurface(view.getHolder().getSurface(), view.getWidth(), view.getHeight());
        updateRefresh();
        // 资源目录缺省 Download/S2X/D2R Mobile/data；没有这个目录、而早先的 Download/data 在时沿用早先的位置，已装好的手机不必搬数据。
        // 按目录判断而不按 .build.config：它也可以由导入的覆盖层提供，资源目录里就没有。
        // The resource directory defaults to Download/S2X/D2R Mobile/data; when that directory does not exist but the earlier
        // Download/data does, the earlier one is kept, so existing installs need not move their data. The test is the directory,
        // not .build.config: the imported overlay can provide that file, and then the resource directory has none.
        File data = new File(downloads, "S2X/D2R Mobile/data"), earlier = new File(downloads, "data");
        if (!data.isDirectory() && earlier.isDirectory()) data = earlier;
        String src = DOOM ? new File(downloads, DATA_DIR).getAbsolutePath() : data.getAbsolutePath();   // [DOOM] Download/DOOM Mobile/{romfs,dm}
        int result = nativeStart(getFilesDir().getAbsolutePath(), ext.getAbsolutePath(), src, Locale.getDefault().toLanguageTag());
        if (result < 0) {
            int resource = switch (result) {
                case -2 -> R.string.dmAndroidMissingData;
                case -3 -> R.string.dmAndroidMissingModules;
                case -4 -> R.string.dmAndroidMissingShaders;
                case -5 -> R.string.dmAndroidMissingConfig;
                case -9 -> R.string.dmAndroidMissingOverlay;
                case -10 -> R.string.dmAndroidInvalidOverlay;
                default -> 0;
            };
            showStartError(resource == 0 ? getString(R.string.dmAndroidStartFailed, result) : getString(resource)); return;
        }
        started = true;
        if (touchOverlay != null) touchOverlay.setInputEnabled(true);
        if (D3) {   // [D3] driver crash guard / fallback message (driverWatch), and the notice from the guard at this launch
            if (driverNotice != null) { android.widget.Toast.makeText(this, driverNotice, android.widget.Toast.LENGTH_LONG).show(); driverNotice = null; }
            view.postDelayed(driverWatch, 1000);
        }
        nativeFocus(3);
    }

    private void showStartError(String message) {
        TextView t = new TextView(this);
        t.setText(message); t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        setContentView(t);
    }

    // 原生线程只投递主线程工作，不持有 Activity，也不直接操作视图。
    // Native threads only post UI work; they neither retain the Activity nor touch views.
    private static void showNativeFailure(int code) {
        bootFailure = code;
        DmActivity activity = current.get();
        if (activity != null) activity.runOnUiThread(() -> {
            if (!activity.isDestroyed()) activity.showBootFailure();
        });
    }

    private void showBootFailure() {
        releaseTouches();
        showStartError(getString(bootFailure == -6 ? R.string.dmAndroidGraphicsFailed : R.string.dmAndroidHostStopped));
    }

    private static void showNativeInput(long id, String title, String sub, String guide, String initial,
                                        int maxChars, int nameRule, int numericMax) {
        DmActivity activity = current.get();
        if (activity == null) { nativeTextResult(id, false, ""); return; }
        activity.runOnUiThread(() -> activity.openInput(id, title, sub, guide, initial, maxChars, nameRule != 0, numericMax));
    }

    private void openInput(long id, String title, String sub, String guide, String initial,
                           int maxChars, boolean nameRule, int numericMax) {
        if (!resumed || isFinishing() || isDestroyed() || inputDialog != null) { nativeTextResult(id, false, ""); return; }
        releaseTouches();
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setInputType(numericMax >= 0 ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setHint(nameRule ? getString(R.string.dmNameBlankRandom) : guide);
        input.setText(TextInputRules.normalize(initial, maxChars, nameRule, numericMax));
        input.setSelection(input.length());
        String message = sub + (sub.isEmpty() || guide.isEmpty() ? "" : "\n") + guide;
        if (nameRule) message += (message.isEmpty() ? "" : "\n") + getString(R.string.dmNameRule);
        if (numericMax >= 0) message = getString(R.string.dmAndroidQuantityRange, numericMax);
        if (title.isEmpty()) title = getString(nameRule ? R.string.dmNameTitle : numericMax >= 0 ? R.string.dmQtyTitle : R.string.dmInputTitle);
        final boolean[] finished = {false};
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(title).setMessage(message).setView(input)
            .setPositiveButton(R.string.dmConfirm, null).setNegativeButton(R.string.dmCancel, (d, which) -> {})
            .create();
        inputDialog = dialog;
        dialog.setOnDismissListener(d -> {
            if (inputDialog == dialog) inputDialog = null;
            if (!finished[0]) { finished[0] = true; nativeTextResult(id, false, ""); }
            if (resumed) immersive();
        });
        dialog.setOnShowListener(d -> {
            Runnable check = () -> {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                int error = nameRule ? TextInputRules.nameError(input.getText().toString()) : 0;
                input.setError(composing ? getString(R.string.dmNamePickIme) : error == 1 ? getString(R.string.dmNameTooShort) : error == 2 ? getString(R.string.dmNameEdge) : null);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(!composing && error == 0);
            };
            Runnable settle = () -> {
                Editable text = input.getText();
                if (BaseInputConnection.getComposingSpanStart(text) < 0) {
                    String normalized = TextInputRules.normalize(text.toString(), maxChars, nameRule, numericMax);
                    if (!normalized.contentEquals(text)) { input.setText(normalized); input.setSelection(input.length()); return; }
                }
                check.run();
            };
            // 输入法结束组字（点候选、按完成）时常常只去掉组字标记、文字不变：TextWatcher 收不到，「确定」一直灰、提示停在「先选完字」。
            // 组字状态一变就和文字变化一样处理（规整 + 重判）：SpanWatcher 挂在文字上，Editable.Factory 让每次 setText 换上的新文字也挂着它。
            // When the IME finishes composing (picking a candidate, Done) it often only removes the composing span and keeps the text: TextWatcher is not
            // called, so OK stays disabled with "finish choosing characters". A change of composing state is handled like a text change (normalize + check):
            // a SpanWatcher sits on the text, and an Editable.Factory attaches it to the new text after every setText.
            SpanWatcher composingWatch = new SpanWatcher() {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                private void changed(Spannable text) {
                    boolean now = BaseInputConnection.getComposingSpanStart(text) >= 0;
                    if (now != composing) { composing = now; input.post(settle); }
                }
                public void onSpanAdded(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanRemoved(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend) { changed(text); }
            };
            input.setEditableFactory(new Editable.Factory() {
                @Override public Editable newEditable(CharSequence source) {
                    Editable e = super.newEditable(source);
                    e.setSpan(composingWatch, 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
                    return e;
                }
            });
            input.getText().setSpan(composingWatch, 0, input.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
            input.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                public void onTextChanged(CharSequence s, int start, int before, int count) {}
                public void afterTextChanged(Editable text) { settle.run(); }
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                finished[0] = true;
                nativeTextResult(id, true, input.getText().toString());
                dialog.dismiss();
            });
            check.run();
            input.requestFocus();
            if (dialog.getWindow() != null) dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        });
        dialog.show();
    }

    private void releaseTouches() {
        if (touchOverlay != null) touchOverlay.cancelTouches();
        if (started) nativeTouch(0, new int[0], new float[0], new float[0]);
    }

    private void updateRefresh() {
        if (!libraryLoaded) return;
        android.view.Display d = view.getDisplay();
        nativeRefresh(d == null ? 0 : d.getRefreshRate());
        if (d != null) nativeRotation(d.getRotation());
    }

    // 游戏手柄：手柄 / 摇杆 / 十字键来源（非虚拟设备）的按键与摇杆交原生层；原生层不认的键照常交给系统。
    // Game controllers: keys and sticks from gamepad / joystick / d-pad sources of a real (not virtual) device go to the native layer; a key
    // the native layer does not map goes on to the system as usual. The phone's own Back (a virtual keyboard source) is never taken.
    private static boolean controller(InputDevice d, int source) {
        if (d == null || d.isVirtual()) return false;
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (DOOM && e.getAction() == KeyEvent.ACTION_DOWN) android.util.Log.i("dm", "[input] java key " + e.getKeyCode() + " source 0x" + Integer.toHexString(e.getSource()) + " device " + e.getDeviceId() + " started " + started + " dialog " + (inputDialog != null));
        if (started && inputDialog == null && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
            && (controller(e.getDevice(), e.getSource())
                || (DOOM && ((e.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD || e.getSource() == 0) && e.getKeyCode() != KeyEvent.KEYCODE_BACK
                    && e.getKeyCode() != KeyEvent.KEYCODE_SCROLL_LOCK))   // [ETERNAL] Scroll Lock = tools/keep_awake.sh's screen keep-alive, not game input   // [DOOM] keyboard too
            && nativePadKey(e.getDeviceId(), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) {
            if (DOOM && controller(e.getDevice(), e.getSource()) && (e.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0) padUsed(e.getDeviceId());
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        int src = e.getSource();
        if (started && inputDialog == null && e.getAction() == MotionEvent.ACTION_MOVE && e.getDevice() != null && !e.getDevice().isVirtual()
            && (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            nativePadAxes(e.getDeviceId(), e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            if (DOOM && (Math.abs(e.getAxisValue(MotionEvent.AXIS_X)) > 0.5f || Math.abs(e.getAxisValue(MotionEvent.AXIS_Y)) > 0.5f
                || Math.abs(e.getAxisValue(MotionEvent.AXIS_Z)) > 0.5f || Math.abs(e.getAxisValue(MotionEvent.AXIS_RZ)) > 0.5f
                || e.getAxisValue(MotionEvent.AXIS_HAT_X) != 0 || e.getAxisValue(MotionEvent.AXIS_HAT_Y) != 0)) padUsed(e.getDeviceId());
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
    @Override public void onInputDeviceAdded(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceChanged(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceRemoved(int id) { if (libraryLoaded) nativePadRemoved(id); usedPads.remove(id); updatePadVisibility(); }

    // [DOOM] A real controller connected (external gamepad / joystick): the on-screen pad hides all but the settings button,
    // and comes back when the last one is disconnected.
    // A device counts only once it has sent controller input: emulators (LDPlayer's "joystick") and some phones register
    // always-present gamepad-class devices for key mapping, which would hide the pad with no controller in hand.
    private final java.util.Set<Integer> usedPads = new java.util.HashSet<>();
    private void padUsed(int id) { if (usedPads.add(id)) updatePadVisibility(); }
    private boolean physicalController(InputDevice d) {
        if (d == null || d.isVirtual() || !usedPads.contains(d.getId())) return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    void updatePadVisibility() {
        if (doomPad == null) return;
        boolean any = false;
        for (int id : InputDevice.getDeviceIds()) if (physicalController(InputDevice.getDevice(id))) { any = true; break; }
        doomPad.setControllerMode(any);
    }
    @Override public void onDisplayAdded(int id) { updateRefresh(); }
    @Override public void onDisplayRemoved(int id) { updateRefresh(); }
    @Override public void onDisplayChanged(int id) { updateRefresh(); }
    @Override protected void onResume() { super.onResume(); resumed = true; immersive(); startHost(); updateRefresh(); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) touchOverlay.setInputEnabled(true); } }
    @Override protected void onPause() { resumed = false; if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) nativeFocus(2); }
    @Override protected void onStop() { if (started) nativeFocus(1); com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (displays != null) displays.unregisterDisplayListener(this); if (inputs != null) inputs.unregisterInputDeviceListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
