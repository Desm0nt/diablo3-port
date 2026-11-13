// [D3] Mods / cheats (gear "Mods / cheats", DmActivity.showD3Mods): offline seasons, a season theme (community buffs) and a few cheats.
// Everything is off by default; the front end passes the choices at start:
//   DM_D3_SEASON=<n>     offline season n (1..99; unset / 0 = off): the game's season XVars Season.Num = n, Season.State = 1 (in progress),
//                        Season.OverrideEnabled = 1, and the checks that need Blizzard's servers answer "yes" (see k_sites "season")
//   DM_D3_THEME=<list>   community buffs (season themes), comma separated keys of k_buffs, or "auto" = the theme of season DM_D3_SEASON
//                        (k_themes: seasons 14..37); unset / 0 = off. Buffs apply to every hero (CommunityEventSeasonOnly = 0)
//   DM_D3_CHEATS=<list>  comma separated keys of k_sites (difficulty, portal, cooldowns, craft, cube, gem100, gemfast, resource, orbs,
//                        legendary); unset = none
// What to change was learned from d3hack by god-jester (https://github.com/god-jester/d3hack, GPL-2.0), an exlaunch module for the Switch
// game build 2.7.6.90885: only the facts (which game function / XVar / instruction, the value it gets) - no d3hack code. Its 2.7.6 sites were
// moved to this update (2.7.7, main build-id 2607a74f) by matching each site's code: most of main is shifted by 0, +0x10, +0x70 or +0x100.
// Every code site is checked word for word before it is patched, every XVar by its GOT slot, vtable and registered name; on any mismatch
// that feature stays off with a red log line.
// Code patches go in before the guest runs (main.c host_preload, dm_d3_mods_install). The XVars are set on the game's own thread from the
// first npad poll (hle_hid.c, dm_d3_mods_pump: the game's static constructors have built them by then) through the game's own setters
// (XVarBool::Set 0xa26fc0 / XVarUint32::Set 0xa273c0, dirty flags 3 as the game's own code passes), and re-checked every ~5 s.
// Offline the game never gets the seasons / config files, so nothing else writes these XVars: the lobby idle (0x606d0 -> 0x60704)
// recomputes the community event state only when the server time becomes known, the season update 0x5ff20 runs only once a seasons
// file is loaded ([GOT 0x114ac48]).
#ifdef DM_GAME_D3
#include "hle.h"
#include "dm.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// ---- code sites: original words -> patch words, grouped by feature (the season group is DM_D3_SEASON, the others DM_D3_CHEATS keys)
#define NW 3
typedef struct { const char *feat; uint32_t off; int n; uint32_t orig[NW], patch[NW]; } site_t;
static const site_t k_sites[] = {
    // offline season (d3hack PatchDynamicSeasonal / PatchBase)
    {"season", 0x5ff0c, 1, {0x12000100}, {0x52800020}},                           // IsSeasonsInitialized: and w0,w8,#1 -> mov w0,#1
    {"season", 0x56b10, 2, {0xa9be4ff4, 0xa9017bfd}, {0x52800020, 0xd65f03c0}},   // IsSignedInOnline(user) -> 1 (hero create 0x351160 refuses a
                                                                                  //   seasonal hero offline: "SeasonNotAvailableOnline")
    {"season", 0x351ccc, 1, {0x54000181}, {0x52800021}},                          // hero create's season-confirm callback 0x351cc0: b.ne -> mov w1,#1
    {"season", 0x1bf5f0, 2, {0xd10403ff, 0xf90063f7}, {0x52800000, 0xd65f03c0}},  // ValidateHeroForPartyMember -> 0 (no error)
    {"season", 0x61620, 2, {0xf81e0ff3, 0xa9017bfd}, {0x52800000, 0xd65f03c0}},   // main / pause menu "Connect to Diablo Servers" entry -> hidden
    {"season", 0x1bd140, 1, {0xb9409115}, {0x52800015}},                          // SetGameParamsForHero, hero without data: season_created =
                                                                                  //   ldr w21,[x8,#0x90] -> mov w21,#n (n filled in at install)
    // cheats (d3hack PortCheatCodes / PatchBase)
    {"difficulty", 0x5282b0, 2, {0x93407c48, 0x36000123}, {0x52800020, 0xd65f03c0}},   // PlayerCanPlayHandicap -> 1: every difficulty unlocked
    {"portal", 0x9e3350, 1, {0x1e380018}, {0x52800018}},                               // town portal / Book of Cain cast time: fcvtzs w24 -> 0
    {"cooldowns", 0x7397d0, 1, {0x1e28cd20}, {0x1e2703e0}},                            // power cooldowns: fcsel s0 -> fmov s0,wzr
    {"cooldowns", 0x7f8a60, 1, {0x14000002}, {0x2a0003e0}},                            //   b over "mov w0,wzr" -> fall into it (0 ticks)
    {"cooldowns", 0x9bc63c, 1, {0x1e20cd08}, {0x1e2703e8}},                            //   fcsel s8 -> fmov s8,wzr
    {"cooldowns", 0x9bc928, 1, {0x1e20cd08}, {0x1e2703e8}},                            //   fcsel s8 -> fmov s8,wzr
    {"craft", 0x20636c, 1, {0x1e213800}, {0xd503201f}},                                // identify progress 1 - t/T -> 1 (instant)
    {"craft", 0x40fa20, 1, {0x11025908}, {0x11001508}},                                // blacksmith craft: now + 150 ticks -> + 5
    {"craft", 0x1dcfa4, 1, {0x1101e108}, {0x11000508}},                                // enchant: now + 120 ticks -> + 1
    {"craft", 0x236d20, 3, {0x321e0fe8, 0x52801689, 0x321d0fea}, {0x52800008, 0x52800009, 0x5280000a}},   // Kanai's Cube: 60 / 180 / 120 -> 0
    {"craft", 0x236d30, 1, {0x321c0fe8}, {0x52800008}},                                //   and 240 -> 0
    {"cube", 0x8875d0, 1, {0xd101c3ff}, {0xd65f03c0}},                                 // Kanai's Cube material consumption -> ret
    {"gem100", 0x6a265c, 1, {0xbc685928}, {0x1e2e1008}},                               // legendary gem upgrade chance (table) -> 1.0
    {"gemfast", 0x349ba8, 1, {0xbd406662}, {0x1e2e1002}},                              // gem upgrade progress step -> 1.0
    {"resource", 0x47ad7c, 1, {0x97ffff41}, {0xd503201f}},                             // resource spend: bl ResourceAttributeSetFloat -> nop
    {"orbs", 0x844ce4, 1, {0x2a1f03e3}, {0x52807ce3}},                                 // extra greater rift progress orbs on elite kills: 0 -> 999
    {"legendary", 0x4fbae4, 1, {0x540002e0}, {0x14000017}},                            // same legendary already equipped: b.eq -> b (allowed)
};
static const char *const k_cheats[] = {"difficulty", "portal", "cooldowns", "craft", "cube", "gem100", "gemfast", "resource", "orbs", "legendary"};

// ---- XVars (main bss; vtable = *(GOT) + 0x10; name copied to +8 by the registration 0xa26b30; value at +0xb0)
enum { GOT_VT_BOOL = 0x115a388, GOT_VT_U32 = 0x115a398, F_BOOL_SET = 0xa26fc0, F_U32_SET = 0xa273c0,
       GOT_SEASON_NUM = 0x114ac60, V_SEASON_NUM = 0x1a1bd78, GOT_SEASON_STATE = 0x114ac68, V_SEASON_STATE = 0x1a1be30,
       GOT_SEASON_OVR = 0x11590d8, V_SEASON_OVR = 0x1a1c1c8,
       GOT_EV_ENABLED = 0x114acd8, V_EV_ENABLED = 0x18ffbe0, GOT_EV_SEASONONLY = 0x114ace8, V_EV_SEASONONLY = 0x18ffc98,
       GOT_BUFF_START = 0x114acb8, V_BUFF_START = 0x1771758, GOT_BUFF_END = 0x114acc0, V_BUFF_END = 0x1771760 };
static const uint32_t k_fn_bool_set[6] = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f3, 0x12000028};
static const uint32_t k_fn_u32_set[6]  = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f3, 0x9102c009};
// community buffs: front-end key, XVar (constructed at 0x4a7cdc.., read through the buff table 0x4a6404..), the game's XVar name
typedef struct { const char *key; uint32_t var; const char *name; } buff_t;
static const buff_t k_buffs[] = {
    {"keystones",   0x18fffc8, "CommunityEventDoubleRiftKeystones"},
    {"bloodshards", 0x1900080, "CommunityEventDoubleBloodShards"},
    {"goblins",     0x1900138, "CommunityEventDoubleTreasureGoblins"},
    {"bags",        0x19001f0, "CommunityEventDoubleBountyBags"},
    {"grandeur",    0x19002a8, "CommunityEventRoyalGrandeur"},
    {"nightmares",  0x1900360, "CommunityEventLegacyOfNightmares"},
    {"triune",      0x1900418, "CommunityEventTriunesWill"},
    {"pandemonium", 0x19004d0, "CommunityEventPandemonium"},
    {"kanai",       0x1900588, "CommunityEventKanaiPowers"},
    {"tempests",    0x1900640, "CommunityEventTrialsOfTempests"},
    {"clones",      0x19006f8, "CommunityEventShadowClones"},
    {"cube4",       0x19007b0, "CommunityEventFourthKanaisCubeSlot"},
    {"ethereal",    0x1900868, "CommunityEventEthrealItems"},   // sic, the game's spelling
    {"shards",      0x1900920, "CommunityEventSoulShards"},
    {"swarm",       0x19009d8, "CommunityEventSwarmRifts"},
    {"sanctified",  0x1900a90, "CommunityEventSanctifiedItems"},
    {"alchemy",     0x1900b48, "CommunityEventDarkAlchemy"},
    {"nesting",     0x18ff848, "CommunityEventNestingPortals"},
};
enum { NBUFF = sizeof k_buffs / sizeof *k_buffs };
// the theme each season had (public season history; seasons without a theme buff are not listed)
static const struct { int season; const char *keys; } k_themes[] = {
    {14, "goblins"}, {15, "bags"}, {16, "grandeur"}, {17, "nightmares"}, {18, "triune"}, {19, "pandemonium"}, {20, "kanai"},
    {21, "tempests"}, {22, "clones,cube4"}, {24, "ethereal"}, {25, "shards"}, {26, "swarm"}, {27, "sanctified"}, {28, "alchemy"},
    {29, "nesting"}, {30, "shards"}, {31, "kanai"}, {32, "ethereal"}, {33, "clones,cube4"}, {34, "sanctified"}, {35, "pandemonium"},
    {36, "shards"}, {37, "kanai"},
};

static module_t *g_m;
static int g_season;              // 0 = off
static uint32_t g_buffs;          // bit per k_buffs entry
static _Atomic int g_state;       // 0 not installed / nothing to do, 1 XVars pending, 2 XVars applied, -1 XVars refused

static int has_key(const char *list, const char *key) {   // comma separated, case-sensitive keys
    size_t n = strlen(key);
    for (const char *p = list; p && *p; ) {
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (*p == ' ')) { p++; l--; }
        while (l && p[l - 1] == ' ') l--;
        if (l == n && !memcmp(p, key, n)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}
static int poke(const site_t *s, const uint32_t *w) {
    uint32_t *at = (uint32_t *)(g_m->base + s->off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + s->n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < s->n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[d3mods] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}
static int site_ok(const site_t *s) { return g_m->text_end >= s->off + 4u * s->n && !memcmp(g_m->base + s->off, s->orig, 4u * s->n); }
// all sites of one feature: checked first, then patched; 0 = off (red line said why)
static int patch_feature(const char *feat, const char *what) {
    int n = 0;
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++) {
        if (strcmp(k_sites[i].feat, feat)) continue;
        if (!site_ok(&k_sites[i])) { LOG("【红】[d3mods] %s: main+%#x differs (not D3 update 2.7.7, build 2607a74f): %s off", feat, k_sites[i].off, what); return 0; }
        n++;
    }
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++) {
        if (strcmp(k_sites[i].feat, feat)) continue;
        uint32_t w[NW]; memcpy(w, k_sites[i].patch, sizeof w);
        if (k_sites[i].off == 0x1bd140) w[0] |= (uint32_t)g_season << 5;   // mov w21,#season
        if (poke(&k_sites[i], w)) { LOG("【红】[d3mods] %s: mprotect RW failed at main+%#x: %s off (sites before it stay patched)", feat, k_sites[i].off, what); return 0; }
    }
    LOG("[d3mods] %s: %d site(s) patched (%s)", feat, n, what);
    return 1;
}

void dm_d3_mods_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_d3_mods_install(void) {
    const char *es = getenv("DM_D3_SEASON"), *et = getenv("DM_D3_THEME"), *ec = getenv("DM_D3_CHEATS");
    int season = es && *es ? atoi(es) : 0;
    if (season < 0 || season > 99) { LOG("【红】[d3mods] DM_D3_SEASON=%s: not 1..99, offline season off", es); season = 0; }
    int theme = et && *et && strcmp(et, "0");
    int cheats = ec && *ec && strcmp(ec, "0");
    if (!season && !theme && !cheats) { LOG("[d3mods] off (no DM_D3_SEASON / DM_D3_THEME / DM_D3_CHEATS)"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_m = &g_mod[i];
    if (!g_m) { LOG("【红】[d3mods] no main module: mods off"); return; }
    if (season && patch_feature("season", "offline season")) g_season = season;
    if (theme) {
        uint32_t m = 0;
        if (!strcmp(et, "auto")) {
            for (unsigned i = 0; i < sizeof k_themes / sizeof *k_themes; i++)
                if (k_themes[i].season == g_season) for (int b = 0; b < NBUFF; b++) if (has_key(k_themes[i].keys, k_buffs[b].key)) m |= 1u << b;
            if (!m) LOG("[d3mods] DM_D3_THEME=auto: season %d has no theme buff (themes: seasons 14..37)", g_season);
        } else for (int b = 0; b < NBUFF; b++) if (has_key(et, k_buffs[b].key)) m |= 1u << b;
        if (et && strcmp(et, "auto") && !m) LOG("【红】[d3mods] DM_D3_THEME=%s: no known buff key", et);
        g_buffs = m;
    }
    if (cheats) {
        for (unsigned i = 0; i < sizeof k_cheats / sizeof *k_cheats; i++) if (has_key(ec, k_cheats[i])) patch_feature(k_cheats[i], "cheat");
    }
    if (g_season || g_buffs) {
        atomic_store(&g_state, 1);
        char b[512] = ""; size_t o = 0;
        for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1) o += (size_t)snprintf(b + o, o < sizeof b ? sizeof b - o : 0, "%s%s", o ? "," : "", k_buffs[i].key);
        LOG("[d3mods] XVars at the first npad poll: season %d, buffs %s", g_season, o ? b : "none");
    }
}

// ---- XVars: checked against the GOT, the class vtable and the registered name
static uint64_t ma(uint32_t off) { return (uint64_t)(uintptr_t)(g_m->base + off); }
static int xvar_ok(uint32_t got, uint32_t var, uint32_t vt_got, const char *name) {
    if (g_m->size < (size_t)var + 0xb8) { LOG("【红】[d3mods] XVar main+%#x (%s) is past the image", var, name); return 0; }
    if (got && *(const uint64_t *)(g_m->base + got) != ma(var)) { LOG("【红】[d3mods] GOT main+%#x does not point at main+%#x (%s)", got, var, name); return 0; }
    uint64_t vt = *(const uint64_t *)(g_m->base + var), want = *(const uint64_t *)(g_m->base + vt_got) + 0x10;
    if (vt != want) { LOG("【红】[d3mods] XVar main+%#x (%s): vtable %#llx, not %#llx (not built yet or not this class)", var, name,
                          (unsigned long long)(vt - ma(0)), (unsigned long long)(want - ma(0))); return 0; }
    if (strncmp((const char *)(g_m->base + var + 8), name, 0x40)) { LOG("【红】[d3mods] XVar main+%#x is \"%.64s\", not %s", var, (const char *)(g_m->base + var + 8), name); return 0; }
    return 1;
}
typedef int (*bool_set_t)(uint64_t xvar, int v, uint32_t dirty);
typedef int (*u32_set_t)(uint64_t xvar, uint32_t v, uint32_t dirty);
static void set_bool(uint32_t var, int v) {
    if ((*(volatile uint8_t *)(g_m->base + var + 0xb0) & 1) != (v & 1)) ((bool_set_t)(void *)(g_m->base + F_BOOL_SET))(ma(var), v, 3);
}
static void set_u32(uint32_t var, uint32_t v) {
    if (*(volatile uint32_t *)(g_m->base + var + 0xb0) != v) ((u32_set_t)(void *)(g_m->base + F_U32_SET))(ma(var), v, 3);
}
static int apply(int first) {
    if (g_season) {
        uint32_t was_n = *(volatile uint32_t *)(g_m->base + V_SEASON_NUM + 0xb0), was_s = *(volatile uint32_t *)(g_m->base + V_SEASON_STATE + 0xb0);
        set_u32(V_SEASON_NUM, (uint32_t)g_season); set_u32(V_SEASON_STATE, 1); set_bool(V_SEASON_OVR, 1);
        if (first) LOG("[d3mods] season XVars: Num %u -> %d, State %u -> 1 (in progress), OverrideEnabled 1", was_n, g_season, was_s);
        else if (was_n != (uint32_t)g_season || was_s != 1) { static int n; if (n++ < 8) LOG("[d3mods] season XVars changed by the game (Num %u, State %u): set again", was_n, was_s); }
    }
    if (g_buffs) {
        int64_t *t0 = (int64_t *)(g_m->base + V_BUFF_START), *t1 = (int64_t *)(g_m->base + V_BUFF_END);
        if (!*t0 || !*t1 || *t1 <= *t0) { *t0 = 1; *t1 = INT64_MAX; }   // the buff window (unset offline): always
        for (int i = 0; i < NBUFF; i++) set_bool(k_buffs[i].var, g_buffs >> i & 1);
        set_bool(V_EV_SEASONONLY, 0); set_bool(V_EV_ENABLED, 1);
        if (first) LOG("[d3mods] community buffs on (mask %#x), CommunityEventEnabled 1, SeasonOnly 0", g_buffs);
    }
    return 1;
}
void dm_d3_mods_pump(void);   // hle_hid.c fill(): every npad poll (the game's main thread)
void dm_d3_mods_pump(void) {
    int st = atomic_load_explicit(&g_state, memory_order_relaxed);
    if (st <= 0) return;
    if (st == 1) {
        int ok = !memcmp(g_m->base + F_BOOL_SET, k_fn_bool_set, sizeof k_fn_bool_set) && !memcmp(g_m->base + F_U32_SET, k_fn_u32_set, sizeof k_fn_u32_set);
        if (!ok) LOG("【红】[d3mods] XVar setters main+%#x / %#x differ: season / buff XVars off", F_BOOL_SET, F_U32_SET);
        if (ok && g_season) ok = xvar_ok(GOT_SEASON_NUM, V_SEASON_NUM, GOT_VT_U32, "Num") && xvar_ok(GOT_SEASON_STATE, V_SEASON_STATE, GOT_VT_U32, "State") &&
                                 xvar_ok(GOT_SEASON_OVR, V_SEASON_OVR, GOT_VT_BOOL, "OverrideEnabled");
        if (ok && g_buffs) {
            ok = xvar_ok(GOT_EV_ENABLED, V_EV_ENABLED, GOT_VT_BOOL, "CommunityEventEnabled") && xvar_ok(GOT_EV_SEASONONLY, V_EV_SEASONONLY, GOT_VT_BOOL, "CommunityEventSeasonOnly");
            for (int i = 0; ok && i < NBUFF; i++) ok = xvar_ok(0, k_buffs[i].var, GOT_VT_BOOL, k_buffs[i].name);
            if (ok && (*(const uint64_t *)(g_m->base + GOT_BUFF_START) != ma(V_BUFF_START) || *(const uint64_t *)(g_m->base + GOT_BUFF_END) != ma(V_BUFF_END))) {
                LOG("【红】[d3mods] GOT main+%#x / %#x do not point at the buff window main+%#x / %#x", GOT_BUFF_START, GOT_BUFF_END, V_BUFF_START, V_BUFF_END); ok = 0; }
        }
        if (!ok) { atomic_store(&g_state, -1); LOG("【红】[d3mods] season / buff XVars not set (the season code patches stay in)"); return; }
        apply(1);
        atomic_store(&g_state, 2);
        return;
    }
    static unsigned polls;
    if (++polls % 300 == 0) apply(0);   // ~5 s at 60 polls a second
}
#endif
