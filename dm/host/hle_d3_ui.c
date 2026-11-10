// [D3] Touch phase 3 (docs/d3/TOUCH_DESIGN.md §7, §8): the game's console UI seen from the host, and a tap that works the touched element through
// the game's own UI code (no pad buttons, no d-pad / stick steps). Everything runs on the game's main thread inside the npad poll.
//   DM_D3_UITAP=1    (default; 0 = off, 2 = dry run: resolve and log only) dm_d3_ui_tap(x, y) (picture fractions, any thread) queues a tap; the next
//                    npad poll resolves the element under the point and acts like the game does for it:
//                      list row        -> the list's own SetSelected 0x3856e0 (what its Up / Down code calls, scrolls, fires its +0x240 callback)
//                                         + its +0x208 sound; a tap on the selected row (or any row of a horizontal list) = accept;
//                      checkbox / value selector in a selected row -> the widget's own message handler (BUTTON_1) / step 0x23b600 / 0x23b6f0;
//                      ring segment    -> the ring helper's own select (0x38a110 cursor + 0x38a4d0 index + the screen's callback), as the stick
//                                         path 0x38a270 does; a tap on the selected segment = accept;
//                      tab header      -> InventoryNav-style Glow1..5 = the console screen group's own open-screen 0x16a340(group, index);
//                                         KeyBinding1 / 2 = Console_PrevScreen / Console_NextScreen (what L / R are bound to);
//                      accept          -> the UI's logical BUTTON_1 press event {code 0x1d, player, button 0} handed to the UI input dispatcher
//                                         0x32c490 (focus -> its HandleMessage, else the registered input listeners), the same entry the game's
//                                         event queue calls; never the pad state.
//                      skills icons / category arrows -> the skills screen's selection group 0x23ace0 / its own step 0x37b5f0;
//                      button legend entry -> that console button's logical press through the same dispatcher (A, B, X, Y, ...);
//                      rings of achievements / banner / season journey / leaderboards / equipment manager (their own 0x389d40 callbacks);
//                      waypoint / big map: waypoint (or act) = the stick selection's own effect (+0x44, flyout 0x239f60, legend refresh),
//                        the selected one = accept (UI_WaypointMapAccept); nothing else on the big map = UI_MapClose;
//                      hero create: hero type / gender radio children -> HeroCreate_SetHeroType 0x34de90 / SetGender 0x3514c0;
//                      button legend: the entry under the finger from the control's own text hit test 0x3a9390 (real glyph metrics);
//                    Unknown targets are logged (topmost control + class) and left alone.
//                    No menu open: a tap on the HUD minimap (dm_d3_ui_minimap_hit) = UI_MapOpen, the big map.
//   dm_d3_ui_scroll(x, y, dy) (UI thread): a swipe over a visible list queues the travel; the poll moves the list's selection past its edge
//                    one row per row height (the game scrolls lists only through the selection), which scrolls it. 0 = no list there.
//                    Test without the front end: a file dm/ui-tap-now holding "x y" queues the same tap (checked every 30 polls).
//   dm_d3_ui_menu_open() (published every poll): front end (title / main menu / hero select), a console screen group open (pause, the "+"
//                    hero menu and its tabs), a focused control or a modal one. Plain gameplay = 0.
//   DM_D3_UIDUMP=1   log every change of the UI focus / active control / menu-open; the file dm/ui-dump-now writes dm/ui-dump.tsv (visible
//                    controls with class, rect, list state, input handler table, ...; header: game state, screen group, input listeners, rings)
//                    and dm/ui-dump-raw.tsv (first 0x400 bytes of each visible control). =2: every control. Names from dm/ui-names.tsv,
//                    handler names from dm/ui-callbacks.tsv (tools/d3_ui_callbacks.py) when present.
//   DM_D3_UI_CRUMBS=1 (default; 0 = off) breadcrumbs for crash reports: focus / active changes, at most 8 lines a second.
// Everything is read through rd(): guest heap directly, main's image directly, anything else through process_vm_readv (a bad pointer reads as
// "absent", never faults). Addresses: update 2.7.7 main, build-id 2607a74f; every called entry point is checked word for word first.
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
    // classes (vtable offsets in main; each checked through its vtable+0x60 HandleMessage slot)
    O_VT_LIST     = 0x10a65f0,   // list (IsA 51): +0x1e0 selected, +0x1e4 first row, +0x1e8 rows shown, +0x208 move sound, +0x210 item UIIDs,
                                 //   +0x218 item count, +0x240 selection-changed callback, +0x258 data source, +0x2a0 layout (0 vertical, 1 grid,
                                 //   2 horizontal), +0x2a4 per row; HandleMessage 0x387530
    O_VT_CHECK    = 0x10a6cc0,   // checkbox: HandleMessage 0x38f530 -> 0x38f130 (BUTTON_1 press = sound +0x1d8 + toggle 0x38e790)
    O_VT_SELECTOR = 0x10a0900,   // value selector "< value >" (IsA 57): HandleMessage 0x23b310, D-pad Left / Right -> 0x23b600 / 0x23b6f0 (sel, 1)
    O_VT_BUTTON   = 0x10a9520,   // text button (IsA 4)
    O_VT_INVBASE  = 0x10a5ee0,   // console inventory screen (ConsoleInventoryRing), vtable+0x28 = the inventory-family Update 0x1f7590
    O_LIST_HM = 0x387530, O_CHECK_HM = 0x38f530, O_SELECTOR_HM = 0x23b310,
    // functions called (all checked)
    O_SETSEL      = 0x3856e0,    // void List_SetSelected(list *, int index)
    O_DISPATCH    = 0x32c490,    // int UI_DispatchInput(const event[0x48]): pad events -> focus (UI_SendMessage) -> input listeners (mgr+0x10)
    O_SENDMSG     = 0x32cd60,    // int UI_SendMessage(const UIID *, const int *kind, const event *): the control's vtable+0x60 HandleMessage
    O_SOUND       = 0x315e20,    // void UI_PlaySound(int id, const void *params); params = *(GOT 0x114b760) = main+0xeb9e10
    O_GOT_SNDP = 0x114b760, O_SNDP = 0xeb9e10,
    O_RING_ROT    = 0x38a110,    // void Ring_PointCursor(ring *, const UIID *segment)
    O_RING_SEL    = 0x38a4d0,    // void Ring_Select(ring *, const UIID *segment, void (*changed)(ring *, const UIID *, int))
    O_INV_UPDATE  = 0x1f7590,    // inventory-family Update (ring helper at obj+0xd0, its callbacks 0x1f76c0 / 0x1f77a0)
    O_INV_RINGCB  = 0x1f76c0, O_INV_RINGOK = 0x1f77a0,
    O_SKL_RINGCB  = 0x37f8a0,    // skills ring callback (0x37f7c0: ring obj+0x168 while obj+0x1f8 == 0)
    O_PAR_RINGCB  = 0x213b40,    // paragon (0x2139f4: ring obj+8 while obj+0xd8 == 0)
    O_JRN_RINGCB  = 0x2080c0,    // journal / quests (0x2091ac: ring obj+0x2860 while obj+0x2858 && !obj+0x2944)
    O_PTY_RINGCB  = 0x21bd40,    // party (0x21d0d8: ring obj+0x318 while obj+0x18 == 2)
    O_OPENSCREEN  = 0x16a340,    // int Console_OpenScreen(group, index, player, immediate): what Console_Pause / the L / R transition use
    O_PREVSCREEN  = 0x3c2a50,    // Console_PrevScreen(const UIID *, player) (named callback; L)
    O_NEXTSCREEN  = 0x3c2a90,    // Console_NextScreen (R)
    O_SEL_PREV    = 0x23b600, O_SEL_NEXT = 0x23b6f0,   // selector step (sel, 1): value, sound +0x1e0, change callback +0x228
    O_INGAME      = 0x4c7df0,    // int: 1 while no game is loaded (front end), 0 in game
    O_SCREENS     = 0x17e5298,   // (GOT 0x114d080) -> console screen manager: [0..0x20] groups, +0x108 current group (0 = none)
    O_GOT_SCREENS = 0x114d080,
    // screen objects (bss pointers, from the screens' own handlers)
    V_SKILLS = 0x186dc40, V_PARAGON = 0x17f0a78, V_JOURNAL = 0x17ee838, V_PARTY = 0x17fedc8,
    V_SKILLS_ROOT = 0x185fc40,   // UIID var: Root.NormalLayer.ConsoleSkillSelection_main (UISkills_On* run only while it is shown)
    O_GRP_SET     = 0x23ace0,    // int SelGroup_Set(group *, index, notify): +8 item UIIDs, +0x10 count, +0x20 selected, +0x28 may-select, +0x30 changed
    O_SKL_STEP    = 0x37b5f0,    // void Skills_Step(skills *, 0 left / 1 right): D-pad / stick left-right of UISkills; past the edge pages the category (+0x6c0)
    O_TEXT_HIT    = 0x3a9390,    // int Text_HitChar(textctrl *, const float pt[2] UI units): text index under a point, real glyph metrics (the
                                 //   mouse text-selection path 0x3aa890 uses it); the legend entry under the finger comes from it
    O_MAPOPEN     = 0x3e79a0,    // UI_MapOpen(const UIID *, player): no console menu -> Console_OpenScreen(8, 0, player, 0) (the big map = waypoint map)
    O_MAPCLOSE    = 0x3e7940,    // UI_MapClose(const UIID *, player): -> 0x166b90(1, 0) close the console menu
    O_WP_FLY      = 0x239f60,    // WaypointMap_ShowFlyout(index) and O_CAP4 0x4caf60 (its condition): what the stick selection 0x238fa0 calls
    O_CAP4        = 0x4caf60,
    O_SM_DIRTY    = 0x1bbb80,    // ButtonLegend_SetDirty(sm, 1): the screens' Update rebuilds the legend
    O_HC_TYPE     = 0x34de90,    // HeroCreate_SetHeroType(index) (L1 / R1 0x351700 / 0x351760 on the HeroTypeRadioButtons radio group)
    O_HC_GENDER   = 0x3514c0,    // HeroCreate_SetGender(index) (left stick 0x351620 on GenderRadioButtons)
    V_WAYPOINT    = 0x17ff370,   // waypoint / big map state: +0 busy, +0x28 waypoints {f32 x, y; int id @8; UIID @0x10} x 0x18, +0x30 count, +0x40 current,
                                 //   +0x44 selected, +0x48 moved, +0x54 overlay mode
    V_WP_ROOT = 0x18624f8, V_WP_WORLD = 0x1862518,   // UIID vars: WaypointMap_main, ...OverlayContainer.WorldMap (acts as its children)
    V_MINIMAP = 0x1861610,       // UIID var: Root.NormalLayer.minimap_dialog_backgroundScreen.minimap_dialog_pve (the HUD minimap)
    V_LEGEND = 0x185f558, V_LEGEND_DEATH = 0x1861438,   // UIID vars: ConsoleButtonLegend_main...ButtonLegend, deathmenu_console_dialog...ButtonLegend
    V_HC_ROOT = 0x185f0d8, V_HC_TYPES = 0x185f1c0, V_HC_GENDERS = 0x185f110,   // ConsoleHeroCreate_main, HeroTypeRadioButtons, GenderRadioButtons
    V_HC_FLAG1 = 0x18626bc, V_HC_FLAG2 = 0x18627a4,   // 0x351760: last hero type = (flag1 || !flag2) ? 1 : 5
    SM_OFF        = 0x110,       // console screen manager + 0x110: button legend model, 0x17 slots x 0x40 {int action (-1 none); string label @8; int n @0x38};
                                 //   +0x5c0 != 0 = being rebuilt (0x1bb820 reads -1). Screens switch on their slot's action: OnButton1 = slot 4, OnButton2 = slot 7
    UI_W = 960, UI_H = 720,      // UI virtual space (main 0x34bc80: 960.0 / 720.0 at 0xeb09e0 / 0xeb0e68)
    MAX_CTRL = 65536, MAX_DEPTH = 40, RAW_BYTES = 0x400, MAX_SEG = 64,
    EV_PRESS = 0x1d, BTN_1 = 0, BTN_LEFT = 10, BTN_RIGHT = 11,   // event codes 0x2b44xx: 0x1d press, 0x1e held, 0x1f long, 0x21 release
};
// inventory-family screen objects (bss pointers; their handlers UIInventory_OnButton* 0x1f6810 fan out to all of them)
static const uint32_t k_inv_vars[] = {0x186dbe8, 0x187c068, 0x17e6308, 0x186dc38, 0x187c060, 0x187ac70, 0x17e6218, 0x17e5d90, 0x17e55e8};
#define NW 6
static const struct { uint32_t off; int n; uint32_t w[NW]; } k_sites[] = {
    {O_SETFOCUS,   4, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},
    {O_UIRECT,     4, {0xd10103ff, 0xfd0013e8, 0xf90017f3, 0xa9037bfd}},
    {O_UPDATERECT, 4, {0xd10143ff, 0x6d0123e9, 0xf90013f5, 0xa9034ff4}},
    {O_SETSEL,     6, {0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0x2a0103f5, 0xaa0003f3}},
    {O_DISPATCH,   6, {0xd10303ff, 0xf9003bf9, 0xa9085ff8, 0xa90957f6, 0xa90a4ff4, 0xa90b7bfd}},
    {O_SENDMSG,    6, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0203f3}},
    {O_RING_ROT,   6, {0x6dbc2beb, 0x6d0123e9, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0103f3}},
    {O_RING_SEL,   6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xb9802808, 0xaa0203f4}},
    {O_INV_RINGCB, 6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0x2a0203f5, 0xaa0103f4}},
    {O_INV_RINGOK, 6, {0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0103f3, 0x94064913}},
    {O_SKL_RINGCB, 6, {0x90006ea8, 0xf9402108, 0xf9400100, 0x1400002d, 0xd100c3ff, 0xa9014ff4}},
    {O_PAR_RINGCB, 6, {0xd10143ff, 0xf90013f5, 0xa9034ff4, 0xa9047bfd, 0x910103fd, 0x2a0203f4}},
    {O_JRN_RINGCB, 6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xf0007a28, 0xf9414508}},
    {O_PTY_RINGCB, 6, {0x900079a8, 0xf942f908, 0xf9400108, 0xb9401909, 0x7100093f, 0x54000061}},
    {O_OPENSCREEN, 6, {0xf81b0ff9, 0xa9015ff8, 0xa90257f6, 0xa9034ff4, 0xa9047bfd, 0x910103fd}},
    {O_PREVSCREEN, 6, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0103f3, 0x97f69c88, 0x340000c0}},
    {O_NEXTSCREEN, 6, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0103f3, 0x97f69c78, 0x340000c0}},
    {O_SEL_PREV,   6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xf9400008, 0xf9408908}},
    {O_SEL_NEXT,   6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xf9400008, 0xf9408908}},
    {O_INGAME,     6, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf0006413, 0xf9446a73, 0xf9400260}},
    {O_SOUND,      6, {0xd10283ff, 0xfd0013ea, 0x6d0323e9, 0xa9046ffc, 0xa90567fa, 0xa9065ff8}},
    {O_GRP_SET,    6, {0xd10143ff, 0xf9000bf7, 0xa90257f6, 0xa9034ff4, 0xa9047bfd, 0x910103fd}},
    {O_SKL_STEP,   6, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb941f808, 0xaa0003f3, 0x71000d1f}},
    {O_TEXT_HIT,   6, {0xd10343ff, 0xfd002bee, 0x6d0633ed, 0x6d072beb, 0x6d0823e9, 0xa9095ff8}},
    {O_MAPOPEN,    6, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x2a0103e0, 0x2a0103f3, 0x9404e26b}},
    {O_MAPCLOSE,   6, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x2a0103f3, 0x97fd0090, 0x6b13001f}},
    {O_WP_FLY,     6, {0xd10183ff, 0xa9025ff8, 0xa90357f6, 0xa9044ff4, 0xa9057bfd, 0x910143fd}},
    {O_CAP4,       6, {0xa9bf7bfd, 0x910003fd, 0x90006408, 0xf9446908, 0xf9400100, 0x97ecf917}},
    {O_SM_DIRTY,   2, {0xb905c001, 0xd65f03c0}},
    {O_HC_TYPE,    6, {0xd10143ff, 0xf90013f5, 0xa9034ff4, 0xa9047bfd, 0x910103fd, 0x2a0003f5}},
    {O_HC_GENDER,  6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0x2a0003f5, 0x90007000}},
    // ring callbacks passed to Ring_Select (8.3, round 3)
    {0x1c1c50, 6, {0xd102c3ff, 0x6d0423e9, 0xa9056ffc, 0xa90667fa, 0xa9075ff8, 0xa90857f6}},
    {0x1c7510, 6, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x2a0203f3, 0xaa0103f4, 0x9405ff77}},
    {0x1740f0, 6, {0xd10343ff, 0xa9095ff8, 0xa90a57f6, 0xa90b4ff4, 0xa90c7bfd, 0x910303fd}},
    {0x18d590, 6, {0xd10183ff, 0xf90013f7, 0xa90357f6, 0xa9044ff4, 0xa9057bfd, 0x910143fd}},
    {0x199e40, 5, {0x90007da8, 0xf947ed08, 0xf9400108, 0x91130100, 0x17fffe1c}},
    {0x198eb0, 6, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xb0007db5, 0xf947eeb5}},
    {0x1a4d30, 6, {0xd0007d48, 0xf941f108, 0xf9400108, 0xb9400109, 0x7100053f, 0x54000061}},
};
static const struct { uint32_t vt, slot, fn; } k_vts[] = {   // relocated vtable slots that must point where the class tables above say
    {O_VT_LIST, 0x60, O_LIST_HM}, {O_VT_CHECK, 0x60, O_CHECK_HM}, {O_VT_SELECTOR, 0x60, O_SELECTOR_HM}, {O_VT_INVBASE, 0x28, O_INV_UPDATE},
    {O_VT_LIST, 0xb0, O_UPDATERECT},
};
static const struct { uint32_t got, var; } k_gots[] = {{O_GOT_G, O_G}, {O_GOT_SCREENS, O_SCREENS}, {O_GOT_SNDP, O_SNDP}};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1;   // -1 unchecked, 0 not this main, 1 ok
static int g_dump_mode, g_tap_mode, g_crumbs, g_tid;
static char g_dir[1100];
static _Atomic int g_menu_open;

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
static int32_t ri(uint64_t a) { int32_t v = 0; return rd(a, &v, 4) ? v : 0; }
static int in_image(uint64_t a) { uint64_t b = (uint64_t)(uintptr_t)g_base; return a >= b && a - b < g_size; }
static uint64_t mo(uint64_t a) { return a - (uint64_t)(uintptr_t)g_base; }
static uint64_t ma(uint64_t off) { return (uint64_t)(uintptr_t)g_base + off; }
static int is_obj(uint64_t p) { uint64_t vt; return p && (p & 7) == 0 && rd(p, &vt, 8) && in_image(vt); }   // a C++ object: vtable inside main

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_D3_UIDUMP"); g_dump_mode = e && *e ? atoi(e) : 0;
    e = getenv("DM_D3_UITAP"); g_tap_mode = e && *e ? atoi(e) : 1;   // default on; 0 = off, 2 = dry run
    e = getenv("DM_D3_UI_CRUMBS"); g_crumbs = !(e && *e == '0');
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < O_G + 0x20) { LOG("【红】[d3ui] no main module"); return 0; }
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, 4u * k_sites[i].n)) {
            LOG("【红】[d3ui] main+%#x differs: not D3 update 2.7.7 (build 2607a74f), UI dump / tap / menu signal off", k_sites[i].off); return 0; }
    for (unsigned i = 0; i < sizeof k_gots / sizeof *k_gots; i++)
        if (*(uint64_t *)(g_base + k_gots[i].got) != ma(k_gots[i].var)) { LOG("【红】[d3ui] GOT main+%#x does not point at main+%#x: off", k_gots[i].got, k_gots[i].var); return 0; }
    for (unsigned i = 0; i < sizeof k_vts / sizeof *k_vts; i++)
        if (*(uint64_t *)(g_base + k_vts[i].vt + k_vts[i].slot) != ma(k_vts[i].fn)) {
            LOG("【红】[d3ui] vtable main+%#x slot %#x is not main+%#x: off", k_vts[i].vt, k_vts[i].slot, k_vts[i].fn); return 0; }
    e = getenv("DM_RUNTIME_DIR"); snprintf(g_dir, sizeof g_dir, "%s", e && *e ? e : "dm");
    LOG("[d3ui] on: dump %d, tap %d%s, crumbs %d, trigger %s/ui-dump-now, game thread tid %d", g_dump_mode, g_tap_mode, g_tap_mode == 2 ? " (dry run)" : "",
        g_crumbs, g_dir, (int)gettid());
    return g_ok = 1;
}

// ---- names (dm/ui-names.tsv: "<16 hex id>\t<path>") and handler names (dm/ui-callbacks.tsv: "entry\thandler\thash\tname"), loaded off the game thread
typedef struct { uint64_t id; char *path; } name_ent;
static name_ent *g_names, *g_cbn; static size_t g_nnames, g_ncbn; static _Atomic int g_names_state;   // 0 not started, 1 loading, 2 ready
static int name_cmp(const void *a, const void *b) { uint64_t x = ((const name_ent *)a)->id, y = ((const name_ent *)b)->id; return x < y ? -1 : x > y; }
static name_ent *load_tsv(const char *file, int col, size_t *count) {   // col = column of the key (hex), the name is the last column
    char p[1200]; snprintf(p, sizeof p, "%s/%s", g_dir, file);
    FILE *f = fopen(p, "r"); size_t cap = 0, n = 0; name_ent *v = NULL; char line[1024];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#' || !strchr(line, '\t')) continue;
            char *c = line; for (int k = 0; k < col && c; k++) { c = strchr(c, '\t'); if (c) c++; }
            char *last = strrchr(line, '\t');
            if (!c || !last || !((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) continue;
            if (n == cap) { cap = cap ? cap * 2 : 4096; name_ent *nv = realloc(v, cap * sizeof *v); if (!nv) break; v = nv; }
            v[n].id = strtoull(c, NULL, 16); v[n].path = strdup(last + 1); if (v[n].path) n++;
        }
        fclose(f); qsort(v, n, sizeof *v, name_cmp);
    }
    *count = n;
    LOG("[d3ui] %s: %zu entries%s", file, n, f ? "" : " (missing)");
    return v;
}
static void *names_load(void *arg) {
    (void)arg;
    size_t n1, n2; name_ent *a = load_tsv("ui-names.tsv", 0, &n1), *b = load_tsv("ui-callbacks.tsv", 1, &n2);
    g_names = a; g_nnames = n1; g_cbn = b; g_ncbn = n2; atomic_store(&g_names_state, 2);
    return NULL;
}
static const char *lookup(const name_ent *v, size_t n, uint64_t id) {
    if (atomic_load(&g_names_state) != 2 || !v) return "";
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (v[m].id < id) lo = m + 1; else hi = m; }
    return lo < n && v[lo].id == id ? v[lo].path : "";
}
static const char *name_of(uint64_t id) { return lookup(g_names, g_nnames, id); }
static const char *cb_name(uint64_t fn) { return in_image(fn) ? lookup(g_cbn, g_ncbn, mo(fn)) : ""; }

// UIID of a child: FNV-1a 64 continued from the parent's id over ".<name>" lowercased (main 0x347360 / 0x6b20)
static uint64_t child_id(uint64_t parent, const char *name) {
    uint64_t h = parent; const char *s = ".";
    for (int pass = 0; pass < 2; pass++, s = name)
        for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 0x100000001b3ull; }
    return h;
}

// ---- UI manager and its control map (main 0x330000: {u32 mask, u32 count, alloc, buckets @0x10}; node {next, UIID, control})
static uint64_t ui_mgr(void) { uint64_t o = rq(ma(O_G) + 0x10); return o ? rq(o + 0xb80) : 0; }
enum { K_OTHER, K_LIST, K_CHECK, K_SELECTOR, K_BUTTON };
typedef struct {
    uint64_t id, c, vt, parent, h118; uint16_t flags; char type[24]; int has_rect, act, fh, kind, layer, depth, sib;
    float r[4]; int path[MAX_DEPTH]; int npath;
} ctrl;
static ctrl *g_c; static int g_nc, g_cap;
static int ctrl_cmp(const void *a, const void *b) { uint64_t x = ((const ctrl *)a)->id, y = ((const ctrl *)b)->id; return x < y ? -1 : x > y; }
static ctrl *find(uint64_t id) {
    int lo = 0, hi = g_nc;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_c[m].id < id) lo = m + 1; else hi = m; }
    return lo < g_nc && g_c[lo].id == id ? &g_c[lo] : NULL;
}
// Class: every "cmp w1, #imm" (0x7100003f | imm12 << 10) of the vtable+0x88 IsA(type) up to its first ret / b, e.g. "37/1/51/2" (list).
static void isa_types(uint64_t vt, char *out, size_t cap) {
    uint64_t f = rq(vt + 0x88); uint32_t w[16]; size_t n = 0; out[0] = 0;
    if (!in_image(f) || !rd(f, w, sizeof w)) { snprintf(out, cap, "?"); return; }
    for (int i = 0; i < 16 && w[i] != 0xd65f03c0 && (w[i] & 0xfc000000) != 0x14000000; i++)
        if ((w[i] & 0xffc003ff) == 0x7100003f && n + 6 < cap) n += (size_t)snprintf(out + n, cap - n, "%s%u", n ? "/" : "", w[i] >> 10 & 0xfff);
    if (!n) snprintf(out, cap, "?");
}
// Snapshot of every control (game thread). Returns the count, -1 when the UI does not exist yet. Grows to MAX_CTRL (in-game maps hold > 8192).
static int snapshot(uint64_t mgr) {
    uint64_t map = rq(mgr); uint32_t hdr[2]; uint64_t buckets;
    if (!map || !rd(map, hdr, 8) || !(buckets = rq(map + 0x10))) return -1;
    uint32_t mask = hdr[0];
    if (mask > 0xfffff || (mask & (mask + 1))) return -1;
    g_nc = 0; int full = 0;
    for (uint32_t i = 0; i <= mask && !full; i++) {
        uint64_t node = rq(buckets + 8ull * i);
        for (int guard = 0; node && guard < 4096; guard++) {
            uint64_t nd[3]; if (!rd(node, nd, sizeof nd)) break;
            uint8_t h[0x40]; uint64_t vt;
            if (nd[2] && rd(nd[2], h, sizeof h) && in_image(vt = *(uint64_t *)h) && *(uint64_t *)(h + 0x30) == nd[1]) {
                if (g_nc == g_cap) {
                    int nc = g_cap ? g_cap * 2 : 16384; if (nc > MAX_CTRL) nc = MAX_CTRL;
                    ctrl *v = nc > g_cap ? realloc(g_c, (size_t)nc * sizeof *g_c) : NULL;
                    if (!v) { full = 1; break; }
                    g_c = v; g_cap = nc;
                }
                ctrl *c = &g_c[g_nc++]; memset(c, 0, sizeof *c);
                c->id = nd[1]; c->c = nd[2]; c->vt = vt; c->flags = *(uint16_t *)(h + 0x18); c->parent = *(uint64_t *)(h + 0x38);
                isa_types(vt, c->type, sizeof c->type); c->has_rect = rq(vt + 0xb0) == ma(O_UPDATERECT);
                if (c->has_rect && !rd(nd[2] + 0x88, c->r, sizeof c->r)) c->has_rect = 0;
                int shown = c->flags >> 2 & 1; uint64_t act = mo(rq(vt + 0x10)); uint32_t f3c8 = 0, f1e8 = 0;
                c->act = act == O_ACT_NONE ? 0 : act == O_ACT_BUTTON ? (rd(nd[2] + 0x3c8, &f3c8, 4) ? shown && (f3c8 & 0x20) : -1)
                       : act == O_ACT_EDIT ? (rd(nd[2] + 0x1e8, &f1e8, 4) ? shown && f1e8 : -1) : act == O_ACT_FLAG ? (rd(nd[2] + 0x3c8, &f3c8, 4) ? shown && (f3c8 & 1) : -1) : -1;
                c->fh = mo(rq(vt + 0x40)) != O_FOCUS_NOP;
                uint64_t v = mo(vt);
                c->kind = v == O_VT_LIST ? K_LIST : v == O_VT_CHECK ? K_CHECK : v == O_VT_SELECTOR ? K_SELECTOR : v == O_VT_BUTTON ? K_BUTTON : K_OTHER;
                c->h118 = c->has_rect ? rq(nd[2] + 0x118) : 0;   // input handler table (0x3b26a0 copies desc+0x560..0x728): +0x20 + 8*button press
            }
            node = nd[0];
        }
    }
    if (full) { static int said; if (!said++) LOG("【红】[d3ui] more than %d UI controls: snapshot cut", g_cap); }
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
static int is_under(const ctrl *c, uint64_t anc) {   // c == anc or a descendant of it
    for (int n = 0; c && n < MAX_DEPTH; n++, c = find(c->parent)) if (c->id == anc) return 1;
    return 0;
}
static int vis(const ctrl *c) { return c && (c->flags & 4); }
static int hit(const ctrl *c, float x, float y) { return vis(c) && c->has_rect && x >= c->r[0] && x < c->r[2] && y >= c->r[1] && y < c->r[3]; }
static uint64_t press_handler(const ctrl *c, int button) {   // the control's own handler for a button press (table +0x118: +0x20 + 8 * button)
    uint64_t f = c && c->h118 ? rq(c->h118 + 0x20 + 8ull * button) : 0; return in_image(f) ? f : 0;
}

// UI units -> window pixels: the game's own 4:3 pixel region (0x34baf0) when its inputs exist, else the same formula from the window size
typedef struct { float l, t, r, b; unsigned w, h; int from_game; } pxmap;
static int px_map(pxmap *m) {
    uint64_t gfx = rq(ma(O_GFXP)); uint32_t wh[2];
    if (!gfx || !rd(gfx + 0x244, wh, 8) || !wh[0] || !wh[1] || wh[0] > 8192 || wh[1] > 8192) return 0;
    m->w = wh[0]; m->h = wh[1]; m->from_game = 0;
    float uw = floorf((float)wh[1] * 1.3333334f); m->l = ((float)wh[0] - uw) * 0.5f; m->r = m->l + uw; m->t = 0; m->b = (float)wh[1];
    uint64_t o = rq(ma(O_G) + 0x10), tgt = o ? rq(o + 0xb78) : 0, t20 = tgt ? rq(tgt + 0x20) : 0;
    if (t20 && in_image(rq(t20))) {   // 0x34baf0 dereferences (*(g+0x10)+0xb78)+0x20 and its vtable+0x28: call only when they exist
        float r[4] = {0}; ((void (*)(float *))(g_base + O_UIRECT))(r);
        if (isfinite(r[0]) && isfinite(r[3]) && r[2] - r[0] > 16 && r[3] - r[1] > 16) { m->l = r[0]; m->t = r[1]; m->r = r[2]; m->b = r[3]; m->from_game = 1; }
    }
    return 1;
}
static float ux2px(const pxmap *m, float x) { return m->l + x * (m->r - m->l) / UI_W; }
static float uy2px(const pxmap *m, float y) { return m->t + y * (m->b - m->t) / UI_H; }

// ---- game state: console screen groups, input listeners, rings
typedef struct { uint64_t grp; int gid, count, cur, player; } screens;
static int screen_group(screens *s) {   // the console screen manager's current group (0x169c80: +0x108 != 0 = a console menu is up)
    memset(s, 0, sizeof *s); s->gid = -1;
    uint64_t S = rq(ma(O_SCREENS)); if (!S) return 0;
    s->grp = rq(S + 0x108); if (!s->grp) return 0;
    for (int i = 0; i <= 0x20; i++) if (rq(S + 8ull * i) == s->grp) { s->gid = i; break; }   // 0x166480
    s->count = ri(s->grp + 8); s->cur = ri(s->grp + 0x18); s->player = ri(s->grp + 0x1c);
    return 1;
}
static int ui_player(uint64_t mgr) { int p = ri(mgr + 0x58); return p >= 0 && p < 8 ? p : 0; }   // mgr+0x58: the UI's owning player (-1 = any)
static int id_valid(uint64_t id) { return id != ~0ull && find(id) != NULL; }

typedef struct { uint64_t h, cb3, cb4, obj; const char *host; int player; } ring;
static int ring_ok(uint64_t h) {   // a ring helper (0x389750): +0 {f32 angle; UIID seg} x 0x10, +8 count, +0x18 ring UIID, +0x20 cursor, +0x28 sel, +0x2c active
    uint64_t v = rq(h), n = rq(h + 8), id = rq(h + 0x18);
    return v && n && n <= MAX_SEG && id && id != ~0ull && (v & 7) == 0;
}
static int find_rings(ring *out, int max, int player) {
    int n = 0; uint64_t o;
    for (unsigned i = 0; i < sizeof k_inv_vars / sizeof *k_inv_vars && n < max; i++)   // inventory family: Update 0x1f7590 runs the ring while obj+0x10
        if (is_obj(o = rq(ma(k_inv_vars[i]))) && rq(rq(o) + 0x28) == ma(O_INV_UPDATE) && rq(o + 0x10) && ring_ok(o + 0xd0))
            out[n++] = (ring){o + 0xd0, ma(O_INV_RINGCB), ma(O_INV_RINGOK), o, rq(o) == ma(O_VT_INVBASE) ? "inventory" : "inventory-family", ri(o + 0xc)};
    // the other hosts of 0x389d40 (call sites 0x1c3d7c, 0x1c7fe4, 0x1740e4, 0x18d42c, 0x198c90, 0x198cf8, 0x1a6240): {struct or pointer var, ring
    // offset, condition}; each passes its own selection-changed callback
    static const struct { uint32_t var; int deref; uint32_t off, cond_off; int cond; uint32_t cb; const char *host; } k_hosts[] = {
        {0x17e5e50, 0, 0xb0, 0xdc, 1, 0x1c1c50, "achievements"},            // cond: +0xdc != 0
        {0x17e5fe8, 0, 0x28, 0, 0, 0x1c7510, "banner"},
        {0x17e5390, 0, 0x98, 0xc4, 1, 0x1740f0, "season journey"},
        {0x17e5928, 0, 8, 0x34, 1, 0x18d590, "leaderboards"},
        {0x17e5c18, 1, 0x4c0, 0, 0, 0x199e40, "leaderboards hero"},
        {0x17e5c18, 1, 0xb0 + 0xd0, 0, 0, 0x198eb0, "leaderboards hero inventory"},
        {0x17e5ce8, 1, 0x3e0, 0, 2, 0x1a4d30, "equipment manager"},          // cond: *(int *)obj == 1
    };
    for (unsigned i = 0; i < sizeof k_hosts / sizeof *k_hosts && n < max; i++) {
        uint64_t b = k_hosts[i].deref ? rq(ma(k_hosts[i].var)) : ma(k_hosts[i].var); if (!b) continue;
        if (k_hosts[i].cond == 1 && !ri(b + k_hosts[i].cond_off)) continue;
        if (k_hosts[i].cond == 2 && ri(b) != 1) continue;
        if (ring_ok(b + k_hosts[i].off)) out[n++] = (ring){b + k_hosts[i].off, ma(k_hosts[i].cb), 0, b, k_hosts[i].host, player};
    }
    if (n < max && (o = rq(ma(V_SKILLS))) && ri(o + 0x1f8) == 0 && ring_ok(o + 0x168)) out[n++] = (ring){o + 0x168, ma(O_SKL_RINGCB), 0, o, "skills", player};
    if (n < max && (o = rq(ma(V_PARAGON))) && ri(o + 0xd8) == 0 && ring_ok(o + 8)) out[n++] = (ring){o + 8, ma(O_PAR_RINGCB), 0, o, "paragon", player};
    if (n < max && (o = rq(ma(V_JOURNAL))) && rq(o + 0x2858) && !ri(o + 0x2944) && ring_ok(o + 0x2860)) out[n++] = (ring){o + 0x2860, ma(O_JRN_RINGCB), 0, o, "journal", player};
    if (n < max && (o = rq(ma(V_PARTY))) && ri(o + 0x18) == 2 && ring_ok(o + 0x318)) out[n++] = (ring){o + 0x318, ma(O_PTY_RINGCB), 0, o, "party", player};
    return n;
}
static uint64_t map_get(uint64_t mgr, uint64_t id) {   // the control map lookup of 0x330000 (FNV-1a 32 over the id's low 4 bytes & mask), no assert
    uint64_t map = rq(mgr), buckets; uint32_t mask, h = 0x811c9dc5u;
    if (!id || id == ~0ull || !map || !rd(map, &mask, 4) || mask > 0xfffff || !(buckets = rq(map + 0x10))) return 0;
    for (int k = 0; k < 4; k++) h = (h ^ (uint32_t)(id >> (8 * k) & 0xff)) * 0x1000193u;
    uint64_t node = rq(buckets + 8ull * (h & mask));
    for (int guard = 0; node && guard < 4096; guard++) { uint64_t nd[3]; if (!rd(node, nd, sizeof nd)) break; if (nd[1] == id) return nd[2]; node = nd[0]; }
    return 0;
}
static int in_game(void) { return !((int (*)(void))(g_base + O_INGAME))(); }   // 0x4c7df0 = 1 while no game is loaded

// "A console menu is up" (published every poll): front end, a console screen group, a focused control, a modal one (mgr+0x38) or a live
// inventory-family screen (inventory / stash / vendor / artisans ...)
static int menu_open_now(uint64_t mgr, int *why) {
    *why = 0;
    if (!rq(mgr)) return 0;
    if (!in_game()) { *why = 1; return 1; }
    screens s; if (screen_group(&s)) { *why = 2; return 1; }
    uint64_t f = map_get(mgr, rq(mgr + 0x50)), m = map_get(mgr, rq(mgr + 0x38)); uint16_t fl;   // present and shown (+0x18 bit 2)
    if (f && rd(f + 0x18, &fl, 2) && (fl & 4)) { *why = 3; return 1; }
    if (m && rd(m + 0x18, &fl, 2) && (fl & 4)) { *why = 4; return 1; }
    for (int k = 0; k < 2; k++) {   // the console button legend or the death screen's legend shown (hidden in plain gameplay)
        uint64_t c = map_get(mgr, rq(ma(k ? V_LEGEND_DEATH : V_LEGEND))); uint16_t f2;
        if (c && rd(c + 0x18, &f2, 2) && (f2 & 4)) { *why = 6; return 1; }
    }
    for (unsigned i = 0; i < sizeof k_inv_vars / sizeof *k_inv_vars; i++) {   // an inventory-family screen live (+0x18, what its handlers test: 0x370790)
        uint64_t o = rq(ma(k_inv_vars[i])), rc;
        if (o && ri(o + 0x18) && (rc = map_get(mgr, rq(o + 0xd0 + 0x18))) && rd(rc + 0x18, &fl, 2) && (fl & 4)) { *why = 5; return 1; }   // + its ring shown
    }
    return 0;
}
int dm_d3_ui_menu_open(void);
int dm_d3_ui_menu_open(void) { return g_ok > 0 && g_tap_mode > 0 && atomic_load_explicit(&g_menu_open, memory_order_acquire); }   // 0 while taps are off (DM_D3_UITAP=0)

// ---- the game's own actions
static int g_dry;
static void play_sound(int id) { if (id != -1 && !g_dry) ((void (*)(int, const void *))(g_base + O_SOUND))(id, (const void *)(uintptr_t)ma(O_SNDP)); }
// the UI's logical press of a console button (BUTTON_1 = accept, BUTTON_2 = back, ...) through the UI input dispatcher: what a press
// reaches, minus the pad and minus the gameplay handlers further down the event chain
static int ui_button(uint64_t mgr, int button, const char *why) {
    uint8_t ev[0x48] __attribute__((aligned(16))) = {0}; int32_t v;
    v = EV_PRESS; memcpy(ev, &v, 4); v = ui_player(mgr); memcpy(ev + 0x28, &v, 4); memcpy(ev + 0x2c, &button, 4);
    uint64_t f = rq(mgr + 0x50);
    int r = g_dry ? -1 : ((int (*)(const void *))(g_base + O_DISPATCH))(ev);
    LOG("[d3ui]   %s: button %d -> UI dispatcher 0x32c490, focus %016llx %s, handled %d", why, button, (unsigned long long)f, name_of(f), r);
    return r;
}
static int accept(uint64_t mgr, const char *why) { return ui_button(mgr, BTN_1, why); }
static int legend_action(int slot) {   // the action id the button legend model holds for a slot (what the screens' OnButton handlers switch on)
    uint64_t S = rq(ma(O_SCREENS)); if (!S) return -1;
    uint64_t sm = S + SM_OFF; if (ri(sm + 0x5c0)) return -1;
    return ri(sm + 0x40ull * slot);
}
static int send_button(uint64_t mgr, uint64_t id, int button) {   // that control's own HandleMessage for a button press (UI_SendMessage 0x32cd60)
    uint8_t ev[0x48] __attribute__((aligned(16))) = {0}; int32_t v, kind[4] = {1, 0, 0, 0};
    v = EV_PRESS; memcpy(ev, &v, 4); v = ui_player(mgr); memcpy(ev + 0x28, &v, 4); memcpy(ev + 0x2c, &button, 4);
    return g_dry ? -1 : ((int (*)(const uint64_t *, const int32_t *, const void *))(g_base + O_SENDMSG))(&id, kind, ev);
}
static void focus_on(uint64_t mgr, uint64_t id) {   // only on focus-driven screens (a valid focus already); listener screens keep focus -1
    uint64_t f = rq(mgr + 0x50);
    if (f == id || !id_valid(f) || g_dry) return;
    ((void (*)(const uint64_t *))(g_base + O_SETFOCUS))(&id);
    LOG("[d3ui]   focus %016llx -> %016llx %s", (unsigned long long)f, (unsigned long long)rq(mgr + 0x50), name_of(id));
}

// ---- tap
static _Atomic uint64_t g_tap_xy; static _Atomic int g_tap_pending;
int dm_d3_ui_tap(float x, float y);
int dm_d3_ui_tap(float x, float y) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_D3_UITAP"); mode = e && *e ? atoi(e) : 1; }   // before the first poll too
    if (g_ok == 0 || mode <= 0 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    atomic_store(&g_tap_xy, (uint64_t)yb << 32 | xb); atomic_store_explicit(&g_tap_pending, 1, memory_order_release);
    return 1;
}
static void segment_centre(const ctrl *c, float *x, float *y) { *x = (c->r[0] + c->r[2]) * 0.5f; *y = (c->r[1] + c->r[3]) * 0.5f; }

// tab headers: <nav>.Glow1..5 (the five header slots of InventoryNav-style navigators) and <nav>.KeyBinding1 / 2 (the L / R icons)
static int tap_nav(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    for (int i = 0; i < nh; i++) {
        ctrl *c = hits[i];
        for (int depth = 0; c && depth < MAX_DEPTH; depth++, c = find(c->parent)) {
            uint64_t p = c->parent; if (!find(child_id(p, "KeyBinding1"))) continue;
            static const char *glow[5] = {"Glow1", "Glow2", "Glow3", "Glow4", "Glow5"};
            int which = -1;
            if (c->id == child_id(p, "KeyBinding1")) which = 100; else if (c->id == child_id(p, "KeyBinding2")) which = 101;
            else for (int k = 0; k < 5; k++) if (c->id == child_id(p, glow[k])) which = k;
            if (which < 0) continue;
            screens s; int has = screen_group(&s); int player = has && s.player >= 0 && s.player < 8 ? s.player : ui_player(mgr);
            if (which >= 100) {
                LOG("[d3ui] tap UI %.0f,%.0f -> nav %016llx %s: %s", ux, uy, (unsigned long long)c->id, name_of(c->id), which == 100 ? "Console_PrevScreen" : "Console_NextScreen");
                if (!g_dry) ((void (*)(const uint64_t *, int))(g_base + (which == 100 ? O_PREVSCREEN : O_NEXTSCREEN)))(&c->id, player);
                return 1;
            }
            float best = 1e9f; for (int k = 0; k < 5; k++) {   // the slots overlap: the nearest centre along x among the visible ones
                ctrl *g = find(child_id(p, glow[k])); if (!vis(g) || !g->has_rect) continue;
                float d = fabsf((g->r[0] + g->r[2]) * 0.5f - ux); if (d < best) { best = d; which = k; }
            }
            if (!has || s.gid < 0) { LOG("[d3ui] tap UI %.0f,%.0f -> tab %d of %s: no console screen group open, nothing done", ux, uy, which + 1, name_of(p)); return 1; }
            // GlowN = the N-th ENABLED screen of the group: L / R (0x16a5c0 / 0x1701b0) skip screens with enabled (+0x10) == 0, and the header
            // shows only the reachable ones (hero menu: 6 screens, the 5th (party, 0x21d4b0) disabled, 5 headers; Glow5 = quests = screen 5)
            uint64_t ents = rq(s.grp); int screen = -1;
            for (int k = 0, seen = 0; ents && k < s.count && k < 32; k++) if (ri(ents + 0x18ull * k + 0x10) && seen++ == which) { screen = k; break; }
            LOG("[d3ui] tap UI %.0f,%.0f -> tab Glow%d of %016llx %s: group %d screen %d -> %d (the %d. enabled of %d)", ux, uy, which + 1,
                (unsigned long long)p, name_of(p), s.gid, s.cur, screen, which + 1, s.count);
            if (screen < 0 || screen == s.cur) return 1;
            int r = g_dry ? -1 : ((int (*)(unsigned, unsigned, int, int))(g_base + O_OPENSCREEN))((unsigned)s.gid, (unsigned)screen, player, 0);
            LOG("[d3ui]   Console_OpenScreen(%d, %d, player %d) = %d", s.gid, screen, player, r);
            return 1;
        }
    }
    return 0;
}
// rings: the segment under the finger (its own rect, else the nearest segment centre within one segment size) inside a live ring helper
static int tap_ring(uint64_t mgr, float ux, float uy) {
    ring rs[16]; int nr = find_rings(rs, 16, ui_player(mgr));
    for (int i = 0; i < nr; i++) {
        ring *R = &rs[i]; uint64_t rid = rq(R->h + 0x18), v = rq(R->h); int n = (int)rq(R->h + 8), sel = ri(R->h + 0x28), active = ri(R->h + 0x2c);
        ctrl *rc = find(rid); if (!hit(rc, ux, uy)) continue;
        int pick = -1; float best = 1e9f;
        for (int k = 0; k < n; k++) {
            ctrl *sc = find(rq(v + 0x10ull * k + 8)); if (!vis(sc) || !sc->has_rect) continue;
            if (hit(sc, ux, uy)) { pick = k; break; }
            float cx, cy, sz = fmaxf(sc->r[2] - sc->r[0], sc->r[3] - sc->r[1]); segment_centre(sc, &cx, &cy);
            float d = hypotf(cx - ux, cy - uy); if (d <= sz && d < best) { best = d; pick = k; }
        }
        if (pick < 0) { LOG("[d3ui] tap UI %.0f,%.0f -> ring %016llx %s (%s): between segments, nothing done", ux, uy, (unsigned long long)rid, name_of(rid), R->host); return 1; }
        uint64_t seg = v + 0x10ull * pick + 8, sid = rq(seg);
        LOG("[d3ui] tap UI %.0f,%.0f -> ring %016llx %s (%s, helper %#llx) segment %d/%d %016llx %s, selected %d, active %d", ux, uy, (unsigned long long)rid,
            name_of(rid), R->host, (unsigned long long)R->h, pick, n, (unsigned long long)sid, name_of(sid), sel, active);
        if (!active && rq(R->obj) == ma(O_VT_INVBASE) && legend_action(7) == 3) {
            // the console inventory with the item list holding the input: its own B (UIInventory_OnButton2 -> 0x36e1d0, legend slot 7 action 3 =
            // back: sound 0x3c0 + vtable+0x18 0x1f7480, which re-activates the ring 0x38a690(ring, 1)); then the segment as usual
            ui_button(mgr, 1, "ring inactive, the inventory's own back (B, legend action 3)");
            active = ri(R->h + 0x2c); sel = ri(R->h + 0x28);
            if (active && pick == sel) return 1;   // back on the ring at that very segment: done (no accept)
        } else if (active && pick == sel) { accept(mgr, "selected ring segment"); return 1; }
        if (!active) { LOG("[d3ui]   ring not active (the screen's other panel has the input; legend B action %d): nothing done", legend_action(7)); return 1; }
        if (R->cb4 && !g_dry && !(((uint64_t (*)(uint64_t, uint64_t))(uintptr_t)R->cb4)(R->h, seg) & 1)) { LOG("[d3ui]   segment refused by the screen (0x1f77a0)"); return 1; }
        if (!g_dry) {
            ((void (*)(uint64_t, uint64_t))(g_base + O_RING_ROT))(R->h, seg);
            ((void (*)(uint64_t, uint64_t, uint64_t))(g_base + O_RING_SEL))(R->h, seg, R->cb3);
        }
        LOG("[d3ui]   ring select: 0x38a110 + 0x38a4d0(cb %#llx) -> selected %d", (unsigned long long)mo(R->cb3), ri(R->h + 0x28));
        return 1;
    }
    return 0;
}
// a checkbox / value selector under the finger: its own message handler (BUTTON_1) / its own step (left or right half)
static int widget(uint64_t mgr, ctrl *w, float ux) {
    if (w->kind == K_CHECK) {
        int r = send_button(mgr, w->id, BTN_1);
        LOG("[d3ui]   checkbox %016llx: BUTTON_1 -> its HandleMessage 0x38f530 (toggle 0x38e790) = %d", (unsigned long long)w->id, r);
        return 1;
    }
    if (w->kind == K_SELECTOR) {
        int right = ux >= (w->r[0] + w->r[2]) * 0.5f;
        if (!g_dry) ((void (*)(uint64_t, int))(g_base + (right ? O_SEL_NEXT : O_SEL_PREV)))(w->c, 1);
        LOG("[d3ui]   selector %016llx: %s step %#x -> value %d", (unsigned long long)w->id, right ? "next" : "previous", right ? O_SEL_NEXT : O_SEL_PREV, ri(w->c + 0x1dc));
        return 1;
    }
    return 0;
}

// skills screen (ConsoleSkillSelection; object *(main+0x186dc40), mode +0x1f8: 1 skills of a category, 2 runes, 3 passives, else the loadout
// ring): each mode moves a selection group (0x23ace0 layout) of its own: +0x200 skills, +0x238 runes, +0x270 passives (0x37b5f0 / 0x37b490).
// A tap on an icon of the active group = SelGroup_Set(group, k, 1) (what its own left / right steps 0x23ae30 / 0x23ae00 end in); a tap on the
// selected icon = accept (UISkills_OnButton1 0x382180: assign). The arrows of ElectiveMode (left unnamed35 / right unnamed34) page the
// category the way its own step does past the edge: put the selection on the edge (SelGroup_Set), then Skills_Step(obj, dir) 0x37b5f0.
static const char *k_grp_name[4] = {"", "skills", "runes", "passives"};
static int tap_skills(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    uint64_t o = rq(ma(V_SKILLS)), root = rq(ma(V_SKILLS_ROOT)); if (!o || !vis(find(root))) return 0;
    int mode = ri(o + 0x1f8);
    uint64_t elective = child_id(child_id(child_id(child_id(root, "LayoutRoot"), "SkillSelection"), "Skills"), "ElectiveMode");
    int arrow = -1;
    for (int i = 0; i < nh; i++) {
        if (hits[i]->id == child_id(elective, "unnamed35")) arrow = 0;
        else if (hits[i]->id == child_id(elective, "unnamed34")) arrow = 1;
    }
    for (int gm = 1; gm <= 3; gm++) {
        uint64_t g = o + 0x200 + 0x38ull * (gm - 1), items = rq(g + 8); int cnt = ri(g + 0x10), sel = ri(g + 0x20);
        if (!items || cnt <= 0 || cnt > 64 || !in_image(rq(g + 0x30))) continue;
        int pick = -1;
        for (int i = 0; i < nh && pick < 0; i++)
            for (int k = 0; k < cnt; k++) if (is_under(hits[i], rq(items + 8ull * k))) { pick = k; break; }
        if (pick < 0) continue;
        uint64_t pid = rq(items + 8ull * pick);
        LOG("[d3ui] tap UI %.0f,%.0f -> skills %s icon %d/%d %016llx %s, selected %d, mode %d", ux, uy, k_grp_name[gm], pick, cnt, (unsigned long long)pid, name_of(pid), sel, mode);
        if (gm != mode) { LOG("[d3ui]   not the active group (mode %d): nothing done", mode); return 1; }
        if (pick == sel) { accept(mgr, "selected skill icon"); return 1; }
        int r = g_dry ? -1 : ((int (*)(uint64_t, int, int))(g_base + O_GRP_SET))(g, pick, 1);
        LOG("[d3ui]   SelGroup_Set 0x23ace0(%d) = %d -> selected %d", pick, r, ri(g + 0x20));
        return 1;
    }
    if (arrow >= 0) {
        uint64_t g = o + 0x200; int cnt = ri(g + 0x10), cat = ri(o + 0x6c0);
        LOG("[d3ui] tap UI %.0f,%.0f -> skills category arrow %s, mode %d, category %d, %d icons", ux, uy, arrow ? "right" : "left", mode, cat, cnt);
        if (mode != 1 || cnt <= 0 || cnt > 64) return 1;
        if (!g_dry) {
            ((int (*)(uint64_t, int, int))(g_base + O_GRP_SET))(g, arrow ? cnt - 1 : 0, 1);   // to the edge, then its own step pages
            ((void (*)(uint64_t, int))(g_base + O_SKL_STEP))(o, arrow);
        }
        LOG("[d3ui]   Skills_Step 0x37b5f0(%d): category %d -> %d, selected %d", arrow, cat, ri(o + 0x6c0), ri(g + 0x20));
        return 1;
    }
    return 0;
}
// button legend (main menu "A accept  X change hero  B exit", ConsoleButtonLegend_main...ButtonLegend, and the dialogs' own legends): the
// text of the text control (+0x398; the main one is built by 0x1bb840 from the legend model: per slot "{icon:<button>,2.0}" + nbsp + label,
// entries 6 spaces apart, centred) is split into entries; the entry under the finger = that console button's logical press through the UI
// dispatcher. Widths are estimated per character (icon 18, space 5.8, other 7.4 UI units; main menu legend measured at 318..642).
static int icon_button(const char *n, size_t len) {
    static const struct { const char *n; int b; } k[] = {{"button1", 0}, {"button2", 1}, {"button3", 2}, {"button4", 3}, {"l1", 4}, {"r1", 5}, {"l2", 6},
        {"r2", 7}, {"up", 8}, {"down", 9}, {"left", 10}, {"right", 11}, {"minus", 12}, {"plus", 13}, {"l3", 14}, {"r3", 15}, {"options", 17}};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (strlen(k[i].n) == len && !strncasecmp(n, k[i].n, len)) return k[i].b;
    return -1;
}
static int tap_legend(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    for (int i = 0; i < nh; i++) {
        ctrl *c = hits[i]; if (c->kind != K_BUTTON) continue;
        char t[512]; uint64_t tp = rq(c->c + 0x398); size_t got = sizeof t - 1;
        if (!tp) continue;
        while (got >= 16 && !rd(tp, t, got)) got /= 2;
        if (got < 16) continue;
        t[got] = 0;
        if (!strstr(t, "{icon:")) continue;
        struct { float a, b; int btn, at; } e[16]; int ne = 0, spaces = 0; float x = 0;
        for (const char *q = t; *q; ) {
            if (!strncasecmp(q, "{icon:", 6)) {
                const char *nm = q + 6, *end = strchr(nm, '}'); if (!end) break;
                size_t l = strcspn(nm, ",}"); int b = icon_button(nm, l);
                if (b >= 0 && ne < 16) { if (ne) e[ne - 1].b = x - spaces * 5.8f; e[ne].a = x; e[ne].b = -1; e[ne].btn = b; e[ne].at = (int)(q - t); ne++; }
                x += 18; spaces = 0; q = end + 1; continue;
            }
            unsigned char ch = (unsigned char)*q;
            if (ch == ' ') { x += 5.8f; spaces++; q++; continue; }
            if (ch == 0xc2 && (unsigned char)q[1] == 0xa0) { x += 5.8f; q += 2; continue; }   // nbsp between icon and label
            if (ch == '{') { const char *end = strchr(q, '}'); q = end ? end + 1 : q + 1; continue; }   // other markup
            spaces = 0; x += 7.4f; q++;
            while ((*q & 0xc0) == 0x80) q++;   // one UTF-8 code point
        }
        if (!ne) continue;
        if (e[ne - 1].b < 0) e[ne - 1].b = x;
        float w = x, cw = c->r[2] - c->r[0], cx = (c->r[0] + c->r[2]) * 0.5f, k = w > cw && w > 0 ? cw / w : 1.0f, x0 = cx - w * k * 0.5f;
        int pick = -1; float best = 1e9f;
        for (int j = 0; j < ne; j++) {
            float a = x0 + e[j].a * k, b = x0 + e[j].b * k, d = ux < a ? a - ux : ux > b ? ux - b : 0;
            if (d < best) { best = d; pick = j; }
        }
        // the real glyph layout: the control's own text hit test (font metrics, alignment) gives the text index under the finger; the entry is the
        // last icon at or before it. Used when it lands inside the text; the estimate above stays for the log and as the fallback.
        float pt[2] = {ux, uy}; int at = g_dry ? -1 : ((int (*)(uint64_t, const float *))(g_base + O_TEXT_HIT))(c->c, pt), real = -1;
        if (at >= 0 && at <= (int)strlen(t)) { for (int j = 0; j < ne; j++) if (e[j].at <= at) real = j; if (real < 0) real = 0; }
        LOG("[d3ui] tap UI %.0f,%.0f -> button legend %016llx %s: %d entries, text index %d -> entry %d; estimate over %.0f..%.0f -> entry %d (%.0f off)", ux, uy,
            (unsigned long long)c->id, name_of(c->id), ne, at, real, x0, x0 + w * k, pick, best);
        if (real >= 0) { pick = real; best = 0; }
        if (pick < 0 || best > 30) return 1;
        ui_button(mgr, e[pick].btn, "button legend entry");
        return 1;
    }
    return 0;
}

// waypoint / big map (WaypointMap_main; UI_MapOpen opens it as console screen group 8): the zone map's waypoints are V_WAYPOINT+0x28, the world
// overview (OverlayContainer.WorldMap shown) lists its acts as that control's children. A tap on one does what the stick selection in
// WaypointMap_OnUpdate does (0x238fa0..0x238fdc): selected (+0x44) = k, moved (+0x48) = 1, on the zone map the flyout 0x239f60 (when
// 0x4caf60) and the legend refresh 0x1bbb80; a tap on the selected one = accept (UI_WaypointMapAccept 0x239b50: travel / open the act).
static int tap_waypoint(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    uint64_t W = ma(V_WAYPOINT);
    if (!vis(find(rq(ma(V_WP_ROOT)))) || ri(W)) return 0;
    ctrl *world = find(rq(ma(V_WP_WORLD))); int overview = vis(world), n, pick = -1; uint64_t base, stride, idoff;
    if (overview) { base = rq(world->c + 0x60); n = (int)rq(world->c + 0x68); stride = 0x18; idoff = 0; }
    else { base = rq(W + 0x28); n = (int)rq(W + 0x30); stride = 0x18; idoff = 0x10; }
    if (!base || n <= 0 || n > 256) return 0;
    for (int i = 0; i < nh && pick < 0; i++)
        for (int k = 0; k < n; k++) if (is_under(hits[i], rq(base + stride * k + idoff))) { pick = k; break; }
    if (pick < 0) return 0;
    int sel = ri(W + 0x44); uint64_t id = rq(base + stride * pick + idoff);
    LOG("[d3ui] tap UI %.0f,%.0f -> %s %d/%d %016llx %s, selected %d, current %d", ux, uy, overview ? "map act" : "waypoint", pick, n, (unsigned long long)id, name_of(id),
        sel, ri(W + 0x40));
    if (pick == sel) { accept(mgr, "selected waypoint (UI_WaypointMapAccept)"); return 1; }
    if (g_dry) return 1;
    int32_t v = pick, one = 1; memcpy((void *)(uintptr_t)(W + 0x44), &v, 4); memcpy((void *)(uintptr_t)(W + 0x48), &one, 4);
    if (!overview) {
        if (((int (*)(void))(g_base + O_CAP4))()) ((void (*)(int))(g_base + O_WP_FLY))(pick);
        uint64_t S = rq(ma(O_SCREENS)); if (S) ((void (*)(uint64_t, int))(g_base + O_SM_DIRTY))(S + SM_OFF, 1);
    }
    return 1;
}
// hero create: the hero type radio group (standard / hardcore / seasonal ...) and the gender radio group; a tap on child k = the screen's own
// setter (what its L1 / R1 and left stick call), within the range its R1 allows
static int tap_herocreate(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    (void)mgr;
    if (!vis(find(rq(ma(V_HC_ROOT))))) return 0;
    for (int g = 0; g < 2; g++) {
        ctrl *grp = find(rq(ma(g ? V_HC_GENDERS : V_HC_TYPES))); if (!vis(grp)) continue;
        uint64_t v = rq(grp->c + 0x60); int n = (int)rq(grp->c + 0x68);
        for (int i = 0; i < nh; i++) for (int k = 0; v && k < n && k < 16; k++) {
            if (!is_under(hits[i], rq(v + 0x18ull * k))) continue;
            int last = g ? 1 : (ri(ma(V_HC_FLAG1)) || !ri(ma(V_HC_FLAG2))) ? 1 : 5;
            LOG("[d3ui] tap UI %.0f,%.0f -> hero create %s %d (of %d, last allowed %d)", ux, uy, g ? "gender" : "hero type", k, n, last);
            if (k <= last && !g_dry) ((void (*)(int))(g_base + (g ? O_HC_GENDER : O_HC_TYPE)))(k);
            return 1;
        }
    }
    return 0;
}
static int tap_list(uint64_t mgr, float ux, float uy, ctrl **hits, int nh) {
    ctrl *L = NULL, *row = NULL; int k = -1;
    uint64_t modal = rq(mgr + 0x38); if (!id_valid(modal)) modal = 0;   // a modal control up (mgr+0x38): only lists inside it
    for (int i = 0; i < g_nc; i++) {
        ctrl *l = &g_c[i]; if (l->kind != K_LIST || !hit(l, ux, uy) || (modal && !is_under(l, modal))) continue;
        int cnt = ri(l->c + 0x218); uint64_t items = rq(l->c + 0x210);
        for (int j = 0; items && j < cnt && j < 256; j++) {
            ctrl *it = find(rq(items + 8ull * j));
            if (hit(it, ux, uy) && (!L || above(l, L))) { L = l; row = it; k = j; break; }
        }
    }
    if (!L) return 0;
    ctrl *fl = find(rq(mgr + 0x50));   // a focused list drawn above this one (a popup over the menu, e.g. ConsolePlayerOptions over the main menu)
    if (fl && fl != L && fl->kind == K_LIST && vis(fl) && above(fl, L)) {
        LOG("[d3ui] tap UI %.0f,%.0f -> list %016llx %s under the focused popup list %016llx %s: nothing done", ux, uy, (unsigned long long)L->id, name_of(L->id),
            (unsigned long long)fl->id, name_of(fl->id));
        return 1;
    }
    int sel = ri(L->c + 0x1e0), first = ri(L->c + 0x1e4), per = ri(L->c + 0x2a4), mode = ri(L->c + 0x2a0);
    int idx = (first < 0 ? 0 : first * (per > 0 ? per : 1)) + k;   // items[k] <-> first * per + k (0x3873d0 / 0x3876c0 use items[sel - first * per])
    LOG("[d3ui] tap UI %.0f,%.0f -> list %016llx %s row %d (item %d %016llx), selected %d, first %d, per %d, layout %d", ux, uy, (unsigned long long)L->id,
        name_of(L->id), idx, k, (unsigned long long)row->id, sel, first, per, mode);
    focus_on(mgr, L->id);
    if (idx != sel) {
        if (!g_dry) ((void (*)(uint64_t, int))(g_base + O_SETSEL))(L->c, idx);
        int now = ri(L->c + 0x1e0);
        if (now == idx) play_sound(ri(L->c + 0x208));   // what the list's own Up / Down does after SetSelected (0x387cc8..0x387ce0)
        LOG("[d3ui]   List_SetSelected 0x3856e0(%d) -> selected %d", idx, now);
        if (mode != 2 || now != idx) return 1;   // vertical / grid: a tap selects, the next tap on it accepts; horizontal (dialog buttons): both
    }
    ctrl *w = NULL;   // a checkbox / selector in the selected row: its own handler
    for (int i = 0; i < nh; i++) if ((hits[i]->kind == K_CHECK || hits[i]->kind == K_SELECTOR) && is_under(hits[i], row->id) && (!w || above(hits[i], w))) w = hits[i];
    if (w && widget(mgr, w, ux)) return 1;
    accept(mgr, idx == sel ? "selected row" : "horizontal list");
    return 1;
}
static void do_tap(uint64_t mgr, float fx, float fy) {
    pxmap m; if (!px_map(&m)) { LOG("[d3ui] tap %.3f,%.3f: no window size yet", fx, fy); return; }
    float px = fx * m.w, py = fy * m.h, ux = (px - m.l) * UI_W / (m.r - m.l), uy = (py - m.t) * UI_H / (m.b - m.t);
    int n = snapshot(mgr); if (n <= 0) { LOG("[d3ui] tap %.3f,%.3f: no UI", fx, fy); return; }
    static ctrl **hits; static int hcap; int nh = 0; ctrl *top = NULL;
    for (int i = 0; i < n; i++) {
        ctrl *c = &g_c[i]; if (!hit(c, ux, uy)) continue;
        if (nh == hcap) { int nc = hcap ? hcap * 2 : 512; ctrl **v = realloc(hits, (size_t)nc * sizeof *v); if (!v) break; hits = v; hcap = nc; }
        hits[nh++] = c;
        int full = c->r[2] - c->r[0] >= 900 && c->r[3] - c->r[1] >= 700;   // screen roots / fade layers: never targets, only skipped in the log
        if (!full && (!top || above(c, top))) top = c;
    }
    g_dry = g_tap_mode == 2;
    if (tap_nav(mgr, ux, uy, hits, nh)) return;
    if (tap_ring(mgr, ux, uy)) return;
    if (tap_skills(mgr, ux, uy, hits, nh)) return;
    if (tap_waypoint(mgr, ux, uy, hits, nh)) return;
    if (tap_herocreate(mgr, ux, uy, hits, nh)) return;
    if (tap_list(mgr, ux, uy, hits, nh)) return;
    if (tap_legend(mgr, ux, uy, hits, nh)) return;
    ctrl *w = NULL, *b = NULL;
    for (int i = 0; i < nh; i++) {
        ctrl *c = hits[i];
        if ((c->kind == K_CHECK || c->kind == K_SELECTOR) && (!w || above(c, w))) w = c;
        if ((c->kind == K_BUTTON || c->act == 1) && press_handler(c, BTN_1) && (!b || above(c, b))) b = c;   // not the screens' event receivers
    }
    if (w) { LOG("[d3ui] tap UI %.0f,%.0f -> %s %016llx %s", ux, uy, w->kind == K_CHECK ? "checkbox" : "selector", (unsigned long long)w->id, name_of(w->id)); widget(mgr, w, ux); return; }
    if (b) {
        uint64_t f = press_handler(b, BTN_1);
        LOG("[d3ui] tap UI %.0f,%.0f -> %016llx %s: own BUTTON_1 handler %#llx %s", ux, uy, (unsigned long long)b->id, name_of(b->id), (unsigned long long)mo(f), cb_name(f));
        focus_on(mgr, b->id);
        LOG("[d3ui]   UI_SendMessage BUTTON_1 = %d", send_button(mgr, b->id, BTN_1));
        return;
    }
    ctrl *title = find(0x3eb11c1a52143430ull);   // Root.NormalLayer.ConsoleStart_main ("press any button"): a tap anywhere = accept
    if (vis(title) && !in_game()) { LOG("[d3ui] tap UI %.0f,%.0f -> title screen", ux, uy); accept(mgr, "title screen"); return; }
    screens sg;   // the big map (console screen group 8): a tap on nothing else closes it with its own UI_MapClose
    if (screen_group(&sg) && sg.gid == 8) {
        uint64_t id = rq(ma(V_WP_ROOT)); int pl = sg.player >= 0 && sg.player < 8 ? sg.player : ui_player(mgr);
        LOG("[d3ui] tap UI %.0f,%.0f -> big map background: UI_MapClose", ux, uy);
        if (!g_dry) ((void (*)(const uint64_t *, int))(g_base + O_MAPCLOSE))(&id, pl);
        return;
    }
    LOG("[d3ui] tap %.3f,%.3f = UI %.0f,%.0f: nothing to do (topmost %016llx %s vt %#llx type %s, %d controls under the finger)", fx, fy, ux, uy,
        top ? (unsigned long long)top->id : 0ull, top ? name_of(top->id) : "", top ? (unsigned long long)mo(top->vt) : 0ull, top ? top->type : "", nh);
}


// ---- swipe = list scroll. The game scrolls a list only by moving its selection (D-pad 0x387a40 and both sticks 0x387800 end in
// List_SetSelected, which brings the new row into view: 0x385980 / 0x385c90). So a swipe selects the row just past the visible edge it pulls
// in: finger down (dy > 0, earlier rows) -> (first row - n), finger up -> (last visible + n); one row per row height of finger travel.
// dm_d3_ui_scroll runs on the UI thread: it answers from a cache of the visible lists (rects in picture fractions) that the game thread
// refreshes every 10 polls while a menu is open, and queues the travel; the next poll applies it.
enum { MAX_LISTS = 24 };
typedef struct { uint64_t id; float l, t, r, b; } list_rect;
static list_rect g_lists[MAX_LISTS]; static int g_nlists; static _Atomic unsigned g_lists_seq;
static _Atomic uint64_t g_scroll_id; static _Atomic int g_scroll_pend;   // travel in 1/100000 of the picture height
static void refresh_lists(uint64_t mgr) {   // game thread: every visible list class control (vtable only, no full snapshot)
    pxmap m; uint64_t map = rq(mgr), buckets; uint32_t mask;
    if (!px_map(&m) || !map || !rd(map, &mask, 4) || mask > 0xfffff || !(buckets = rq(map + 0x10))) return;
    list_rect tmp[MAX_LISTS]; int n = 0;
    for (uint32_t i = 0; i <= mask && n < MAX_LISTS; i++) {
        uint64_t node = rq(buckets + 8ull * i);
        for (int guard = 0; node && guard < 4096 && n < MAX_LISTS; guard++) {
            uint64_t nd[3]; if (!rd(node, nd, sizeof nd)) break;
            uint64_t vt; uint16_t fl; float r[4];
            if (nd[2] && rd(nd[2], &vt, 8) && vt == ma(O_VT_LIST) && rd(nd[2] + 0x18, &fl, 2) && (fl & 4) && rd(nd[2] + 0x88, r, sizeof r) && r[2] > r[0] && r[3] > r[1])
                tmp[n++] = (list_rect){nd[1], ux2px(&m, r[0]) / m.w, uy2px(&m, r[1]) / m.h, ux2px(&m, r[2]) / m.w, uy2px(&m, r[3]) / m.h};
            node = nd[0];
        }
    }
    atomic_fetch_add_explicit(&g_lists_seq, 1, memory_order_acq_rel);   // odd: writing
    memcpy(g_lists, tmp, sizeof tmp[0] * (size_t)n); g_nlists = n;
    atomic_fetch_add_explicit(&g_lists_seq, 1, memory_order_release);
}
int dm_d3_ui_scroll(float x, float y, float dy);
int dm_d3_ui_scroll(float x, float y, float dy) {
    if (g_ok <= 0 || g_tap_mode <= 0 || !atomic_load_explicit(&g_menu_open, memory_order_acquire) || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    uint64_t best = 0; float area = 1e9f;
    for (int tries = 0; tries < 4; tries++) {
        unsigned s0 = atomic_load_explicit(&g_lists_seq, memory_order_acquire); if (s0 & 1) continue;
        list_rect v[MAX_LISTS]; int n = g_nlists; if (n > MAX_LISTS) n = MAX_LISTS; memcpy(v, g_lists, sizeof v[0] * (size_t)n);
        if (atomic_load_explicit(&g_lists_seq, memory_order_acquire) != s0) continue;
        for (int i = 0; i < n; i++) {   // the smallest list under the finger (a list inside a popup over a full-height one)
            float a = (v[i].r - v[i].l) * (v[i].b - v[i].t);
            if (x >= v[i].l && x < v[i].r && y >= v[i].t && y < v[i].b && a < area) { area = a; best = v[i].id; }
        }
        break;
    }
    if (!best) return 0;
    if (atomic_exchange(&g_scroll_id, best) != best) atomic_store(&g_scroll_pend, 0);
    if (isfinite(dy)) atomic_fetch_add(&g_scroll_pend, (int)lrintf(dy * 100000.0f));
    return 1;
}
static void do_scroll(uint64_t mgr) {   // game thread
    static uint64_t last_id; static float acc;
    int pend = atomic_exchange(&g_scroll_pend, 0); uint64_t id = atomic_load(&g_scroll_id);
    if (!id) return;
    if (id != last_id) { last_id = id; acc = 0; }
    if (!pend) return;
    uint64_t L = map_get(mgr, id), vt, items; uint16_t fl;
    if (!L || !rd(L, &vt, 8) || vt != ma(O_VT_LIST) || !rd(L + 0x18, &fl, 2) || !(fl & 4) || !(items = rq(L + 0x210))) return;
    float rr[4]; uint64_t it0 = map_get(mgr, rq(items));
    if (!it0 || !rd(it0 + 0x88, rr, sizeof rr) || rr[3] - rr[1] < 4) return;
    float rowh = rr[3] - rr[1];
    acc += pend / 100000.0f * UI_H;   // picture height = the 720 UI units
    int rows = (int)(acc / rowh); if (!rows) return;
    acc -= rows * rowh;
    int sel = ri(L + 0x1e0), first = ri(L + 0x1e4), per = ri(L + 0x2a4), shown = ri(L + 0x1e8), layout = ri(L + 0x2a0);
    if (per < 1) per = 1;
    if (first < 0) first = 0;
    if (layout == 2) return;   // horizontal lists (dialog buttons) do not scroll vertically
    uint64_t ds = rq(L + 0x258), cf = ds ? rq(rq(ds) + 0x10) : 0;   // data source vtable+0x10 = number of entries (what SetSelected checks)
    if (!in_image(cf)) return;
    int count = ((int (*)(uint64_t))(uintptr_t)cf)(ds), col = sel >= 0 ? sel % per : 0, to;
    if (count <= 0) return;
    if (rows > 0) to = (first - rows) * per + col;                    // finger down: pull in earlier rows at the top
    else to = first * per + (shown > 0 ? shown - 1 : 0) + (-rows) * per;   // finger up: later rows at the bottom
    if (to < 0) to = col < count ? col : 0;
    if (to > count - 1) to = count - 1;
    if (to == sel) return;
    if (!g_dry) ((void (*)(uint64_t, int))(g_base + O_SETSEL))(L, to);
    LOG("[d3ui] scroll %016llx %s: %+d rows (row %.0f UI) -> List_SetSelected(%d): selected %d -> %d, first %d -> %d", (unsigned long long)id, name_of(id), rows,
        rowh, to, sel, ri(L + 0x1e0), first, ri(L + 0x1e4));
}


// ---- minimap -> big map. The HUD minimap's rect (picture fractions) is published every 10 polls while no menu is up; a tap on it (no menu
// open) = UI_MapOpen 0x3e79a0 — the named callback behind the game's own map button: Console_OpenScreen(8, 0, player, 0), the big map.
static _Atomic uint64_t g_mm_lt, g_mm_rb;   // packed float pairs; 0 = no minimap shown
static uint64_t pack2(float a, float b) { uint32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return (uint64_t)y << 32 | x; }
static void unpack2(uint64_t v, float *a, float *b) { uint32_t x = (uint32_t)v, y = (uint32_t)(v >> 32); memcpy(a, &x, 4); memcpy(b, &y, 4); }
static void refresh_minimap(uint64_t mgr, int open) {
    uint64_t c = open ? 0 : map_get(mgr, rq(ma(V_MINIMAP))); uint16_t fl; float r[4]; pxmap m;
    if (!c || !rd(c + 0x18, &fl, 2) || !(fl & 4) || !rd(c + 0x88, r, sizeof r) || r[2] <= r[0] || r[3] <= r[1] || !px_map(&m)) {
        atomic_store(&g_mm_lt, 0); atomic_store(&g_mm_rb, 0); return; }
    atomic_store(&g_mm_lt, pack2(ux2px(&m, r[0]) / m.w, uy2px(&m, r[1]) / m.h)); atomic_store(&g_mm_rb, pack2(ux2px(&m, r[2]) / m.w, uy2px(&m, r[3]) / m.h));
}
int dm_d3_ui_minimap_hit(float x, float y);
int dm_d3_ui_minimap_hit(float x, float y) {   // any thread: the point is on the HUD minimap (and no menu is open)
    if (g_ok <= 0 || g_tap_mode <= 0 || atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;
    uint64_t lt = atomic_load(&g_mm_lt), rb = atomic_load(&g_mm_rb); float l, t, r, b;
    if (!lt || !rb) return 0;
    unpack2(lt, &l, &t); unpack2(rb, &r, &b);
    return x >= l && x < r && y >= t && y < b;
}
static void tap_minimap(uint64_t mgr, float x, float y) {   // game thread, no menu open
    uint64_t lt = atomic_load(&g_mm_lt), rb = atomic_load(&g_mm_rb); float l, t, r, b;
    if (!lt || !rb) { LOG("[d3ui] tap %.3f,%.3f: no menu open, no minimap: ignored", x, y); return; }
    unpack2(lt, &l, &t); unpack2(rb, &r, &b);
    if (!(x >= l && x < r && y >= t && y < b)) { LOG("[d3ui] tap %.3f,%.3f: no menu open, not on the minimap: ignored", x, y); return; }
    uint64_t id = rq(ma(V_MINIMAP));
    LOG("[d3ui] tap %.3f,%.3f -> minimap: UI_MapOpen (big map)", x, y);
    if (g_tap_mode != 2) ((void (*)(const uint64_t *, int))(g_base + O_MAPOPEN))(&id, ui_player(mgr));
}

// ---- full dump
static void dump(uint64_t mgr) {
    char p[1200], praw[1200]; snprintf(p, sizeof p, "%s/ui-dump.tsv", g_dir); snprintf(praw, sizeof praw, "%s/ui-dump-raw.tsv", g_dir);
    int n = snapshot(mgr); pxmap m; int havem = px_map(&m);
    uint64_t focus = rq(mgr + 0x50), active = rq(mgr + 0x28), hover = rq(mgr + 0x20), modal = rq(mgr + 0x38);
    FILE *f = fopen(p, "w"), *fr = fopen(praw, "w");
    if (!f) { LOG("【红】[d3ui] cannot write %s: errno %d", p, errno); if (fr) fclose(fr); return; }
    int why; int open = menu_open_now(mgr, &why);
    fprintf(f, "# D3 UI dump: %d controls, mgr %#llx, focus %016llx %s, active(+0x28) %016llx, +0x20 %016llx, modal(+0x38) %016llx %s, player(+0x58) %d\n", n,
            (unsigned long long)mgr, (unsigned long long)focus, name_of(focus), (unsigned long long)active, (unsigned long long)hover, (unsigned long long)modal,
            name_of(modal), ri(mgr + 0x58));
    if (havem) fprintf(f, "# window %ux%u px; UI 960x720 -> px rect %.1f %.1f %.1f %.1f (%s)\n", m.w, m.h, m.l, m.t, m.r, m.b, m.from_game ? "main 0x34baf0" : "4:3 formula");
    fprintf(f, "# in game %d, menu open %d (why %d: 1 front end, 2 screen group, 3 focus, 4 modal, 5 inventory screen, 6 legend)\n", in_game(), open, why);
    screens s;
    if (screen_group(&s)) {
        fprintf(f, "# screen group %d (%#llx): screen %d of %d, player %d; screens (open fn, close fn, enabled):", s.gid, (unsigned long long)s.grp, s.cur, s.count, s.player);
        uint64_t e = rq(s.grp);
        for (int k = 0; e && k < s.count && k < 16; k++) fprintf(f, " [%d %#llx %#llx %d]", k, (unsigned long long)mo(rq(e + 0x18ull * k)), (unsigned long long)mo(rq(e + 0x18ull * k + 8)), ri(e + 0x18ull * k + 0x10));
        fputc('\n', f);
    } else fprintf(f, "# no console screen group\n");
    uint64_t ls = rq(mgr + 0x10), la = ls ? rq(ls) : 0; uint32_t lc = ls ? (uint32_t)ri(ls + 8) : 0;   // input listeners (dispatcher 0x32cb08)
    fprintf(f, "# input listeners (mgr+0x10): %u:", lc);
    for (uint32_t k = 0; la && k < lc && k < 64; k++) { uint64_t c = rq(la + 8ull * k), id = c ? rq(c + 0x30) : 0; fprintf(f, " %016llx %s;", (unsigned long long)id, name_of(id)); }
    fputc('\n', f);
    fprintf(f, "# button legend model (slot: action):");   // slots: 4 A, 5 X, 6 Y, 7 B, 8/9 L/R, 0xa/0xb ZL/ZR, 0xc..0xf D-pad, 0x12 +, 0x13 -
    for (int k = 0; k < 0x17; k++) { int a = legend_action(k); if (a != -1) fprintf(f, " %#x:%d", k, a); }
    fputc('\n', f);
    { uint64_t o = rq(ma(V_SKILLS)), root = rq(ma(V_SKILLS_ROOT));
      if (o && vis(find(root))) {
          fprintf(f, "# skills %#llx: mode %d, category %d;", (unsigned long long)o, ri(o + 0x1f8), ri(o + 0x6c0));
          for (int gm = 1; gm <= 3; gm++) {
              uint64_t g = o + 0x200 + 0x38ull * (gm - 1), items = rq(g + 8); int cnt = ri(g + 0x10);
              fprintf(f, " %s: %d of %d [", k_grp_name[gm], ri(g + 0x20), cnt);
              for (int k = 0; items && k < cnt && k < 16; k++) { uint64_t id = rq(items + 8ull * k); fprintf(f, "%s%016llx %s", k ? ", " : "", (unsigned long long)id, name_of(id)); }
              fprintf(f, "];");
          }
          fputc('\n', f);
      } }
    ring rs[16]; int nr = find_rings(rs, 16, ui_player(mgr));
    for (int i = 0; i < nr; i++) {
        uint64_t h = rs[i].h, v = rq(h), rid = rq(h + 0x18), cur = rq(h + 0x20); int cnt = (int)rq(h + 8);
        fprintf(f, "# ring %s helper %#llx: ring %016llx %s, cursor %016llx, selected %d, active %d, flags %#x, cb %#llx, %d segments:", rs[i].host, (unsigned long long)h,
                (unsigned long long)rid, name_of(rid), (unsigned long long)cur, ri(h + 0x28), ri(h + 0x2c), ri(h + 0x30), (unsigned long long)mo(rs[i].cb3), cnt);
        for (int k = 0; k < cnt; k++) {
            float a = 0; rd(v + 0x10ull * k, &a, 4); uint64_t sid = rq(v + 0x10ull * k + 8); ctrl *sc = find(sid);
            fprintf(f, " [%d %.2f %016llx %s", k, a, (unsigned long long)sid, name_of(sid));
            if (sc && sc->has_rect) fprintf(f, " %.0f,%.0f", (sc->r[0] + sc->r[2]) * 0.5f, (sc->r[1] + sc->r[3]) * 0.5f);
            fputc(']', f);
        }
        fputc('\n', f);
    }
    fprintf(f, "id\tname\tvt\ttype\tflags\tvis\tshown\tparent\tlayer\tdepth\tsib\tact\tfh\tfocus\tui_l\tui_t\tui_r\tui_b\tpx_l\tpx_t\tpx_r\tpx_b\tfx_l\tfx_t\tfx_r\tfx_b\textra\n");
    int nv = 0;
    for (int i = 0; i < n; i++) {
        ctrl *c = &g_c[i]; int vs = c->flags >> 2 & 1;
        if (!vs && g_dump_mode < 2) continue;
        nv += vs;
        fprintf(f, "%016llx\t%s\t%#llx\t%s\t%#x\t%d\t%d\t%016llx\t%d\t%d\t%d\t%d\t%d\t%d", (unsigned long long)c->id, name_of(c->id), (unsigned long long)mo(c->vt),
                c->type, c->flags, vs, c->flags >> 1 & 1, (unsigned long long)c->parent, c->layer, c->depth, c->sib, c->act, c->fh, c->id == focus);
        if (c->has_rect) {
            fprintf(f, "\t%.1f\t%.1f\t%.1f\t%.1f", c->r[0], c->r[1], c->r[2], c->r[3]);
            if (havem) { float x0 = ux2px(&m, c->r[0]), y0 = uy2px(&m, c->r[1]), x1 = ux2px(&m, c->r[2]), y1 = uy2px(&m, c->r[3]);
                fprintf(f, "\t%.0f\t%.0f\t%.0f\t%.0f\t%.4f\t%.4f\t%.4f\t%.4f", x0, y0, x1, y1, x0 / m.w, y0 / m.h, x1 / m.w, y1 / m.h); }
            else fprintf(f, "\t\t\t\t\t\t\t\t");
        } else fprintf(f, "\t\t\t\t\t\t\t\t\t\t\t\t");
        fputc('\t', f);
        if (c->kind == K_LIST) {
            int32_t cnt = ri(c->c + 0x218); uint64_t items = rq(c->c + 0x210);
            fprintf(f, "list sel=%d first=%d per=%d rows=%d layout=%d n=%d sound=%d cb=%#llx items=", ri(c->c + 0x1e0), ri(c->c + 0x1e4), ri(c->c + 0x2a4), ri(c->c + 0x1e8),
                    ri(c->c + 0x2a0), cnt, ri(c->c + 0x208), (unsigned long long)(in_image(rq(c->c + 0x240)) ? mo(rq(c->c + 0x240)) : 0));
            for (int k = 0; items && k < cnt && k < 64; k++) fprintf(f, "%s%016llx", k ? "," : "", (unsigned long long)rq(items + 8ull * k));
            fputc(' ', f);
        } else if (c->kind == K_CHECK) fprintf(f, "checkbox state=%d ", ri(c->c + 0x1e0));
        else if (c->kind == K_SELECTOR) fprintf(f, "selector value=%d ", ri(c->c + 0x1dc));
        if (c->h118) {   // input handler table: press handlers by button (BUTTON_1 = 0 ... see TOUCH_DESIGN §8), sticks +0x140 / +0x148
            fprintf(f, "in:");
            for (int k = 0; k < 18; k++) { uint64_t fn = rq(c->h118 + 0x20 + 8ull * k); if (in_image(fn)) fprintf(f, " b%d=%#llx%s%s", k, (unsigned long long)mo(fn), *cb_name(fn) ? "/" : "", cb_name(fn)); }
            for (int k = 0; k < 2; k++) { uint64_t fn = rq(c->h118 + 0x140 + 8ull * k); if (in_image(fn)) fprintf(f, " %cs=%#llx%s%s", k ? 'r' : 'l', (unsigned long long)mo(fn), *cb_name(fn) ? "/" : "", cb_name(fn)); }
            fputc(' ', f);
        }
        uint64_t own = c->has_rect ? rq(c->c + 0x48) : 0; if (is_obj(own)) fprintf(f, "owner=%#llx(vt %#llx)", (unsigned long long)own, (unsigned long long)mo(rq(own)));
        fputc('\n', f);
        if (fr && vs) {
            uint8_t raw[RAW_BYTES]; size_t got = RAW_BYTES;
            while (got >= 0x40 && !rd(c->c, raw, got)) got /= 2;
            if (got >= 0x40) { fprintf(fr, "%016llx\t%#llx\t", (unsigned long long)c->id, (unsigned long long)mo(c->vt)); for (size_t k = 0; k < got; k++) fprintf(fr, "%02x", raw[k]); fputc('\n', fr); }
        }
    }
    fclose(f); if (fr) fclose(fr);
    LOG("[d3ui] dump: %d controls (%d visible) -> %s, raw %s; focus %016llx %s, menu open %d", n, nv, p, praw, (unsigned long long)focus, name_of(focus), open);
}

// ---- per poll (hle_hid.c fill(), after the front end's buttons: the game's main thread, main 0x40810 -> 0x2b4230 -> 0xf84b0 -> 0xf7310).
// The pad buttons are not touched (the argument stays for the call site).
void dm_d3_ui_pump(uint64_t *buttons);
void dm_d3_ui_pump(uint64_t *buttons) {
    (void)buttons;
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    uint64_t mgr = ui_mgr();
    if (!mgr) { atomic_store_explicit(&g_menu_open, 0, memory_order_release); return; }
    // calls into the game (0x4c7df0 reads the game thread's TLS) only from the thread that polls the pads for the game's main loop
    if (!g_tid) { g_tid = (int)gettid(); LOG("[d3ui] UI manager up, game thread tid %d", g_tid); }
    else if ((int)gettid() != g_tid) { static int said; if (!said++) LOG("【红】[d3ui] npad poll from another thread (tid %d, game %d): ignored", (int)gettid(), g_tid); return; }
    int why, open = menu_open_now(mgr, &why);
    atomic_store_explicit(&g_menu_open, open, memory_order_release);
    { static int was = -1; if (open != was && (g_dump_mode || g_tap_mode)) LOG("[d3ui] menu %s (%s)", open ? "open" : "closed",
          why == 1 ? "front end" : why == 2 ? "console screen group" : why == 3 ? "focus" : why == 4 ? "modal" : why == 5 ? "inventory screen" : why == 6 ? "button legend" : "gameplay"); was = open; }
    if (g_tap_mode && atomic_exchange_explicit(&g_tap_pending, 0, memory_order_acquire)) {
        uint64_t xy = atomic_load(&g_tap_xy); float x, y; uint32_t xb = (uint32_t)xy, yb = (uint32_t)(xy >> 32); memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
        if (open) do_tap(mgr, x, y); else tap_minimap(mgr, x, y);
    }
    static unsigned tick; ++tick;
    if (g_tap_mode && open && tick % 10 == 3) refresh_lists(mgr);
    if (g_tap_mode && tick % 10 == 7) refresh_minimap(mgr, open);
    if (!open && g_nlists) { atomic_fetch_add(&g_lists_seq, 1); g_nlists = 0; atomic_fetch_add(&g_lists_seq, 1); }
    if (g_tap_mode) { g_dry = g_tap_mode == 2; if (open) do_scroll(mgr); else atomic_store(&g_scroll_pend, 0); }
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
