package com.xiaoboluo.d2r;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.PixelCopy;
import android.view.SurfaceView;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

// [D3] Skill icons for the on-screen buttons (D3PadView, gear "Skill icons on buttons", pref doom_controls "d3_icons"), copied from the
// game's own console HUD: about once a second (and soon after a resume) the game's SurfaceView is read back with PixelCopy on a
// background thread, the row of button letters under the HUD's skill slots is searched, each letter is recognised (A B X Y L R ZL ZR)
// and the round slot above it is cropped as the icon of that button. Only what the HUD itself labels gets an icon, so a slot the game
// has not unlocked yet, a remapped button (game Options -> Controller) or an unknown HUD stays a plain button.
//
// HUD geometry (ConsoleHUD_SinglePlayerFrame.ui: SinglePlayerTemplate Local.PotionButton + Local.SkillsParent.Skills.Slot1..6, flipped
// for the local player = bottom left). Measured on 2400x1080 phone captures of the stretched 16:9 picture (work/logs/nh1_100.png,
// vA_80.png, s1_80.png, m01_70.png, z3_70.png, ab_base2_80.png, all level-1 heroes: potion "R" + slot "A"), mapped back to the
// game's 1600x900 frame (x / 1.5, y / 1.2):
//   potion slot centre (85, 834.5), letter "R" columns 79..86; skill slot "A" centre (142.5, 834.5), letter "A" columns 138..147;
//   letters on rows 851..863 (centre 857, 13 px high); the round icon art has a radius of ~18 px; the slots are 57 px apart and the
//   XP bar above them runs to x ~456, room for the potion + 6 skill slots (142.5 + 5 x 57 = 427.5).
// The HUD sits outside the centred 4:3 UI region (x0 = 200 at 1600x900, docs/d3/WIDESCREEN.md) and is anchored to the left and bottom
// edges of the real window, at the console UI scale height / 900: so all positions here are game pixels at height 900 measured from
// the frame's left / bottom edge, and they hold for docked 1600x900, handheld 1280x720 (x 0.8) and widescreen W x 900 alike.
// PixelCopy copies the SurfaceView's buffer, which is the game's output size (vulkan_present.cpp present_native sets the buffer
// geometry to the game's crop), so the picture mode of the view (16:9 bars / stretched / widescreen) does not matter; the copy goes
// into a bitmap of the game's render size (DmActivity.d3RenderSize).
final class D3SkillIcons {
    static final long PERIOD_MS = 1000, RESUME_MS = 500;
    // Letter row search band (game px at height 900, from the left / to the letter centre) and slot geometry relative to a letter.
    static final float BAND_X0 = 30, BAND_X1 = 500, LETTER_Y = 857, BAND_HALF_H = 11;
    static final float SLOT_DY = -23.5f, ICON_HALF = 16;   // icon centre above the letter centre (22.5 measured, 1 px up keeps the letter out); crop half side (art radius ~18)
    static final float GLYPH_MIN_W = 3, GLYPH_MAX_W = 30, GLYPH_MIN_H = 7, GLYPH_MAX_H = 17, GLYPH_GAP = 2;   // a letter group ("ZR" = 2 letters)
    // Recognition: Pearson correlation of a GxG coverage grid against bold sans templates, minus an aspect-ratio penalty.
    static final int G = 10;
    static final float ACCEPT = 0.40f, MARGIN = 0.12f, MARGIN_ZLR = 0.05f, ASPECT_W = 0.6f;
    // An icon is taken when two samples in a row agree (signature correlation > SAME); a darker copy of the current icon (a cooldown
    // cover) is not taken; a button whose letter has not been seen in DROP_FRAMES frames that showed the HUD loses its icon.
    static final float SAME = 0.85f, COOLDOWN_DARK = 0.85f, BLACK_LUM = 12;
    static final int SIG = 12, ICON_PX = 96, DROP_FRAMES = 20;

    static final String[] LABELS = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    static final int[] KEYS = {D3PadView.K_A, D3PadView.K_B, D3PadView.K_X, D3PadView.K_Y, D3PadView.K_L, D3PadView.K_R, D3PadView.K_ZL, D3PadView.K_ZR};

    private final DmActivity act;
    private final Runnable changed;            // posted to the UI thread when the icon set changed
    private HandlerThread thread; private volatile Handler bg;
    private volatile boolean gate;             // sampling allowed now (option on, pad shown, play mode, activity resumed)
    private volatile Map<Integer, Bitmap> icons = Collections.emptyMap();   // key -> icon (immutable snapshot, read on the UI thread)

    // Background thread only:
    private Bitmap frame; private boolean busy;
    private final float[][] tmplGrid = new float[LABELS.length][]; private final float[] tmplAspect = new float[LABELS.length];
    private final Map<Integer, float[]> prevSig = new HashMap<>(), curSig = new HashMap<>();
    private final Map<Integer, Float> curLum = new HashMap<>();
    private final Map<Integer, Integer> unseen = new HashMap<>();

    D3SkillIcons(DmActivity act, Runnable changed) { this.act = act; this.changed = changed; }

    Bitmap icon(int key) { return icons.get(key); }

    void start() {
        if (thread != null) return;
        thread = new HandlerThread("d3-skill-icons"); thread.start(); bg = new Handler(thread.getLooper());
        bg.post(this::templates);
        bg.postDelayed(tick, RESUME_MS);
    }
    void stop() {
        if (thread == null) return;
        bg.removeCallbacksAndMessages(null); thread.quitSafely(); thread = null; bg = null;
    }
    // Called on the UI thread whenever one of the conditions changes; opening the gate samples soon (resume, menu mode off, ...).
    void setGate(boolean on) {
        boolean was = gate; gate = on;
        Handler h = bg;
        if (on && !was && h != null) { h.removeCallbacks(tick); h.postDelayed(tick, RESUME_MS); }
    }

    private final Runnable tick = new Runnable() { public void run() {
        Handler h = bg; if (h == null) return;
        h.postDelayed(this, PERIOD_MS);
        if (gate && !busy) request();
    } };

    private void request() {
        SurfaceView v = act.gameSurface();
        int[] gs = act.d3RenderSize();
        if (v == null || gs == null || gs[0] <= 0 || gs[1] <= 0) return;
        try {
            if (!v.getHolder().getSurface().isValid()) return;
            if (frame == null || frame.getWidth() != gs[0] || frame.getHeight() != gs[1]) frame = Bitmap.createBitmap(gs[0], gs[1], Bitmap.Config.ARGB_8888);
            busy = true;
            final Handler h = bg;
            PixelCopy.request(v, frame, r -> { busy = false; if (r == PixelCopy.SUCCESS && gate && h == bg) analyse(); }, h);
        } catch (RuntimeException e) {   // the surface went away between the check and the copy
            busy = false;
        }
    }

    // ---------------- recognition ----------------
    private void templates() {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)); p.setTextSize(64); p.setColor(0xffffffff);
        for (int i = 0; i < LABELS.length; i++) {
            Bitmap b = Bitmap.createBitmap(200, 100, Bitmap.Config.ARGB_8888);
            new Canvas(b).drawText(LABELS[i], 10, 80, p);
            int[] px = new int[200 * 100]; b.getPixels(px, 0, 200, 0, 0, 200, 100); b.recycle();
            float[] w = new float[px.length];
            for (int k = 0; k < px.length; k++) w[k] = ((px[k] >>> 24) & 0xff) / 255f;
            int[] bb = bbox(w, 200, 0, 0, 200, 100);
            if (bb == null) continue;
            tmplGrid[i] = grid(w, 200, bb[0], bb[1], bb[2] - bb[0], bb[3] - bb[1]);
            tmplAspect[i] = (bb[2] - bb[0]) / (float) (bb[3] - bb[1]);
        }
    }
    // Bounding box {x0, y0, x1, y1} (exclusive) of w > 0.5 inside the given rectangle, or null.
    private static int[] bbox(float[] w, int stride, int x0, int y0, int x1, int y1) {
        int ax = Integer.MAX_VALUE, ay = Integer.MAX_VALUE, bx = -1, by = -1;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (w[y * stride + x] > 0.5f) { ax = Math.min(ax, x); ay = Math.min(ay, y); bx = Math.max(bx, x); by = Math.max(by, y); }
        return bx < 0 ? null : new int[] {ax, ay, bx + 1, by + 1};
    }
    // Area-averaged G x G grid of w over the box (x0, y0, bw, bh).
    private static float[] grid(float[] w, int stride, int x0, int y0, int bw, int bh) {
        float[] g = new float[G * G];
        for (int gy = 0; gy < G; gy++) {
            float ya = gy * bh / (float) G, yb = (gy + 1) * bh / (float) G;
            for (int gx = 0; gx < G; gx++) {
                float xa = gx * bw / (float) G, xb = (gx + 1) * bw / (float) G, s = 0;
                for (int py = (int) ya; py < yb; py++) {
                    float oy = Math.min(yb, py + 1) - Math.max(ya, py); if (oy <= 0) continue;
                    for (int px = (int) xa; px < xb; px++) {
                        float ox = Math.min(xb, px + 1) - Math.max(xa, px); if (ox <= 0) continue;
                        s += ox * oy * w[(y0 + py) * stride + x0 + px];
                    }
                }
                g[gy * G + gx] = s / ((xb - xa) * (yb - ya));
            }
        }
        return g;
    }
    private static float corr(float[] a, float[] b) {
        float ma = 0, mb = 0; for (int i = 0; i < a.length; i++) { ma += a[i]; mb += b[i]; } ma /= a.length; mb /= b.length;
        float ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < a.length; i++) { float x = a[i] - ma, y = b[i] - mb; ab += x * y; aa += x * x; bb += y * y; }
        return aa <= 1e-6f || bb <= 1e-6f ? 0 : ab / (float) Math.sqrt(aa * bb);
    }
    // The label index of the letter group in w (box), or -1 when it is not clearly one of LABELS.
    private int recognise(float[] w, int stride, int x0, int y0, int bw, int bh) {
        float[] g = grid(w, stride, x0, y0, bw, bh); float asp = bw / (float) bh;
        int best = -1, second = -1; float bs = -9, ss = -9;
        for (int i = 0; i < LABELS.length; i++) {
            if (tmplGrid[i] == null) continue;
            float s = corr(g, tmplGrid[i]) - ASPECT_W * Math.abs((float) Math.log(asp / tmplAspect[i]));
            if (s > bs) { second = best; ss = bs; best = i; bs = s; } else if (s > ss) { second = i; ss = s; }
        }
        if (best < 0 || bs < ACCEPT) return -1;
        boolean zPair = second >= 0 && LABELS[best].startsWith("Z") && LABELS[second].startsWith("Z");
        return bs - ss >= (zPair ? MARGIN_ZLR : MARGIN) ? best : -1;
    }

    // ---------------- one frame ----------------
    private void analyse() {
        if (tmplGrid[0] == null) return;
        Bitmap f = frame; int fw = f.getWidth(), fh = f.getHeight();
        float u = fh / 900f;
        int bx0 = Math.round(BAND_X0 * u), bx1 = Math.min(fw, Math.round(BAND_X1 * u));
        int by0 = Math.round((LETTER_Y - BAND_HALF_H) * u), by1 = Math.min(fh, Math.round((LETTER_Y + BAND_HALF_H) * u) + 1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw <= 0 || bh <= 0) return;
        int[] px = new int[bw * bh]; f.getPixels(px, 0, bw, bx0, by0, bw, bh);
        // Whiteness: the letters are near-white with a dark outline; coloured or grey world pixels score low.
        float[] w = new float[px.length];
        for (int i = 0; i < px.length; i++) {
            int c = px[i], r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
            int mn = Math.min(r, Math.min(g, b)), mx = Math.max(r, Math.max(g, b));
            w[i] = mx - mn < 70 ? Math.max(0f, Math.min(1f, (mn - 110) / 100f)) : 0f;
        }
        boolean[] col = new boolean[bw];
        for (int x = 0; x < bw; x++) for (int y = 0; y < bh; y++) if (w[y * bw + x] > 0.5f) { col[x] = true; break; }
        int gap = Math.max(1, Math.round(GLYPH_GAP * u));
        Map<Integer, float[]> found = new HashMap<>();   // key -> slot centre {x, y} in the frame
        for (int x = 0; x < bw; ) {
            if (!col[x]) { x++; continue; }
            int s = x, e = x + 1, idle = 0;
            for (int k = x + 1; k < bw && idle <= gap; k++) { if (col[k]) { e = k + 1; idle = 0; } else idle++; }
            x = e;
            int gw = e - s;
            if (gw < GLYPH_MIN_W * u || gw > GLYPH_MAX_W * u) continue;
            int[] bb = bbox(w, bw, s, 0, e, bh);
            if (bb == null) continue;
            int gh = bb[3] - bb[1];
            if (gh < GLYPH_MIN_H * u || gh > GLYPH_MAX_H * u) continue;
            int li = recognise(w, bw, bb[0], bb[1], bb[2] - bb[0], gh);
            if (li < 0) continue;
            float cx = bx0 + (bb[0] + bb[2]) / 2f, cy = by0 + (bb[1] + bb[3]) / 2f + SLOT_DY * u;
            found.put(KEYS[li], new float[] {cx, cy});
        }
        if (found.isEmpty()) return;   // no HUD letters in this frame (menu, loading, cut-scene, map): keep the icons
        Map<Integer, Bitmap> next = null;
        for (Map.Entry<Integer, float[]> en : found.entrySet()) {
            int key = en.getKey(); float cx = en.getValue()[0], cy = en.getValue()[1], half = ICON_HALF * u;
            int x0 = Math.round(cx - half), y0 = Math.round(cy - half), side = Math.round(2 * half);
            unseen.put(key, 0);
            if (x0 < 0 || y0 < 0 || x0 + side > fw || y0 + side > fh || side < 4) continue;
            int[] ip = new int[side * side]; f.getPixels(ip, 0, side, x0, y0, side, side);
            float lum = 0; float[] sig = new float[SIG * SIG], lw = new float[ip.length];
            for (int i = 0; i < ip.length; i++) { int c = ip[i]; lw[i] = 0.299f * ((c >> 16) & 0xff) + 0.587f * ((c >> 8) & 0xff) + 0.114f * (c & 0xff); lum += lw[i]; }
            lum /= ip.length;
            if (lum < BLACK_LUM) { prevSig.remove(key); continue; }   // a black slot: fade, loading
            for (int gy = 0; gy < SIG; gy++) for (int gx = 0; gx < SIG; gx++) {
                int ax = gx * side / SIG, ay = gy * side / SIG, ex = Math.max(ax + 1, (gx + 1) * side / SIG), ey = Math.max(ay + 1, (gy + 1) * side / SIG); float s = 0;
                for (int y = ay; y < ey; y++) for (int x = ax; x < ex; x++) s += lw[y * side + x];
                sig[gy * SIG + gx] = s / ((ex - ax) * (ey - ay));
            }
            float[] prev = prevSig.put(key, sig);
            if (prev == null || corr(sig, prev) < SAME) continue;           // wait for a second, matching sample (passing effects, fades)
            float[] cur = curSig.get(key);
            if (cur != null) {
                float c = corr(sig, cur), cl = curLum.get(key);
                if (c >= SAME && lum < cl * COOLDOWN_DARK) continue;           // the same icon under a cooldown cover: keep the bright one
                if (c > 0.97f && Math.abs(lum - cl) < 3) continue;             // unchanged
            }
            Bitmap crop = Bitmap.createBitmap(f, x0, y0, side, side);
            Bitmap ic = Bitmap.createScaledBitmap(crop, ICON_PX, ICON_PX, true);
            if (ic != crop) crop.recycle();
            if (next == null) next = new HashMap<>(icons);
            next.put(key, ic); curSig.put(key, sig); curLum.put(key, lum);
        }
        for (int key : KEYS) {
            if (found.containsKey(key)) continue;
            prevSig.remove(key);
            int n = unseen.containsKey(key) ? unseen.get(key) + 1 : 1; unseen.put(key, n);
            if (n >= DROP_FRAMES && icons.containsKey(key)) {   // the HUD shows letters but not this one for a while: no slot on this button any more
                if (next == null) next = new HashMap<>(icons);
                next.remove(key); curSig.remove(key); curLum.remove(key);
            }
        }
        if (next != null) { icons = Collections.unmodifiableMap(next); act.runOnUiThread(changed); }
    }
}
