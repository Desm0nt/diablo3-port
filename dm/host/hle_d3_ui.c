// [D3] Touch phase 3 (docs/d3/TOUCH_DESIGN.md §7): the game's console UI seen from the host, on the game thread.
//   DM_D3_UIDUMP=1   log every change of the UI focus (UI manager +0x50) / active control (+0x28); when the file dm/ui-dump-now appears,
//                    write dm/ui-dump.tsv (visible controls: id, name, class, flags, rect in UI units / window pixels / picture fractions,
//                    parent, layer, sibling index, activatable, focus handler, list data) and dm/ui-dump-raw.tsv (first 0x400 bytes of each
//                    visible control, hex) and delete the trigger. DM_D3_UIDUMP=2: every control, visible or not.
//                    Names come from dm/ui-names.tsv if present (tools/d3_uiid_names.py; the game keeps only 64-bit hashes).
//   DM_D3_UITAP=1    dm_d3_ui_tap(x, y) (picture fractions, any thread) queues a tap; the next npad poll (the game's main thread) finds the
//                    topmost visible control under the point that is activatable / has its own focus handler / is a list, calls the game's
//                    UI_SetFocus (main+0x327dd0) on it and holds A for 3 polls one poll later. DM_D3_UITAP=2: any visible control.
//                    Test without the front end: a file dm/ui-tap-now holding "x y" queues the same tap (checked every 30 polls).
//   DM_D3_UI_CRUMBS=1 (default; 0 = off) breadcrumbs for crash reports (boot.log -> Download/Diablo3Mobile/logs/crash.txt): with dump
//                    and tap off, the reader still logs each change of the UI focus / active control (which menu or screen was up when the
//                    game died), at most 8 lines a second (a flickering focus is summed up in the next line). Reads only, on the game
//                    thread, two pointer reads a poll after the 2.7.7 sites check.
// Everything is read through rd(): guest heap directly, main's image directly, anything else through process_vm_readv (a bad pointer
// reads as "absent", never faults). Addresses: update 2.7.7 main, build-id 2607a74f; the entry points are checked word for word first.
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include "dm_input.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum {
    O_G           = 0x1905610,   // &g (GOT 0x114a840); g+0x10 -> object: +0xb80 UI manager, +0xb78 UI render-target holder (0x12f030)
    O_GOT_G       = 0x114a840,
    O_GFXP        = 0x1830f68,   // (GOT 0x1150470) -> gfx: +0x244 / +0x248 UI window width / height in pixels (0x29a760)
    O_SETFOCUS    = 0x327dd0,    // void UI_SetFocus(const UIID *)
    O_UIRECT      = 0x34baf0,    // void UI_PixelRect(float out[4] l,t,r,b): the centred 4:3 region the 960x720 UI space maps to
    O_UPDATERECT  = 0x3b31f0,    // vtable+0xb0 of every control class that has a rect (+0x88 l,t,r,b in UI units)
    O_FOCUS_NOP   = 0x22d4c0,    // vtable+0x40 OnFocusGained of the base class (ret)
    O_ACT_NONE    = 0x22d470,    // vtable+0x10 CanActivate: base (return 0)
    O_ACT_BUTTON  = 0x38b370,    //   visible (+0x18 bit 2) && (+0x3c8 & 0x20)
    O_ACT_EDIT    = 0x38f720,    //   +0x1e8 != 0 && visible
    O_ACT_FLAG    = 0x38f960,    //   visible && (+0x3c8 & 1)
    O_VT_LIST     = 0x10a65f0,   // list class (IsA 51): +0x1e0 selected, +0x1e4 first row, +0x2a4 per row, +0x210 item UIIDs, +0x218 count
    UI_W = 960, UI_H = 720,      // UI virtual space (main 0x34bc80: 960.0 / 720.0 at 0xeb09e0 / 0xeb0e68)
    MAX_CTRL = 8192, MAX_DEPTH = 40, RAW_BYTES = 0x400,
};
static const struct { uint32_t off, w[4]; } k_sites[] = {
    {O_SETFOCUS, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},
    {O_UIRECT,   {0xd10103ff, 0xfd0013e8, 0xf90017f3, 0xa9037bfd}},
    {O_UPDATERECT, {0xd10143ff, 0x6d0123e9, 0xf90013f5, 0xa9034ff4}},
};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1;   // -1 unchecked, 0 not this main, 1 ok
static int g_dump_mode, g_tap_mode, g_crumbs;
static char g_dir[1100];

// ---- safe guest reads
int dm_guest_heap_readable(uint64_t a, size_t n);
static int rd(uint64_t a, void *out, size_t n) {
    if (a < 0x10000 || a >= (1ull << 48) || n > (1u << 20)) return 0;
    if (dm_guest_heap_readable(a, n)) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (a >= b && n <= g_size && a - b <= g_size - n) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    ssize_t got = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL);
    if (got < 0) { static int said; if (!said++ && errno != EFAULT) LOG("[d3ui] process_vm_readv: errno %d (only heap / image reads)", errno); }
    return got == (ssize_t)n;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static int in_image(uint64_t a) { uint64_t b = (uint64_t)(uintptr_t)g_base; return a >= b && a - b < g_size; }
static uint64_t mo(uint64_t a) { return a - (uint64_t)(uintptr_t)g_base; }

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_D3_UIDUMP"); g_dump_mode = e && *e ? atoi(e) : 0;
    e = getenv("DM_D3_UITAP"); g_tap_mode = e && *e ? atoi(e) : 0;
    e = getenv("DM_D3_UI_CRUMBS"); g_crumbs = !(e && *e == '0');   // [D3] breadcrumbs: focus changes into boot.log (default on)
    if (!g_dump_mode && !g_tap_mode && !g_crumbs) return 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < O_G + 0x20) { LOG("【红】[d3ui] no main module"); return 0; }
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, sizeof k_sites[i].w)) {
            LOG("【红】[d3ui] main+%#x differs: not D3 update 2.7.7 (build 2607a74f), UI dump / tap off", k_sites[i].off); return 0; }
    if (*(uint64_t *)(g_base + O_GOT_G) != (uint64_t)(uintptr_t)(g_base + O_G)) { LOG("【红】[d3ui] GOT main+%#x does not point at main+%#x: off", O_GOT_G, O_G); return 0; }
    e = getenv("DM_RUNTIME_DIR"); snprintf(g_dir, sizeof g_dir, "%s", e && *e ? e : "dm");
    LOG("[d3ui] on: dump %d, tap %d, crumbs %d, trigger %s/ui-dump-now, game thread tid %d", g_dump_mode, g_tap_mode, g_crumbs, g_dir, (int)gettid());
    return g_ok = 1;
}

// ---- names (dm/ui-names.tsv, loaded off the game thread)
typedef struct { uint64_t id; char *path; } name_ent;
static name_ent *g_names; static size_t g_nnames; static _Atomic int g_names_state;   // 0 not started, 1 loading, 2 ready
static int name_cmp(const void *a, const void *b) { uint64_t x = ((const name_ent *)a)->id, y = ((const name_ent *)b)->id; return x < y ? -1 : x > y; }
static void *names_load(void *arg) {
    (void)arg; char p[1200]; snprintf(p, sizeof p, "%s/ui-names.tsv", g_dir);
    FILE *f = fopen(p, "r"); size_t cap = 0, n = 0; name_ent *v = NULL; char line[1024];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t'); if (!tab || line[0] == '#') continue;
            line[strcspn(line, "\r\n")] = 0;
            if (n == cap) { cap = cap ? cap * 2 : 4096; name_ent *nv = realloc(v, cap * sizeof *v); if (!nv) break; v = nv; }
            v[n].id = strtoull(line, NULL, 16); v[n].path = strdup(tab + 1); if (v[n].path) n++;
        }
        fclose(f); qsort(v, n, sizeof *v, name_cmp);
    }
    g_names = v; g_nnames = n; atomic_store(&g_names_state, 2);
    LOG("[d3ui] names: %zu from %s%s", n, p, f ? "" : " (missing: ids only; tools/d3_uiid_names.py makes it)");
    return NULL;
}
static const char *name_of(uint64_t id) {
    if (atomic_load(&g_names_state) != 2) return "";
    size_t lo = 0, hi = g_nnames;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_names[m].id < id) lo = m + 1; else hi = m; }
    return lo < g_nnames && g_names[lo].id == id ? g_names[lo].path : "";
}

// ---- UI manager and its control map (main 0x330000: {u32 mask, u32 count, alloc, buckets @0x10}; node {next, UIID, control})
static uint64_t ui_mgr(void) { uint64_t o = rq((uint64_t)(uintptr_t)(g_base + O_G) + 0x10); return o ? rq(o + 0xb80) : 0; }
typedef struct {
    uint64_t id, c, vt, parent; uint16_t flags; char type[24]; int has_rect, act, fh, list, layer, depth, sib;
    float r[4]; int path[MAX_DEPTH]; int npath;
} ctrl;
static ctrl *g_c; static int g_nc;
static int ctrl_cmp(const void *a, const void *b) { uint64_t x = ((const ctrl *)a)->id, y = ((const ctrl *)b)->id; return x < y ? -1 : x > y; }
static ctrl *find(uint64_t id) {
    int lo = 0, hi = g_nc;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_c[m].id < id) lo = m + 1; else hi = m; }
    return lo < g_nc && g_c[lo].id == id ? &g_c[lo] : NULL;
}
// Class: every "cmp w1, #imm" (0x7100003f | imm12 << 10) of the vtable+0x88 IsA(type) up to its first ret / b, e.g. "37/1/51/2" (list).
// The compiler's compare order is not the derivation order; 1 = UIControl base, 37 = container (child vector +0x60), 2 = has a rect.
static void isa_types(uint64_t vt, char *out, size_t cap) {
    uint64_t f = rq(vt + 0x88); uint32_t w[16]; size_t n = 0; out[0] = 0;
    if (!in_image(f) || !rd(f, w, sizeof w)) { snprintf(out, cap, "?"); return; }
    for (int i = 0; i < 16 && w[i] != 0xd65f03c0 && (w[i] & 0xfc000000) != 0x14000000; i++)
        if ((w[i] & 0xffc003ff) == 0x7100003f && n + 6 < cap) n += (size_t)snprintf(out + n, cap - n, "%s%u", n ? "/" : "", w[i] >> 10 & 0xfff);
    if (!n) snprintf(out, cap, "?");
}
// Snapshot of every control (game thread). Returns the count, -1 when the UI does not exist yet.
static int snapshot(uint64_t mgr) {
    uint64_t map = rq(mgr); uint32_t hdr[2]; uint64_t buckets;
    if (!map || !rd(map, hdr, 8) || !(buckets = rq(map + 0x10))) return -1;
    uint32_t mask = hdr[0];
    if (mask > 0xfffff || (mask & (mask + 1))) return -1;
    if (!g_c && !(g_c = calloc(MAX_CTRL, sizeof *g_c))) return -1;
    g_nc = 0;
    for (uint32_t i = 0; i <= mask && g_nc < MAX_CTRL; i++) {
        uint64_t node = rq(buckets + 8ull * i);
        for (int guard = 0; node && guard < 4096 && g_nc < MAX_CTRL; guard++) {
            uint64_t nd[3]; if (!rd(node, nd, sizeof nd)) break;
            uint8_t h[0x40]; uint64_t vt;
            if (nd[2] && rd(nd[2], h, sizeof h) && in_image(vt = *(uint64_t *)h) && *(uint64_t *)(h + 0x30) == nd[1]) {
                ctrl *c = &g_c[g_nc++]; memset(c, 0, sizeof *c);
                c->id = nd[1]; c->c = nd[2]; c->vt = vt; c->flags = *(uint16_t *)(h + 0x18); c->parent = *(uint64_t *)(h + 0x38);
                isa_types(vt, c->type, sizeof c->type); c->has_rect = rq(vt + 0xb0) == (uint64_t)(uintptr_t)(g_base + O_UPDATERECT);
                if (c->has_rect && !rd(nd[2] + 0x88, c->r, sizeof c->r)) c->has_rect = 0;
                int vis = c->flags >> 2 & 1; uint64_t act = mo(rq(vt + 0x10)); uint32_t f3c8 = 0, f1e8 = 0;
                c->act = act == O_ACT_NONE ? 0 : act == O_ACT_BUTTON ? (rd(nd[2] + 0x3c8, &f3c8, 4) ? vis && (f3c8 & 0x20) : -1)
                       : act == O_ACT_EDIT ? (rd(nd[2] + 0x1e8, &f1e8, 4) ? vis && f1e8 : -1) : act == O_ACT_FLAG ? (rd(nd[2] + 0x3c8, &f3c8, 4) ? vis && (f3c8 & 1) : -1) : -1;
                c->fh = mo(rq(vt + 0x40)) != O_FOCUS_NOP;
                c->list = mo(vt) == O_VT_LIST;
            }
            node = nd[0];
        }
    }
    qsort(g_c, g_nc, sizeof *g_c, ctrl_cmp);
    // tree position: sibling index in the parent's child vector (+0x60 {UIID, ctrl, ctrl-if-visible} x 0x18, count +0x68), layer, depth
    for (int i = 0; i < g_nc; i++) {
        ctrl *c = &g_c[i]; c->sib = -1;
        ctrl *p = find(c->parent);
        if (p) {
            uint64_t v = rq(p->c + 0x60), n = rq(p->c + 0x68);
            for (uint64_t k = 0; v && k < n && k < 4096; k++) if (rq(v + 0x18 * k) == c->id) { c->sib = (int)k; break; }
        }
    }
    for (int i = 0; i < g_nc; i++) {
        ctrl *c = &g_c[i]; int rev[MAX_DEPTH], n = 0; uint64_t root = c->id;
        for (ctrl *x = c; x && n < MAX_DEPTH; x = find(x->parent)) { rev[n++] = x->sib; root = x->id; }
        c->layer = root == 3 ? 3 : root == 2 ? 2 : root == 1 ? 1 : 0;   // Root.TopLayer / WindowLayer / NormalLayer (main+0xeba350)
        c->depth = n; c->npath = n; for (int k = 0; k < n; k++) c->path[k] = rev[n - 1 - k];
    }
    return g_nc;
}
static int above(const ctrl *a, const ctrl *b) {   // a drawn after b: higher layer, then later sibling along the path, then deeper
    if (a->layer != b->layer) return a->layer > b->layer;
    for (int k = 0; k < a->npath && k < b->npath; k++) if (a->path[k] != b->path[k]) return a->path[k] > b->path[k];
    return a->npath > b->npath;
}

// UI units -> window pixels: the game's own 4:3 pixel region (0x34baf0) when its inputs exist, else the same formula from the window size
typedef struct { float l, t, r, b; unsigned w, h; int from_game; } pxmap;
static int px_map(pxmap *m) {
    uint64_t gfx = rq((uint64_t)(uintptr_t)(g_base + O_GFXP)); uint32_t wh[2];
    if (!gfx || !rd(gfx + 0x244, wh, 8) || !wh[0] || !wh[1] || wh[0] > 8192 || wh[1] > 8192) return 0;
    m->w = wh[0]; m->h = wh[1]; m->from_game = 0;
    float uw = floorf((float)wh[1] * 1.3333334f); m->l = ((float)wh[0] - uw) * 0.5f; m->r = m->l + uw; m->t = 0; m->b = (float)wh[1];
    uint64_t o = rq((uint64_t)(uintptr_t)(g_base + O_G) + 0x10), tgt = o ? rq(o + 0xb78) : 0, t20 = tgt ? rq(tgt + 0x20) : 0;
    if (t20 && in_image(rq(t20))) {   // 0x34baf0 dereferences (*(g+0x10)+0xb78)+0x20 and its vtable+0x28: call only when they exist
        float r[4] = {0}; ((void (*)(float *))(g_base + O_UIRECT))(r);
        if (isfinite(r[0]) && isfinite(r[3]) && r[2] - r[0] > 16 && r[3] - r[1] > 16) { m->l = r[0]; m->t = r[1]; m->r = r[2]; m->b = r[3]; m->from_game = 1; }
    }
    return 1;
}
static float ux2px(const pxmap *m, float x) { return m->l + x * (m->r - m->l) / UI_W; }
static float uy2px(const pxmap *m, float y) { return m->t + y * (m->b - m->t) / UI_H; }

// ---- full dump
static void dump(uint64_t mgr) {
    char p[1200], praw[1200]; snprintf(p, sizeof p, "%s/ui-dump.tsv", g_dir); snprintf(praw, sizeof praw, "%s/ui-dump-raw.tsv", g_dir);
    int n = snapshot(mgr); pxmap m; int havem = px_map(&m);
    uint64_t focus = rq(mgr + 0x50), active = rq(mgr + 0x28), hover = rq(mgr + 0x20);
    FILE *f = fopen(p, "w"), *fr = fopen(praw, "w");
    if (!f) { LOG("【红】[d3ui] cannot write %s: errno %d", p, errno); if (fr) fclose(fr); return; }
    fprintf(f, "# D3 UI dump: %d controls, mgr %#llx, focus %016llx %s, active(+0x28) %016llx, +0x20 %016llx\n", n, (unsigned long long)mgr,
            (unsigned long long)focus, name_of(focus), (unsigned long long)active, (unsigned long long)hover);
    if (havem) fprintf(f, "# window %ux%u px; UI 960x720 -> px rect %.1f %.1f %.1f %.1f (%s)\n", m.w, m.h, m.l, m.t, m.r, m.b, m.from_game ? "main 0x34baf0" : "4:3 formula");
    fprintf(f, "id\tname\tvt\ttype\tflags\tvis\tshown\tparent\tlayer\tdepth\tsib\tact\tfh\tfocus\tui_l\tui_t\tui_r\tui_b\tpx_l\tpx_t\tpx_r\tpx_b\tfx_l\tfx_t\tfx_r\tfx_b\textra\n");
    int nv = 0;
    for (int i = 0; i < n; i++) {
        ctrl *c = &g_c[i]; int vis = c->flags >> 2 & 1;
        if (!vis && g_dump_mode < 2) continue;
        nv += vis;
        fprintf(f, "%016llx\t%s\t%#llx\t%s\t%#x\t%d\t%d\t%016llx\t%d\t%d\t%d\t%d\t%d\t%d", (unsigned long long)c->id, name_of(c->id), (unsigned long long)mo(c->vt),
                c->type, c->flags, vis, c->flags >> 1 & 1, (unsigned long long)c->parent, c->layer, c->depth, c->sib, c->act, c->fh, c->id == focus);
        if (c->has_rect) {
            fprintf(f, "\t%.1f\t%.1f\t%.1f\t%.1f", c->r[0], c->r[1], c->r[2], c->r[3]);
            if (havem) { float x0 = ux2px(&m, c->r[0]), y0 = uy2px(&m, c->r[1]), x1 = ux2px(&m, c->r[2]), y1 = uy2px(&m, c->r[3]);
                fprintf(f, "\t%.0f\t%.0f\t%.0f\t%.0f\t%.4f\t%.4f\t%.4f\t%.4f", x0, y0, x1, y1, x0 / m.w, y0 / m.h, x1 / m.w, y1 / m.h); }
            else fprintf(f, "\t\t\t\t\t\t\t\t");
        } else fprintf(f, "\t\t\t\t\t\t\t\t\t\t\t\t");
        fputc('\t', f);
        if (c->list) {
            int32_t sel = 0, first = 0, per = 0, cnt = 0; uint64_t items = rq(c->c + 0x210);
            rd(c->c + 0x1e0, &sel, 4); rd(c->c + 0x1e4, &first, 4); rd(c->c + 0x2a4, &per, 4); rd(c->c + 0x218, &cnt, 4);
            fprintf(f, "list sel=%d first=%d per=%d n=%d items=", sel, first, per, cnt);
            for (int k = 0; items && k < cnt && k < 64; k++) fprintf(f, "%s%016llx", k ? "," : "", (unsigned long long)rq(items + 8ull * k));
        }
        fputc('\n', f);
        if (fr && vis) {
            uint8_t raw[RAW_BYTES]; size_t got = RAW_BYTES;
            while (got >= 0x40 && !rd(c->c, raw, got)) got /= 2;
            if (got >= 0x40) { fprintf(fr, "%016llx\t%#llx\t", (unsigned long long)c->id, (unsigned long long)mo(c->vt)); for (size_t k = 0; k < got; k++) fprintf(fr, "%02x", raw[k]); fputc('\n', fr); }
        }
    }
    fclose(f); if (fr) fclose(fr);
    LOG("[d3ui] dump: %d controls (%d visible) -> %s, raw %s; focus %016llx %s", n, nv, p, praw, (unsigned long long)focus, name_of(focus));
}

// ---- tap queue (any thread -> game thread)
static _Atomic uint64_t g_tap_xy; static _Atomic int g_tap_pending;
int dm_d3_ui_tap(float x, float y);
int dm_d3_ui_tap(float x, float y) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_D3_UITAP"); mode = e && *e ? atoi(e) : 0; }   // before the first poll too
    if (g_ok == 0 || mode <= 0 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    atomic_store(&g_tap_xy, (uint64_t)yb << 32 | xb); atomic_store_explicit(&g_tap_pending, 1, memory_order_release);
    return 1;
}
static int g_a_wait, g_a_hold;   // polls before A, polls A is held
static void do_tap(uint64_t mgr, float fx, float fy) {
    pxmap m; if (!px_map(&m)) { LOG("[d3ui] tap %.3f,%.3f: no window size yet", fx, fy); return; }
    float px = fx * m.w, py = fy * m.h, ux = (px - m.l) * UI_W / (m.r - m.l), uy = (py - m.t) * UI_H / (m.b - m.t);
    int n = snapshot(mgr); ctrl *best = NULL, *anyhit = NULL;
    for (int i = 0; i < n; i++) {
        ctrl *c = &g_c[i];
        if (!(c->flags & 4) || !c->has_rect || ux < c->r[0] || ux >= c->r[2] || uy < c->r[1] || uy >= c->r[3]) continue;
        if (!anyhit || above(c, anyhit)) anyhit = c;
        if ((g_tap_mode >= 2 || c->act == 1 || c->fh || c->list) && (!best || above(c, best))) best = c;
    }
    if (!best) { LOG("[d3ui] tap %.3f,%.3f = UI %.0f,%.0f: no target (topmost visible %016llx %s)", fx, fy, ux, uy,
                     anyhit ? (unsigned long long)anyhit->id : 0ull, anyhit ? name_of(anyhit->id) : ""); return; }
    uint64_t id = best->id, before = rq(mgr + 0x50);
    ((void (*)(const uint64_t *))(g_base + O_SETFOCUS))(&id);
    LOG("[d3ui] tap %.3f,%.3f = UI %.0f,%.0f -> %016llx %s (type %s act %d fh %d list %d) focus %016llx -> %016llx, A next poll",
        fx, fy, ux, uy, (unsigned long long)id, name_of(id), best->type, best->act, best->fh, best->list, (unsigned long long)before, (unsigned long long)rq(mgr + 0x50));
    g_a_wait = 1; g_a_hold = 3;
}

// ---- per poll (hle_hid.c fill(), after the front end's buttons: the game's main thread, main 0x40810 -> 0x2b4230 -> 0xf84b0 -> 0xf7310)
void dm_d3_ui_pump(uint64_t *buttons);
void dm_d3_ui_pump(uint64_t *buttons) {
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    if (g_a_wait > 0) g_a_wait--;
    else if (g_a_hold > 0) { g_a_hold--; *buttons |= DM_BTN_A; }
    uint64_t mgr = ui_mgr();
    if (!mgr) return;
    if (g_tap_mode && atomic_exchange_explicit(&g_tap_pending, 0, memory_order_acquire)) {
        uint64_t xy = atomic_load(&g_tap_xy); float x, y; uint32_t xb = (uint32_t)xy, yb = (uint32_t)(xy >> 32); memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
        if (!g_a_hold) do_tap(mgr, x, y);
    }
    static unsigned tick; ++tick;
    if (g_tap_mode && tick % 30 == 15) {   // test path without the front end: dm/ui-tap-now holding "x y" (picture fractions)
        char t[1200]; snprintf(t, sizeof t, "%s/ui-tap-now", g_dir); FILE *f = fopen(t, "r");
        if (f) { float x = -1, y = -1; int ok = fscanf(f, "%f %f", &x, &y) == 2; fclose(f); remove(t);
                 if (!ok || !dm_d3_ui_tap(x, y)) LOG("[d3ui] %s: want \"x y\" in 0..1", t); }
    }
    if (!atomic_load(&g_names_state)) {   // names for the focus / tap / dump lines, read off the game thread
        atomic_store(&g_names_state, 1); pthread_t t;
        if (!pthread_create(&t, NULL, names_load, NULL)) pthread_detach(t); else atomic_store(&g_names_state, 0);
    }
    if (!g_dump_mode && !g_crumbs) return;
    static uint64_t last_focus = 1, last_active = 1;
    uint64_t focus = rq(mgr + 0x50), active = rq(mgr + 0x28);
    if (focus != last_focus || active != last_active) {
        if (g_dump_mode) LOG("[d3ui] focus %016llx %s | active %016llx %s", (unsigned long long)focus, name_of(focus), (unsigned long long)active, name_of(active));
        else {   // [D3] breadcrumb: rate-limited to 8 lines a second; changes past that are counted into the next line
            static uint64_t win_t; static unsigned win_n, skipped; uint64_t now = now_ns();
            if (now - win_t > 1000000000ull) { win_t = now; win_n = 0; }
            if (win_n++ < 8) {
                LOG("[d3ui] crumb %.1f s: focus %016llx %s | active %016llx %s%s", (now - g_t0_ns) / 1e9, (unsigned long long)focus, name_of(focus),
                    (unsigned long long)active, name_of(active), skipped ? " (more changes before this one were not logged)" : "");
                skipped = 0;
            } else skipped++;
        }
        last_focus = focus; last_active = active;
    }
    if (!g_dump_mode) return;
    if (tick % 30 == 0) {
        char t[1200]; snprintf(t, sizeof t, "%s/ui-dump-now", g_dir);
        if (!access(t, F_OK)) { remove(t); dump(mgr); }
    }
}
#endif
