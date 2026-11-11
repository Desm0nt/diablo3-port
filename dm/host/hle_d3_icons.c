// [D3] Icons of the HUD skill slots for the on-screen buttons (D3PadView, gear "Skill icons on buttons"), read from the game's own UI
// tree instead of from pixels of letters (the old D3SkillIcons PixelCopy + letter template matching is gone; docs/d3/TOUCH_DESIGN.md §9).
// Where it lives (update 2.7.7 main, build 2607a74f; offsets = main.raw, every site checked word for word at the first poll):
//   HUD player frames: main+0x187ac90 + frame * 0x4d8, frame 0..3 = ConsoleHUD_main.LayoutRoot.PlayerSlots.Player1..4.Local (init 0x41cd40 ->
//   0x415220 builds the UIIDs; the per-frame update 0x41af60 picks the frame of a player as 0x187ac78 + 0x18 + 0x4d8 * 0x416070(player)):
//     +0x00 the frame's ".Local" UIID (shown only for a local player: the one whose HUD is on screen);
//     +0x98 .PotionIcon (skill-icon class, type 0x21), +0xa0 .PotionButton (text: the potion's button glyph, 0x407020), +0xb8
//       .PotionCooldownCover (shown while the potion cools down; the potion update 0x418300);
//     +0x100 + 0x30 * i, i = 0..5: u32 the slot's action index (<= 7), +0x108 the UIID of .SkillsParent.Skills.Slot<i+1>. The slot
//       update 0x417ae0 (called from 0x41b648) hides a slot that has no power (locked / nothing assigned: 0x520ed0 gives -1) and for a
//       shown one sets Slot.Skill (type 0x21: its power SNO at +0x4fc, 0x39aba0), shows Slot.SkillCooldownCover while 0x39a620(Skill) > 0
//       (cooldown left), and writes Slot.KeyBinding's text = 0x4075c0(0x407650(action index, player), player, 1) = 0x4070c0:
//       "{icon:button1_label}" .. "{icon:r2_label}" for the button the action is bound to (game Options -> Controller remaps follow).
//   Button codes (0x4070c0 switch, also the published code): 0 button1 = A, 1 button2 = B, 2 button3 = X, 3 button4 = Y, 4 l1 = L, 5 r1 = R,
//   6 l2 = ZL, 7 r2 = ZR, 8..11 d-pad, 12 back (-), 13 start (+), 14 L3, 15 R3. Unreadable text: the default binding tables main+0xeb3b60
//   (action -> binding) and +0xebbfb4 (binding -> code) as 0x407650 / 0x4075c0 use them without a custom layout; the potion falls back to R.
// Rects: Slot.Skill / PotionIcon +0x88 in 960x720 UI units (the HUD sits left of the 4:3 area: negative x), mapped to window fractions
// with the game's own 4:3 pixel region (hle_d3_ui.c dm_d3_ui_window_map, main 0x34baf0).
// The pixels: what the game drew. While the HUD is on screen the presenter (vulkan_present.cpp, translation thread) copies the icon
// rectangles of the slots that changed (or are due a refresh: every 5 s, never while their cooldown cover is up) out of the presented frame
// into a host buffer (vkCmdCopyImageToBuffer in its own command buffer, read back after that frame's fence: no extra GPU sync), at most
// 4 times a second; they are scaled to 64 x 64 ARGB and published with a serial for Java (JNI nativeD3IconSerial / nativeD3Icons).
// No crop while anything may cover the HUD: a console menu (hle_d3_ui.c menu-open: front end, screen group, focus, modal, inventory,
// legend), a loading screen (st = *(*(g + 0x10) + 0x800), st+0x6c, as hle_d3.c), or no local HUD frame shown (cut-scene, death); then
// 600 ms after the last cover (fade-outs), only rects that held still for 3 polls, and 500 ms after a cooldown cover went away (its white
// flash). A batch whose frame was presented across a cover is thrown away (cover generation); icons already published stay.
//   DM_D3_ICONS=0 turns it off (default on: it only reads; a wrong rectangle can only give a wrong picture on a touch button).
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int dm_d3_ui_read(uint64_t a, void *out, size_t n);
uint64_t dm_d3_ui_control(uint64_t mgr, uint64_t id);
uint64_t dm_d3_ui_child_id(uint64_t parent, const char *name);
int dm_d3_ui_window_map(float m[6]);

enum {
    F_BASE = 0x187ac90, F_STRIDE = 0x4d8, F_N = 4,
    F_POT_ICON = 0x98, F_POT_BTN = 0xa0, F_POT_COOL = 0xb8, F_SLOT_IDX = 0x100, F_SLOT_ID = 0x108, F_SLOT = 0x30, NSKILL = 6,
    C_FLAGS = 0x18, C_PARENT = 0x38, C_RECT = 0x88, C_TEXT = 0x398, C_POWER = 0x4fc,
    O_UPDATERECT = 0x3b31f0, O_G = 0x1905610, O_BIND_ACT = 0xeb3b60, O_BIND_BTN = 0xebbfb4,
    S_SKILL = 0xe1b166, S_COOL = 0xe5cd9d, S_KEY = 0xe56055,
    NSLOT = 7, ICON = 64, MAXPX = 160, POTION_DEFAULT = 5,
};
static const struct { uint32_t off, n, w[5]; } k_sites[] = {
    {0x41afd4, 5, {0x9000a319, 0x9131e339, 0x52809b08, 0x9b286417, 0x910062f4}},   // frame = 0x187ac78 + 0x4d8 * n (smaddl), + 0x18
    {0x41cdc4, 5, {0xd000a2f3, 0x91322273, 0x91002260, 0x910023e1, 0x97ffe113}},   // 0x415220(main+0x187ac90, "...Player1.Local")
    {0x418324, 1, {0x91026019}}, {0x41833c, 1, {0x91028278}}, {0x41836c, 1, {0x9102e275}},   // potion +0x98 icon, +0xa0 button, +0xb8 cooldown cover
    {0x417b4c, 1, {0x9104a2d6}}, {0x417b58, 1, {0xd10082d8}}, {0x417b68, 1, {0xb89d82c8}},   // slots: +0x128 - 0x20 = UIID +0x108, - 0x28 = index +0x100
    {0x417c34, 2, {0x90005021, 0x91059821}},   // ".Skill"
    {0x417c9c, 2, {0xb0005221, 0x91367421}},   // ".SkillCooldownCover"
    {0x417ea0, 1, {0xb944ff21}},               // ldr w1, [Skill, #0x4fc] (power)
    {0x417ff0, 2, {0xf00051e1, 0x91015421}},   // ".KeyBinding"
    {0x41801c, 1, {0x97ffbd8d}}, {0x41802c, 1, {0x97ffbd65}},   // bl 0x407650 (binding of the action), bl 0x4075c0 (its glyph text)
    {0x41b63c, 1, {0x97fff331}}, {0x41b648, 1, {0x97fff126}},   // the frame update calls the potion 0x418300 and slot 0x417ae0 updates
    {0x3ebe0, 5, {0xf9442108, 0xf9400908, 0xf9440115, 0x97ff2f81, 0xb9406ea8}},   // loading screen st = *(*(g + 0x10) + 0x800), +0x6c
};
static const struct { uint32_t off; const char *s; } k_strs[] = {{S_SKILL, ".Skill"}, {S_COOL, ".SkillCooldownCover"}, {S_KEY, ".KeyBinding"}};
static const int32_t k_bind_act[8] = {0, 4, 2, 3, -1, -1, 5, 1}, k_bind_btn[6] = {0, 7, 2, 3, 1, 6};
static const char *const k_btn_name[16] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "up", "down", "left", "right", "-", "+", "L3", "R3"};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1; static uint64_t g_mgr;
static uint64_t ma(uint64_t off) { return (uint64_t)(uintptr_t)g_base + off; }
static int rd(uint64_t a, void *o, size_t n) { return dm_d3_ui_read(a, o, n); }
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static int32_t ri(uint64_t a) { int32_t v = 0; return rd(a, &v, 4) ? v : 0; }
static uint64_t ctl(uint64_t id) { return id && id != ~0ull ? dm_d3_ui_control(g_mgr, id) : 0; }
static int shown(uint64_t c) { uint16_t f = 0; return c && rd(c + C_FLAGS, &f, 2) && (f & 4); }
// the control and every ancestor shown (+0x18 bit 2, what vtable+0x50 sets); the walk ends at a layer (Root.*Layer = UIID 1..3)
static int drawn(uint64_t c) {
    for (int d = 0; c && d < 48; d++) {
        if (!shown(c)) return 0;
        uint64_t p = rq(c + C_PARENT);
        if (!p || p == ~0ull || p <= 3) return 1;
        c = ctl(p);
        if (!c) return 1;   // an ancestor outside the control map: what was seen is shown
    }
    return c != 0;
}
static int rect_of(uint64_t c, float r[4]) {   // +0x88 l, t, r, b (UI units) of a control class that keeps a rect (vtable+0xb0 = 0x3b31f0)
    uint64_t vt = c ? rq(c) : 0;
    return vt && rq(vt + 0xb0) == ma(O_UPDATERECT) && rd(c + C_RECT, r, 16) && isfinite(r[0]) && isfinite(r[1]) && isfinite(r[2]) && isfinite(r[3]) &&
           r[2] > r[0] + 4 && r[3] > r[1] + 4 && r[2] - r[0] < 400 && r[3] - r[1] < 400;
}
static int isa(uint64_t c, unsigned type) {   // vtable+0x88 IsA: one of its "cmp w1, #imm" (as hle_d3_ui.c isa_types)
    uint64_t vt = c ? rq(c) : 0, f = vt ? rq(vt + 0x88) : 0; uint32_t w[16];
    if (!f || f < ma(0) || f >= ma(g_size) || !rd(f, w, sizeof w)) return 0;
    for (int i = 0; i < 16 && w[i] != 0xd65f03c0 && (w[i] & 0xfc000000) != 0x14000000; i++) if ((w[i] & 0xffc003ff) == 0x7100003f && (w[i] >> 10 & 0xfff) == type) return 1;
    return 0;
}
static int icon_code(const char *n, size_t l) {
    static const struct { const char *n; int b; } k[] = {{"button1", 0}, {"button2", 1}, {"button3", 2}, {"button4", 3}, {"l1", 4}, {"r1", 5}, {"l2", 6},
        {"r2", 7}, {"up", 8}, {"down", 9}, {"left", 10}, {"right", 11}, {"back", 12}, {"minus", 12}, {"start", 13}, {"plus", 13}, {"l3", 14}, {"r3", 15}};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (strlen(k[i].n) == l && !strncasecmp(n, k[i].n, l)) return k[i].b;
    return -1;
}
static int text_button(uint64_t c, char *seen, size_t seen_cap) {   // the button of the first "{icon:<name>[_label]...}" in a text control (+0x398)
    uint64_t tp = c ? rq(c + C_TEXT) : 0; char t[96]; size_t got = sizeof t - 1;
    if (seen_cap) seen[0] = 0;
    if (!tp) return -1;
    while (got >= 8 && !rd(tp, t, got)) got /= 2;   // the string may end near the end of readable memory
    if (got < 8) return -1;
    t[got] = 0; t[strcspn(t, "\r\n")] = 0;
    if (seen_cap) { strncpy(seen, t, seen_cap - 1); seen[seen_cap - 1] = 0; }
    const char *q = strstr(t, "{icon:"); if (!q) return -1;
    q += 6; size_t l = strcspn(q, ",}");
    if (l > 6 && !strncasecmp(q + l - 6, "_label", 6)) l -= 6;
    return icon_code(q, l);
}

static _Atomic int g_cover_gen, g_covered;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int n, idx[NSLOT]; float r[NSLOT][4]; float ww, wh; int64_t hash[NSLOT]; uint64_t t; int taken, used[NSLOT], nused, gen; } g_req;   // under g_mu
static int64_t g_have[NSLOT]; static uint64_t g_got_t[NSLOT];
static uint32_t g_px[NSLOT][ICON * ICON]; static int g_meta[NSLOT][4]; static _Atomic int g_serial;   // published (under g_mu)

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_D3_ICONS");
    if (e && *e == '0') { LOG("[d3icons] DM_D3_ICONS=0: off"); return 0; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < F_BASE + F_N * F_STRIDE) { LOG("【红】[d3icons] no main module: button icons off"); return 0; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, 4u * k_sites[i].n)) {
            LOG("【红】[d3icons] main+%#x differs: not D3 update 2.7.7 (build 2607a74f), button icons off", k_sites[i].off); return 0; }
    for (size_t i = 0; i < sizeof k_strs / sizeof *k_strs; i++)
        if (memcmp(g_base + k_strs[i].off, k_strs[i].s, strlen(k_strs[i].s) + 1)) { LOG("【红】[d3icons] string main+%#x is not \"%s\": button icons off", k_strs[i].off, k_strs[i].s); return 0; }
    if (memcmp(g_base + O_BIND_ACT, k_bind_act, sizeof k_bind_act) || memcmp(g_base + O_BIND_BTN, k_bind_btn, sizeof k_bind_btn)) {
        LOG("【红】[d3icons] default binding tables main+%#x / +%#x differ: button icons off", O_BIND_ACT, O_BIND_BTN); return 0; }
    for (int i = 0; i < NSLOT; i++) { g_have[i] = 0x7fffffffffffffffll; g_meta[i][0] = -1; g_meta[i][3] = i == 0; }
    LOG("[d3icons] on: HUD frames main+%#x (x %#x): potion +0x98 / +0xa0 / +0xb8, skill slots +0x100 x 0x30 (Slot.Skill / .KeyBinding / "
        ".SkillCooldownCover), icons cropped from the presented frame", F_BASE, F_STRIDE);
    return g_ok = 1;
}
static int loading_now(void) {   // hle_d3.c: st = *(*(g + 0x10) + 0x800), st+0x6c = loading screen up
    uint64_t o = rq(ma(O_G) + 0x10), st = o ? rq(o + 0x800) : 0;
    return st && ri(st + 0x6c);
}

// ---- game thread, every npad poll (hle_d3_ui.c dm_d3_ui_pump, after the menu-open signal)
void dm_d3_icons_poll(uint64_t mgr, int menu_open);
void dm_d3_icons_poll(uint64_t mgr, int menu_open) {
    if (g_ok < 0) setup();
    if (g_ok <= 0 || !mgr) return;
    g_mgr = mgr;
    static uint64_t next, clear_since; static int last_why = -1, frame = -1; uint64_t now = now_ns();
    int why = menu_open ? 1 : loading_now() ? 2 : 0;
    uint64_t F = 0;
    if (!why) {   // the local player's frame: the first one whose ".Local" is shown (single player: Player1)
        int fk = -1;
        for (int k = 0; k < F_N && fk < 0; k++) if (drawn(ctl(rq(ma(F_BASE) + (uint64_t)k * F_STRIDE)))) fk = k;
        if (fk < 0) why = 3; else F = ma(F_BASE) + (uint64_t)fk * F_STRIDE;
        if (fk >= 0 && fk != frame) { LOG("[d3icons] HUD frame %d (Player%d.Local %016llx)", fk, fk + 1, (unsigned long long)rq(F)); frame = fk; }
    }
    if (why != last_why) { static int told; if (told++ < 100) LOG("[d3icons] crop %s (%s)", why ? "paused" : "allowed again in 600 ms",
        (const char *const[]){"HUD shown", "console menu open", "loading screen", "no local HUD frame shown"}[why]); last_why = why; }
    if (why) {
        if (!atomic_load(&g_covered)) atomic_store(&g_covered, 1);
        atomic_fetch_add(&g_cover_gen, 1); clear_since = 0;
        pthread_mutex_lock(&g_mu); if (!g_req.taken) g_req.n = 0; pthread_mutex_unlock(&g_mu);   // a waiting request would crop the cover
        return;   // published icons and states stay as they are
    }
    atomic_store(&g_covered, 0); if (!clear_since) clear_since = now;
    if (now < next) return;
    next = now + 50000000ull;   // 20 Hz is plenty
    int settled = now - clear_since >= 600000000ull;
    float m[6]; if (!dm_d3_ui_window_map(m) || m[4] < 64 || m[5] < 64 || m[2] - m[0] < 16) return;
    int want[NSLOT], nw = 0, published_change = 0;
    static float cur_r[NSLOT][4]; static int64_t cur_hash[NSLOT];
    for (int i = 0; i < NSLOT; i++) {
        uint64_t icon, cool, keyc, slot = 0; int code, empty = 0; int32_t act = -1; char txt[64];
        if (i == 0) { icon = ctl(rq(F + F_POT_ICON)); keyc = ctl(rq(F + F_POT_BTN)); cool = ctl(rq(F + F_POT_COOL)); }
        else {
            uint64_t sid = rq(F + F_SLOT_ID + (uint64_t)(i - 1) * F_SLOT); act = ri(F + F_SLOT_IDX + (uint64_t)(i - 1) * F_SLOT);
            slot = ctl(sid); empty = !shown(slot);   // 0x417ae0 hides a slot without a power (locked / nothing assigned)
            icon = ctl(dm_d3_ui_child_id(sid, "Skill")); keyc = ctl(dm_d3_ui_child_id(sid, "KeyBinding")); cool = ctl(dm_d3_ui_child_id(sid, "SkillCooldownCover"));
        }
        code = text_button(keyc, txt, sizeof txt); int from_text = code >= 0;
        if (code < 0) code = i == 0 ? POTION_DEFAULT : act >= 0 && act < 8 && k_bind_act[act] >= 0 ? k_bind_btn[k_bind_act[act]] : -1;
        int64_t hash = icon && isa(icon, 0x21) ? (int64_t)ri(icon + C_POWER) : 0;
        float u[4] = {0}; int vis = 0;
        if (!empty) {
            if (rect_of(icon, u)) vis = drawn(icon);
            else if (rect_of(i ? slot : keyc, u) && drawn(i ? slot : keyc)) {
                // from the slot's own rect (46 wide: the icon is the 34 x 34 square 6 units in from its top / sides) or the potion button's
                // (46 x 72, the letter below: the icon centre 11 units below its top), as the in-game UI dump shows them
                float s = (u[2] - u[0]) * 34.f / 46.f, cx = (u[0] + u[2]) * 0.5f, t = i ? u[1] + (u[2] - u[0]) * 6.f / 46.f : u[1] + (u[2] - u[0]) * 11.f / 46.f - s * 0.5f;
                u[0] = cx - s * 0.5f; u[2] = cx + s * 0.5f; u[1] = t; u[3] = t + s; vis = 1;
            }
        }
        float r[4] = {(m[0] + u[0] * (m[2] - m[0]) / 960.f) / m[4], (m[1] + u[1] * (m[3] - m[1]) / 720.f) / m[5],
                      (m[0] + u[2] * (m[2] - m[0]) / 960.f) / m[4], (m[1] + u[3] * (m[3] - m[1]) / 720.f) / m[5]};
        if (vis && !(r[0] >= 0 && r[1] >= 0 && r[2] <= 1 && r[3] <= 1)) vis = 0;   // off the window
        // cooldown: the cover is up; and 500 ms after it went away (the white flash 0x4c8120 starts then)
        static uint64_t cool_quiet[NSLOT]; static int was_cool[NSLOT];
        int c_now = !empty && shown(cool);
        if (was_cool[i] && !c_now) cool_quiet[i] = now + 500000000ull;
        was_cool[i] = c_now;
        int c_eff = c_now || now < cool_quiet[i];
        // the HUD slides / scales in: only a rectangle that stayed put for 3 polls (150 ms)
        static float last_r[NSLOT][4]; static int same[NSLOT];
        if (memcmp(last_r[i], r, 16)) { memcpy(last_r[i], r, 16); same[i] = 0; } else if (same[i] < 100) same[i]++;
        if (code != g_meta[i][0] || empty != (g_meta[i][1] == 2) || hash != cur_hash[i]) {
            static int told; if (told++ < 80) LOG("[d3icons] %s: button %s (%s \"%s\"), power %#llx, %s, UI %.0f,%.0f..%.0f,%.0f -> %.4f,%.4f..%.4f,%.4f",
                i ? (const char *const[]){"", "Slot1", "Slot2", "Slot3", "Slot4", "Slot5", "Slot6"}[i] : "potion", code >= 0 && code < 16 ? k_btn_name[code] : "?",
                from_text ? "KeyBinding text" : "default binding", txt, (unsigned long long)hash, empty ? "empty (hidden)" : vis ? "shown" : "not drawn",
                u[0], u[1], u[2], u[3], r[0], r[1], r[2], r[3]);
            cur_hash[i] = hash;
        }
        memcpy(cur_r[i], r, 16);
        pthread_mutex_lock(&g_mu);
        if (g_meta[i][0] != code) { g_meta[i][0] = code; published_change = 1; }
        if (empty && g_meta[i][1] != 2) { g_meta[i][1] = 2; g_have[i] = 0x7fffffffffffffffll; published_change = 1; }   // no pixels
        if (!empty && g_meta[i][1] == 2) { g_meta[i][1] = 0; published_change = 1; }   // unlocked / assigned again: wait for the crop
        if (g_meta[i][2] != c_now) { g_meta[i][2] = c_now; published_change = 1; }
        pthread_mutex_unlock(&g_mu);
        if (empty || !vis || c_eff || same[i] < 3 || !settled || code < 0) continue;
        if (hash != g_have[i] || now - g_got_t[i] > 5000000000ull) want[nw++] = i;
    }
    if (published_change) atomic_fetch_add(&g_serial, 1);
    if (!nw) return;
    pthread_mutex_lock(&g_mu);
    if (!g_req.n || (g_req.taken && now - g_req.t > 1000000000ull) || (!g_req.taken && now - g_req.t > 250000000ull)) {
        g_req.n = nw; g_req.ww = m[4]; g_req.wh = m[5]; g_req.t = now; g_req.taken = 0; g_req.gen = atomic_load(&g_cover_gen);
        for (int k = 0; k < nw; k++) { g_req.idx[k] = want[k]; memcpy(g_req.r[k], cur_r[want[k]], 16); g_req.hash[k] = cur_hash[want[k]]; }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- translation thread (vulkan_present.cpp present()): rectangles to copy, in source pixels of the presented picture (x, y, w, h);
// 0 = nothing this frame. At most one batch per 250 ms; none while the picture is not the whole window (the loading-screen crop of hle_d3.c).
static int icon_want(int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t out[][4], int max) {
    if (g_ok <= 0 || pw <= 0 || ph <= 0) return 0;
    static uint64_t last; uint64_t now = now_ns(); int n = 0;
    pthread_mutex_lock(&g_mu);
    if (g_req.n && !g_req.taken && (atomic_load(&g_covered) || g_req.gen != atomic_load(&g_cover_gen))) g_req.n = 0;   // covered since
    if (g_req.n && !g_req.taken && fabsf((float)pw * g_req.wh - (float)ph * g_req.ww) > 0.01f * (float)pw * g_req.wh) {
        static int said; if (said++ < 5) LOG("[d3icons] presented picture %dx%d is not the %.0fx%.0f window: no crop", pw, ph, g_req.ww, g_req.wh);
        g_req.n = 0;
    }
    if (g_req.n && !g_req.taken && now - last >= 250000000ull) {
        for (int k = 0; k < g_req.n && n < max; k++) {
            const float *r = g_req.r[k];
            int x0 = px + (int)(r[0] * pw + .5f), y0 = py + (int)(r[1] * ph + .5f), x1 = px + (int)(r[2] * pw + .5f), y1 = py + (int)(r[3] * ph + .5f);
            if (x0 < px) x0 = px; if (y0 < py) y0 = py; if (x1 > px + pw) x1 = px + pw; if (y1 > py + ph) y1 = py + ph;
            int w = x1 - x0, h = y1 - y0;
            if (w < 4 || h < 4 || w > MAXPX || h > MAXPX) continue;
            out[n][0] = x0; out[n][1] = y0; out[n][2] = w; out[n][3] = h; g_req.used[n] = k; n++;
        }
        g_req.taken = 1; g_req.nused = n; last = now; if (!n) g_req.n = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
// the copied pixels (tightly packed rows, 4 bytes, RGBA or BGRA), in the order icon_want gave the rectangles
static void icon_done(int n, const int32_t rect[][4], const uint8_t *pix, int bgra) {
    if (g_ok <= 0) return;
    pthread_mutex_lock(&g_mu);
    int m = g_req.taken ? g_req.nused : 0, k = 0;
    if (m && (atomic_load(&g_covered) || g_req.gen != atomic_load(&g_cover_gen))) m = 0;   // a cover came between the request and this frame
    for (; k < m && k < n; k++) {
        int j = g_req.used[k], i = g_req.idx[j], w = rect[k][2], h = rect[k][3];
        uint32_t *o = g_px[i];
        for (int y = 0; y < ICON; y++) for (int x = 0; x < ICON; x++) {   // nearest of the centre (icons are 40..60 px)
            int sx = (x * w + w / 2) / ICON, sy = (y * h + h / 2) / ICON; const uint8_t *p = pix + ((size_t)sy * w + sx) * 4;
            uint8_t R = bgra ? p[2] : p[0], G = p[1], B = bgra ? p[0] : p[2];
            o[y * ICON + x] = 0xff000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | B;
        }
        pix += (size_t)w * h * 4;
        if (g_meta[i][1] != 2) g_meta[i][1] = 1;
        g_have[i] = g_req.hash[j]; g_got_t[i] = now_ns();
    }
    if (k) { static int said; if (said++ < 20) LOG("[d3icons] cropped %d icon(s) from the presented frame (first %dx%d px)", k, rect[0][2], rect[0][3]); }
    g_req.n = 0; g_req.taken = 0;
    pthread_mutex_unlock(&g_mu);
    if (k) atomic_fetch_add(&g_serial, 1);
}
int (*dm_present_icon_want)(int32_t, int32_t, int32_t, int32_t, int32_t[][4], int) = icon_want;
void (*dm_present_icon_done)(int, const int32_t[][4], const uint8_t *, int) = icon_done;

// ---- Java (any thread)
int dm_d3_icon_serial(void);
int dm_d3_icon_serial(void) { return g_ok > 0 ? atomic_load(&g_serial) : -1; }
// px: NSLOT * 64 * 64 ARGB, meta: NSLOT * 4 (button code 0 A .. 7 ZR (see the header) or -1, state 0 unknown / 1 icon / 2 empty or locked,
// cooldown cover up 0 / 1, 1 = the potion slot); slot 0 = potion, 1..6 = Slot1..6. Returns the serial.
int dm_d3_icons(uint32_t *px, int npx, int *meta, int nmeta);
int dm_d3_icons(uint32_t *px, int npx, int *meta, int nmeta) {
    if (g_ok <= 0) return -1;
    pthread_mutex_lock(&g_mu);
    if (px && npx >= NSLOT * ICON * ICON) memcpy(px, g_px, sizeof g_px);
    if (meta && nmeta >= NSLOT * 4) memcpy(meta, g_meta, sizeof g_meta);
    int s = atomic_load(&g_serial);
    pthread_mutex_unlock(&g_mu);
    return s;
}
#endif
