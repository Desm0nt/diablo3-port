// SPDX-License-Identifier: GPL-2.0-only
// [D3] Mods / cheats (gear "Mods / cheats", DmActivity.showD3Mods): offline seasons, season buffs and cheats.
// This file contains code derived from d3hack (https://github.com/god-jester/d3hack, studied at c0f0dbd), Copyright (C) god-jester and the
// d3hack contributors, licensed under the GNU General Public License version 2 (text: hle_d3_mods.LICENSE next to this file). d3hack is an
// exlaunch module for the Switch game build 2.7.6.90885; its sources carry no per-file headers. Ported from it (C++ -> C, our hook
// mechanism, 2.7.7 addresses): the hooks MoveSpeed / AttackSpeed / EquipAny (d3/hooks/util.hpp), ForceAncient + AugmentSpecifier
// (util.hpp), the sInitializeWorld hook with EnableGod / FlagsAreFun (main.cpp, d3/_lobby.hpp), the offline-season / community-event
// XVar setup (d3/patches.cpp PatchDynamicSeasonal / PatchDynamicEvents / PatchBase) and the cheat patches (PortCheatCodes, PatchBase).
// The rest of this tree is MIT; this file is GPL-2.0 (see README "License").
//
// Everything is off by default; the front end passes the choices at start:
//   DM_D3_SEASON=<n>     offline season n (1..99; unset / 0 = off): the game's season XVars Season.Num = n, Season.State = 1 (in progress),
//                        Season.OverrideEnabled = 1, and the checks that need Blizzard's servers answer "yes" (see k_sites "season")
//   DM_D3_BUFFS=<list>   season buffs / events, comma separated keys of k_buffs (any number at once: every buff is its own XVar and the
//                        game checks each one on its own), "auto" = the theme of season DM_D3_SEASON (k_themes: seasons 14..37); unset = off.
//                        Buffs apply to every hero (CommunityEventSeasonOnly = 0)
//   DM_D3_CHEATS=<list>  comma separated keys: code patches of k_sites (difficulty, portal, cooldowns, craft, cube, gem100, gemfast,
//                        resource, orbs, legendary) and hooks (god, dbgflags, equipany, ancient, primal); unset = none
//   DM_D3_SPEED=<x>      movement speed multiplier (d3hack MovementSpeedMultiplier; 1 / unset = off, 0.1..10)
//   DM_D3_ATKSPEED=<x>   attack speed multiplier (d3hack AttackSpeedMultiplier; 1 / unset = off, 0.1..10)
// d3hack's 2.7.6 sites were moved to this update (2.7.7, main build-id 2607a74f) by matching each site's code: most of main is shifted by
// 0, +0x10, +0x70 or +0x100. Every code site and every hooked / called game function is checked word for word, every XVar by its GOT
// slot, vtable and registered name; on any mismatch that feature stays off with a red log line.
// Code patches and hooks go in before the guest runs (main.c host_preload, dm_d3_mods_install). A hook = the function's first 4
// instructions replaced by "ldr x16,#8; br x16; .quad host" (x16 is free at a call); the original 4 (none PC-relative, checked) run from
// a trampoline that jumps back to +16. Guest code calls host C directly like every HLE entry. The XVars are set on the game's own thread
// from the first npad poll (hle_hid.c, dm_d3_mods_pump: the static constructors have built them by then) through the game's own setters
// (XVarBool::Set 0xa26fc0 / XVarUint32::Set 0xa273c0, dirty flags 3), and re-checked every ~5 s. Offline the game never gets the seasons /
// config files, so nothing else writes them: the lobby idle (0x606d0 -> 0x60704) recomputes the community event state only when the
// server time becomes known, the season update 0x5ff20 runs only once a seasons file is loaded ([GOT 0x114ac48]).
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
static const char *const k_patch_cheats[] = {"difficulty", "portal", "cooldowns", "craft", "cube", "gem100", "gemfast", "resource", "orbs", "legendary"};

// ---- game functions: hooked (h) or called (c), first 4 words checked (2.7.6 -> 2.7.7 address in the comment)
enum {
    F_MOVESPEED  = 0x0c8000,   // h float CPlayerGetMoveSpeedForStickInput(ACD *, Player *, SNO power, float minClientWalkSpeed)   0x0c8000 +0
    F_ATKSPEED   = 0x6cc700,   // h float PowerGetFormulaValueAtLevel(AttribGroupID, const ACD *, SNO power, u8 *formula, int size, int level) 0x6cc600 +0x100
    F_INSLOT     = 0x489280,   // h GameError ACDInventoryItemAllowedInSlot(const ACD *item, const InventoryLocation *, BOOL skipReq, BOOL swapping) 0x489210 +0x70
    F_ANCIENT    = 0x88dee0,   // h void LootRollForAncientLegendary(LootSpecifier *, ACDID looter)                                  0x88dde0 +0x100
    F_INITWORLD  = 0x8128a0,   // h void sInitializeWorld(SNO world, SWorld *)                                                         0x8127a0 +0x100
    F_SRVLOCAL   = 0x4c7e80,   // c BOOL ServerIsLocal(void)                                                                           0x4c7d80 +0x100
    F_STARTUP    = 0x4c7df0,   // c BOOL GameIsStartUp(void) (1 while no game is loaded)                                               0x4c7cf0 +0x100
    F_PLFIRST    = 0x51f8c0,   // c Player *PlayerGetFirstAll(void)                                                                    0x51f7c0 +0x100
    F_PLNEXT     = 0x51f970,   // c Player *PlayerGetNextAll(const Player *)                                                           0x51f870 +0x100
    F_ACDGET     = 0x669cb0,   // c ACD *ACDTryToGet(ACDID)                                                                            0x669bb0 +0x100
    F_ACTORGET   = 0x86e070,   // c Actor *ActorGet(ActorID)                                                                           0x86df70 +0x100
    F_ACTORFLAG  = 0x86f940,   // c void ActorFlagSet(ActorID, ActorFlag, BOOL)                                                        0x86f840 +0x100
    F_ATTRSETI   = 0x46fc00,   // c void ACD_AttributesSetInt(ACD *, FastAttribKey, int)                                               0x46fb90 +0x70
    F_ATTRSETF   = 0x46fb80,   // c void ACD_AttributesSetFloat(ACD *, FastAttribKey, float)                                           0x46fb10 +0x70
    O_APPGLOBALS = 0x1905610, GOT_APPGLOBALS = 0x114a840,   // AppGlobals: +0x40 / +0x44 / +0x48 dwDebugFlags / 2 / 3
};
typedef struct { uint32_t off; uint32_t w[4]; } fn_t;
static const fn_t k_fns[] = {
    {F_MOVESPEED, {0xd101c3ff, 0xfd000bec, 0x6d022beb, 0x6d0323e9}}, {F_ATKSPEED, {0xd101c3ff, 0xa9035ff8, 0xa90457f6, 0xa9054ff4}},
    {F_INSLOT,    {0xd10303ff, 0xa9066ffc, 0xa90767fa, 0xa9085ff8}}, {F_ANCIENT,  {0xfc1b0fea, 0x6d0123e9, 0xa90257f6, 0xa9034ff4}},
    {F_INITWORLD, {0xd10183ff, 0xa9025ff8, 0xa90357f6, 0xa9044ff4}}, {F_SRVLOCAL, {0xf0006408, 0xf9442108, 0xb9513508, 0x51000908}},
    {F_STARTUP,   {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf0006413}}, {F_PLFIRST,  {0xa9bf7bfd, 0x910003fd, 0xf0006148, 0xf9446908}},
    {F_PLNEXT,    {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf0006148}}, {F_ACDGET,   {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x3100041f}},
    {F_ACTORGET,  {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x900046e8}}, {F_ACTORFLAG, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},
    {F_ATTRSETI,  {0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd}}, {F_ATTRSETF, {0x6dbc23e9, 0xf9000bf5, 0xa9024ff4, 0xa9037bfd}},
};
// attribute ids (d3hack attributes.hpp; checked in 2.7.7 against the attribute table the game builds at 0x67b390's callers: id * 0x40)
enum { A_INVULNERABLE = 0x69, A_APS_TOTAL = 0xc9, A_IMMUNE_KNOCKBACK = 0x12d, A_STUN_IMMUNE = 0x134, A_FREEZE_IMMUNE = 0x13a,
       A_SLOWDOWN_IMMUNE = 0x14b, A_GETHIT_IMMUNE = 0x14c, A_FEAR_IMMUNE = 0x231, A_IMMUNE_CHARM = 0x2c7, A_IMMUNE_BLIND = 0x2c8,
       A_GOD = 0x44d, A_ITEMS_INDESTRUCTIBLE = 0x4c0, A_RECEIVED_SEASONAL_LEGENDARY = 0x519 };
enum { ACTOR_GOD_MODE_BIT = 0, ACTOR_EASYKILL_BIT = 1, ACTOR_NO_COLLISION_BIT = 4 };
// struct offsets (d3hack types/common.hpp; LootSpecifier +0x64 / +0x68 / +0xd8 match the reads / writes of 0x88dee0)
enum { PLAYER_ID_ACD = 0x4, ACD_ID_OWNER = 0x14, LS_DISABLE_ANCIENT = 0x64, LS_DISABLE_PRIMAL = 0x68, LS_ANCIENT_RANK = 0xd8 };

// ---- XVars (main bss; vtable = *(GOT) + 0x10; name copied to +8 by the registration 0xa26b30; value at +0xb0)
enum { GOT_VT_BOOL = 0x115a388, GOT_VT_U32 = 0x115a398, F_BOOL_SET = 0xa26fc0, F_U32_SET = 0xa273c0,
       GOT_SEASON_NUM = 0x114ac60, V_SEASON_NUM = 0x1a1bd78, GOT_SEASON_STATE = 0x114ac68, V_SEASON_STATE = 0x1a1be30,
       GOT_SEASON_OVR = 0x11590d8, V_SEASON_OVR = 0x1a1c1c8,
       GOT_EV_ENABLED = 0x114acd8, V_EV_ENABLED = 0x18ffbe0, GOT_EV_SEASONONLY = 0x114ace8, V_EV_SEASONONLY = 0x18ffc98,
       GOT_BUFF_START = 0x114acb8, V_BUFF_START = 0x1771758, GOT_BUFF_END = 0x114acc0, V_BUFF_END = 0x1771760,
       GOT_EGG_FLAG = 0x114ae38, V_EGG_FLAG = 0x1771820 };
static const uint32_t k_fn_bool_set[6] = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f3, 0x12000028};
static const uint32_t k_fn_u32_set[6]  = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0003f3, 0x9102c009};
// season buffs / events: front-end key, XVar, the game's XVar name, GOT slot (0 = none), community event (needs CommunityEventEnabled)
typedef struct { const char *key; uint32_t var; const char *name; uint32_t got; int community; } buff_t;
static const buff_t k_buffs[] = {
    {"goblins",     0x1900138, "CommunityEventDoubleTreasureGoblins", 0, 1},
    {"bags",        0x19001f0, "CommunityEventDoubleBountyBags", 0, 1},
    {"grandeur",    0x19002a8, "CommunityEventRoyalGrandeur", 0, 1},
    {"nightmares",  0x1900360, "CommunityEventLegacyOfNightmares", 0, 1},
    {"triune",      0x1900418, "CommunityEventTriunesWill", 0, 1},
    {"pandemonium", 0x19004d0, "CommunityEventPandemonium", 0, 1},
    {"kanai",       0x1900588, "CommunityEventKanaiPowers", 0, 1},
    {"tempests",    0x1900640, "CommunityEventTrialsOfTempests", 0, 1},
    {"clones",      0x19006f8, "CommunityEventShadowClones", 0, 1},
    {"cube4",       0x19007b0, "CommunityEventFourthKanaisCubeSlot", 0, 1},
    {"ethereal",    0x1900868, "CommunityEventEthrealItems", 0, 1},   // sic, the game's spelling
    {"shards",      0x1900920, "CommunityEventSoulShards", 0, 1},
    {"swarm",       0x19009d8, "CommunityEventSwarmRifts", 0, 1},
    {"sanctified",  0x1900a90, "CommunityEventSanctifiedItems", 0, 1},
    {"alchemy",     0x1900b48, "CommunityEventDarkAlchemy", 0, 1},
    {"nesting",     0x18ff848, "CommunityEventNestingPortals", 0, 1},
    {"keystones",   0x18fffc8, "CommunityEventDoubleRiftKeystones", 0, 1},
    {"bloodshards", 0x1900080, "CommunityEventDoubleBloodShards", 0, 1},
    {"anniversary", 0x18ffa70, "AnniversaryEnabled", 0x1154c00, 0},      // d3hack event_ann
    {"eggworld",    0x18ffb28, "EasterEggWorldEnabled", 0x1152cc8, 0},   // d3hack event_egg_xvar (+ the u32 flag V_EGG_FLAG)
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
static int g_god, g_dbgflags, g_rank;   // hooks' settings (g_rank: 0 off, 1 ancient, 2 primal)
static float g_move = 1, g_atk = 1;

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
static int poke(uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(g_m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
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
        if (poke(k_sites[i].off, w, k_sites[i].n)) { LOG("【红】[d3mods] %s: mprotect RW failed at main+%#x: %s off (sites before it stay patched)", feat, k_sites[i].off, what); return 0; }
    }
    LOG("[d3mods] %s: %d site(s) patched (%s)", feat, n, what);
    return 1;
}

// ---- hooks
static int fn_ok(uint32_t off) {
    for (unsigned i = 0; i < sizeof k_fns / sizeof *k_fns; i++)
        if (k_fns[i].off == off) {
            if (g_m->text_end >= off + 16u && !memcmp(g_m->base + off, k_fns[i].w, 16)) return 1;
            LOG("【红】[d3mods] game function main+%#x differs (not D3 update 2.7.7, build 2607a74f)", off); return 0;
        }
    LOG("【红】[d3mods] main+%#x: no expected words", off); return 0;
}
static int pc_relative(uint32_t w) {
    return (w & 0x1f000000) == 0x10000000 /* adr / adrp */ || (w & 0x7c000000) == 0x14000000 /* b / bl */ ||
           (w & 0xff000010) == 0x54000000 /* b.cond */ || (w & 0x7e000000) == 0x34000000 /* cbz / cbnz */ ||
           (w & 0x7e000000) == 0x36000000 /* tbz / tbnz */ || (w & 0x3b000000) == 0x18000000 /* ldr literal */;
}
static uint32_t *g_tramp; static unsigned g_tramp_n;   // one page of trampolines, 8 words each
// Redirects main+off to host fn. orig != NULL: *orig = a trampoline running the original function (its first 4 words + jump to +16).
static int hook(uint32_t off, void *fn, void **orig, const char *what) {
    if (!fn_ok(off)) { LOG("【红】[d3mods] %s: hook off", what); return 0; }
    const uint32_t *w = (const uint32_t *)(g_m->base + off);
    if (orig) {
        for (int i = 0; i < 4; i++) if (pc_relative(w[i])) { LOG("【红】[d3mods] %s: main+%#x word %d is PC-relative, hook off", what, off, i); return 0; }
        if (!g_tramp) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) { LOG("【红】[d3mods] %s: trampoline mmap failed, hook off", what); return 0; }
            g_tramp = p;
        } else if (mprotect(g_tramp, 4096, PROT_READ | PROT_WRITE)) { LOG("【红】[d3mods] %s: trampoline mprotect failed, hook off", what); return 0; }
        if (g_tramp_n + 8 > 4096 / 4) { LOG("【红】[d3mods] %s: no trampoline room, hook off", what); return 0; }
        uint32_t *t = g_tramp + g_tramp_n; uint64_t back = (uint64_t)(uintptr_t)(g_m->base + off + 16);
        for (int i = 0; i < 4; i++) t[i] = w[i];
        t[4] = 0x58000050; t[5] = 0xd61f0200; t[6] = (uint32_t)back; t[7] = (uint32_t)(back >> 32);   // ldr x16,#8; br x16; .quad back
        if (mprotect(g_tramp, 4096, PROT_READ | PROT_EXEC)) die("[d3mods] trampoline mprotect RX");
        __builtin___clear_cache((char *)t, (char *)(t + 8));
        g_tramp_n += 8; *orig = t;
    }
    uint64_t to = (uint64_t)(uintptr_t)fn;
    const uint32_t jmp[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
    if (poke(off, jmp, 4)) { LOG("【红】[d3mods] %s: mprotect RW failed at main+%#x, hook off", what, off); return 0; }
    LOG("[d3mods] %s: main+%#x hooked%s", what, off, orig ? " (original through a trampoline)" : " (replaced)");
    return 1;
}
#define GFN(type, off) ((type)(void *)(g_m->base + (off)))
typedef int (*v_int_t)(void);
typedef uint64_t (*p_void_t)(void);
typedef uint64_t (*p_p_t)(uint64_t);
typedef uint64_t (*p_id_t)(int32_t);
typedef void (*flag_t)(int32_t, int32_t, int32_t);
typedef void (*seti_t)(uint64_t, uint32_t, int32_t);
typedef void (*setf_t)(uint64_t, uint32_t, float);
static uint32_t attrib_key(uint32_t attr, int32_t param) { return attr | (uint32_t)param << 12; }   // d3hack MakeAttribKey
static void int_attrib_set(uint64_t acd, uint32_t attr) { GFN(seti_t, F_ATTRSETI)(acd, attrib_key(attr, -1), 1); }   // d3hack IntAttribSet(tACD, attr)

// d3hack hooks/util.hpp MoveSpeed: float CPlayerGetMoveSpeedForStickInput(ACD *tACDPlayer, Player *tPlayer, SNO snoPower, float flMinClientWalkSpeed)
static float (*o_movespeed)(uint64_t, uint64_t, int32_t, float);
static float h_movespeed(uint64_t acd, uint64_t player, int32_t sno, float minwalk) {
    float speed = o_movespeed(acd, player, sno, minwalk);
    return g_move > 0.0f ? speed * g_move : speed;
}
// d3hack hooks/util.hpp AttackSpeed: float PowerGetFormulaValueAtLevel(AttribGroupID idFastAttrib, const ACD *ptACD, SNO snoPower, uint8 *pbFormula,
// int32 nFormulaSize, int32 nLevel); only the power d3hack picks (0x50860), whose formula gives the attack speed
static float (*o_atkspeed)(uint32_t, uint64_t, int32_t, uint64_t, int32_t, int32_t);
static float h_atkspeed(uint32_t group, uint64_t acd, int32_t sno, uint64_t formula, int32_t size, int32_t level) {
    float ret = o_atkspeed(group, acd, sno, formula, size, level);
    if (sno != 0x50860 || g_atk <= 0.0f || g_atk == 1.0f) return ret;
    if (acd) GFN(setf_t, F_ATTRSETF)(acd, A_APS_TOTAL, 3.0f);   // tKey.nValue = ATTACKS_PER_SECOND_TOTAL; ACD_AttributesSetFloat(ptACD, tKey, 3.0f)
    return ret * g_atk;
}
// d3hack hooks/util.hpp EquipAny: GameError ACDInventoryItemAllowedInSlot(const ACD *tACDItem, const InventoryLocation *tInvLoc, BOOL fSkipRequirements, BOOL fSwapping)
static int32_t (*o_inslot)(uint64_t, uint64_t, int32_t, int32_t);
static int32_t h_inslot(uint64_t item, uint64_t loc, int32_t skip, int32_t swapping) {
    (void)o_inslot(item, loc, skip, swapping);
    return 0;   // GAMEERROR_NONE
}
// d3hack hooks/util.hpp ForceAncient (replaces LootRollForAncientLegendary) + AugmentSpecifier. Only the ancient fields are written: d3hack
// also writes its [loot_modifiers] values into ForcedILevel / TieredLootRunLevel / torment / gift fields, whose "off" values would overwrite
// what the game set, so they are left alone here.
static void h_ancient(uint64_t spec, int32_t looter) {
    *(int32_t *)(uintptr_t)(spec + LS_DISABLE_ANCIENT) = 0;   // bDisableAncientDrops
    *(int32_t *)(uintptr_t)(spec + LS_DISABLE_PRIMAL) = 0;    // bDisablePrimalAncientDrops
    *(int32_t *)(uintptr_t)(spec + LS_ANCIENT_RANK) = g_rank; // eAncientRank = AncientRankValue (1 ancient, 2 primal)
    uint64_t acd = GFN(p_id_t, F_ACDGET)(looter);
    if (acd) GFN(seti_t, F_ATTRSETI)(acd, A_RECEIVED_SEASONAL_LEGENDARY, 0);   // tKey.nValue = RECEIVED_SEASONAL_LEGENDARY; ACD_AttributesSetInt(.., 0)
}
// d3hack _lobby.hpp FlagsAreFun
static void flags_are_fun(void) {
    uint32_t *f = (uint32_t *)(g_m->base + O_APPGLOBALS + 0x40);   // dwDebugFlags, dwDebugFlags2, dwDebugFlags3
    f[0] |= 1u << 0x6 /* APP_GLOBAL_ALL_HITS_CRIT_BIT */ | 1u << 0xe /* DISPLAY_ALL_SKILLS */ | 1u << 0xf /* DISPLAY_ITEM_AFFIXES */ |
            1u << 0x17 /* DISPLAY_ITEM_ATTRIBUTES */ | 1u << 0x19 /* ALWAYS_PLAY_GETHIT_BIT */;
    f[1] |= 1u << 0xd /* APP_GLOBAL2_DONT_IDENTIFY_RARES_BIT */ | 1u << 0x3 /* ALL_HITS_OVERKILL_BIT */ | 1u << 0x14 /* ALL_HITS_CRUSHING_BLOW_BIT */ |
            1u << 0x16 /* DONT_THROTTLE_FLOATING_NUMBERS */ | 1u << 0x15 /* DISPLAY_ITEM_TARGETED_CLASS */;
    f[2] |= 1u << 0x5 /* APP_GLOBAL3_ANCIENT_ROLLS_WILL_ALWAYS_SUCCEED */ | 1u << 0x4 /* ANCIENTS_ALWAYS_ROLL_PRIMAL */ |
            1u << 0x0 /* LOW_HEALTH_SCREEN_GLOW_DISABLED */ | 1u << 0x1 /* CONSOLE_COLLECTORS_EDITION */;
}
// d3hack _lobby.hpp EnableGod (without FlagsAreFun: that is the separate "dbgflags" toggle here)
static void enable_god(void) {
    for (uint64_t pl = GFN(p_void_t, F_PLFIRST)(); pl; pl = GFN(p_p_t, F_PLNEXT)(pl)) {
        int32_t id = *(const int32_t *)(uintptr_t)(pl + PLAYER_ID_ACD);
        if (id == -1) continue;
        uint64_t acd = GFN(p_id_t, F_ACDGET)(id);
        if (!acd) continue;
        uint64_t actor = GFN(p_id_t, F_ACTORGET)(*(const int32_t *)(uintptr_t)(acd + ACD_ID_OWNER));
        if (!actor) continue;
        int32_t aid = *(const int32_t *)(uintptr_t)actor;
        GFN(flag_t, F_ACTORFLAG)(aid, ACTOR_GOD_MODE_BIT, 1);
        GFN(flag_t, F_ACTORFLAG)(aid, ACTOR_EASYKILL_BIT, 1);
        GFN(flag_t, F_ACTORFLAG)(aid, ACTOR_NO_COLLISION_BIT, 1);
        static const uint32_t attrs[] = {A_GOD, A_INVULNERABLE, A_ITEMS_INDESTRUCTIBLE, A_IMMUNE_KNOCKBACK, A_IMMUNE_BLIND, A_IMMUNE_CHARM,
                                         A_STUN_IMMUNE, A_FEAR_IMMUNE, A_SLOWDOWN_IMMUNE, A_FREEZE_IMMUNE, A_GETHIT_IMMUNE};
        for (unsigned i = 0; i < sizeof attrs / sizeof *attrs; i++) int_attrib_set(acd, attrs[i]);
    }
}
// d3hack main.cpp sInitializeWorld hook: after the world is set up, on the local server (_HOSTCHK = ServerIsLocal() && !GameIsStartUp())
static void (*o_initworld)(int32_t, uint64_t);
static void h_initworld(int32_t sno, uint64_t world) {
    o_initworld(sno, world);
    if (!GFN(v_int_t, F_SRVLOCAL)() || GFN(v_int_t, F_STARTUP)()) return;
    if (g_dbgflags) flags_are_fun();
    if (g_god) enable_god();
    static int n; if (n++ < 4) LOG("[d3mods] world %#x set up: %s%s", (unsigned)sno, g_god ? "god mode on " : "", g_dbgflags ? "debug flags on" : "");
}
static float env_mult(const char *name) {
    const char *e = getenv(name);
    float v = e && *e ? strtof(e, NULL) : 1.0f;
    if (!(v >= 0.1f && v <= 10.0f)) { LOG("【红】[d3mods] %s=%s: not 0.1..10, off", name, e); v = 1.0f; }
    return v;
}
static int called_fns_ok(void) {
    static const uint32_t c[] = {F_SRVLOCAL, F_STARTUP, F_PLFIRST, F_PLNEXT, F_ACDGET, F_ACTORGET, F_ACTORFLAG, F_ATTRSETI, F_ATTRSETF};
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) if (!fn_ok(c[i])) return 0;
    return 1;
}

void dm_d3_mods_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_d3_mods_install(void) {
    const char *es = getenv("DM_D3_SEASON"), *eb = getenv("DM_D3_BUFFS"), *ec = getenv("DM_D3_CHEATS");
    int season = es && *es ? atoi(es) : 0;
    if (season < 0 || season > 99) { LOG("【红】[d3mods] DM_D3_SEASON=%s: not 1..99, offline season off", es); season = 0; }
    int buffs = eb && *eb && strcmp(eb, "0");
    int cheats = ec && *ec && strcmp(ec, "0");
    float move = env_mult("DM_D3_SPEED"), atk = env_mult("DM_D3_ATKSPEED");
    if (!season && !buffs && !cheats && move == 1.0f && atk == 1.0f) { LOG("[d3mods] off (no DM_D3_SEASON / BUFFS / CHEATS / SPEED / ATKSPEED)"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_m = &g_mod[i];
    if (!g_m) { LOG("【红】[d3mods] no main module: mods off"); return; }
    if (season && patch_feature("season", "offline season")) g_season = season;
    if (buffs) {
        uint32_t m = 0;
        if (has_key(eb, "auto")) {
            for (unsigned i = 0; i < sizeof k_themes / sizeof *k_themes; i++)
                if (k_themes[i].season == g_season) for (int b = 0; b < NBUFF; b++) if (has_key(k_themes[i].keys, k_buffs[b].key)) m |= 1u << b;
            if (!m) LOG("[d3mods] DM_D3_BUFFS=auto: season %d has no theme buff (themes: seasons 14..37)", g_season);
        }
        for (int b = 0; b < NBUFF; b++) if (has_key(eb, k_buffs[b].key)) m |= 1u << b;
        if (!m && !has_key(eb, "auto")) LOG("【红】[d3mods] DM_D3_BUFFS=%s: no known buff key", eb);
        g_buffs = m;
    }
    if (cheats) {
        for (unsigned i = 0; i < sizeof k_patch_cheats / sizeof *k_patch_cheats; i++)
            if (has_key(ec, k_patch_cheats[i]) || (!strcmp(k_patch_cheats[i], "resource") && has_key(ec, "god"))) patch_feature(k_patch_cheats[i], "cheat");   // d3hack: super god mode implies infinite MP
        if (has_key(ec, "equipany")) hook(F_INSLOT, (void *)h_inslot, (void **)&o_inslot, "equip anything in any slot");
        int rank = has_key(ec, "primal") ? 2 : has_key(ec, "ancient") ? 1 : 0;
        if (rank && fn_ok(F_ACDGET) && fn_ok(F_ATTRSETI) && hook(F_ANCIENT, (void *)h_ancient, NULL, rank == 2 ? "every legendary primal" : "every legendary ancient")) g_rank = rank;
        int god = has_key(ec, "god"), dbg = has_key(ec, "dbgflags");
        if ((god || dbg) && called_fns_ok()) {
            if (*(const uint64_t *)(g_m->base + GOT_APPGLOBALS) != (uint64_t)(uintptr_t)(g_m->base + O_APPGLOBALS)) LOG("【红】[d3mods] GOT main+%#x does not point at AppGlobals main+%#x: god / debug flags off", GOT_APPGLOBALS, O_APPGLOBALS);
            else if (hook(F_INITWORLD, (void *)h_initworld, (void **)&o_initworld, "god mode / debug flags at world setup")) { g_god = god; g_dbgflags = dbg; }
        }
    }
    if (move != 1.0f && hook(F_MOVESPEED, (void *)h_movespeed, (void **)&o_movespeed, "movement speed")) { g_move = move; LOG("[d3mods] movement speed x%.2f", move); }
    if (atk != 1.0f && fn_ok(F_ATTRSETF) && hook(F_ATKSPEED, (void *)h_atkspeed, (void **)&o_atkspeed, "attack speed")) { g_atk = atk; LOG("[d3mods] attack speed x%.2f", atk); }
    if (g_season || g_buffs) {
        atomic_store(&g_state, 1);
        char b[512] = ""; size_t o = 0;
        for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1) o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", o ? "," : "", k_buffs[i].key);
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
static int community(void) { int c = 0; for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1 && k_buffs[i].community) c = 1; return c; }
static void apply(int first) {
    if (g_season) {
        uint32_t was_n = *(volatile uint32_t *)(g_m->base + V_SEASON_NUM + 0xb0), was_s = *(volatile uint32_t *)(g_m->base + V_SEASON_STATE + 0xb0);
        set_u32(V_SEASON_NUM, (uint32_t)g_season); set_u32(V_SEASON_STATE, 1); set_bool(V_SEASON_OVR, 1);
        if (first) LOG("[d3mods] season XVars: Num %u -> %d, State %u -> 1 (in progress), OverrideEnabled 1", was_n, g_season, was_s);
        else if (was_n != (uint32_t)g_season || was_s != 1) { static int n; if (n++ < 8) LOG("[d3mods] season XVars changed by the game (Num %u, State %u): set again", was_n, was_s); }
    }
    if (g_buffs) {
        for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1) set_bool(k_buffs[i].var, 1);   // only the chosen ones; the rest keep the game's value
        for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1 && !strcmp(k_buffs[i].key, "eggworld")) *(volatile uint32_t *)(g_m->base + V_EGG_FLAG) = 1;
        if (community()) {
            int64_t *t0 = (int64_t *)(g_m->base + V_BUFF_START), *t1 = (int64_t *)(g_m->base + V_BUFF_END);
            if (!*t0 || !*t1 || *t1 <= *t0) { *t0 = 1; *t1 = INT64_MAX; }   // the buff window (unset offline): always
            set_bool(V_EV_SEASONONLY, 0); set_bool(V_EV_ENABLED, 1);
        }
        if (first) LOG("[d3mods] season buffs on (mask %#x)%s", g_buffs, community() ? ", CommunityEventEnabled 1, SeasonOnly 0" : "");
    }
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
            for (int i = 0; ok && i < NBUFF; i++) if (g_buffs >> i & 1) ok = xvar_ok(k_buffs[i].got, k_buffs[i].var, GOT_VT_BOOL, k_buffs[i].name);
            if (ok && community()) {
                ok = xvar_ok(GOT_EV_ENABLED, V_EV_ENABLED, GOT_VT_BOOL, "CommunityEventEnabled") && xvar_ok(GOT_EV_SEASONONLY, V_EV_SEASONONLY, GOT_VT_BOOL, "CommunityEventSeasonOnly");
                if (ok && (*(const uint64_t *)(g_m->base + GOT_BUFF_START) != ma(V_BUFF_START) || *(const uint64_t *)(g_m->base + GOT_BUFF_END) != ma(V_BUFF_END))) {
                    LOG("【红】[d3mods] GOT main+%#x / %#x do not point at the buff window main+%#x / %#x", GOT_BUFF_START, GOT_BUFF_END, V_BUFF_START, V_BUFF_END); ok = 0; }
            }
            for (int i = 0; ok && i < NBUFF; i++) if (g_buffs >> i & 1 && !strcmp(k_buffs[i].key, "eggworld") && *(const uint64_t *)(g_m->base + GOT_EGG_FLAG) != ma(V_EGG_FLAG)) {
                LOG("【红】[d3mods] GOT main+%#x does not point at the easter-egg flag main+%#x", GOT_EGG_FLAG, V_EGG_FLAG); ok = 0; }
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
