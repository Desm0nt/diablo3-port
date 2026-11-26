// SPDX-License-Identifier: GPL-2.0-or-later
// [D3] Mods / cheats (gear "Mods / cheats", DmActivity.showD3Mods): offline seasons, season buffs and cheats.
// This file contains code derived from d3hack (https://github.com/god-jester/d3hack, studied at c0f0dbd), Copyright (C) god-jester and the
// d3hack contributors, distributed with the GNU General Public License version 2 text and no version named for the program (text:
// hle_d3_mods.LICENSE next to this file; GPL-2 section 9: any version may be chosen). d3hack is an
// exlaunch module for the Switch game build 2.7.6.90885; its sources carry no per-file headers. Ported from it (C++ -> C, our hook
// mechanism, 2.7.7 addresses): the hooks MoveSpeed / AttackSpeed / EquipAny (d3/hooks/util.hpp), ForceAncient + AugmentSpecifier
// (util.hpp), the sInitializeWorld hook with EnableGod / FlagsAreFun (main.cpp, d3/_lobby.hpp), the offline-season / community-event
// XVar setup (d3/patches.cpp PatchDynamicSeasonal / PatchDynamicEvents / PatchBase), the cheat patches (PortCheatCodes, PatchBase) and
// the offline Challenge Rift (d3/hooks/debug.hpp StorageGetPubFile / ChallengeRiftCallback / the reward hooks, d3/util.cpp
// PopulateChallengeRiftData; data: examples/config/d3hack-nx/rift_data -> hle_d3_rift_data.inc).
// The port as a whole is GPL-3.0-or-later since 0.2.26 (see README "License").
//
// Everything is off by default; the front end passes the choices at start:
//   DM_D3_SEASON=<n>     offline season n (1..99; unset / 0 = off): the game's season XVars Season.Num = n, Season.State = 1 (in progress),
//                        Season.OverrideEnabled = 1, and the checks that need Blizzard's servers answer "yes" (see k_sites "season");
//                        with it the mailbox opens offline ("mail") and the Diablo Network status label is hidden ("netlabel")
//   DM_D3_BUFFS=<list>   season buffs / events, comma separated keys of k_buffs (any number at once: every buff is its own XVar and the
//                        game checks each one on its own), "auto" = the theme of season DM_D3_SEASON (k_themes: seasons 14..37); unset = off.
//                        Buffs apply to every hero (CommunityEventSeasonOnly = 0)
//   DM_D3_CHEATS=<list>  comma separated keys: code patches of k_sites (difficulty, portal, cooldowns, craft, cube, gem100, gemfast,
//                        resource, orbs, legendary, easykill, reddmg) and hooks (god, dbgflags, equipany; legendary drops: ancient,
//                        primal, noprimal, noancient); unset = none
//   DM_D3_SPEED=<x>      movement speed multiplier (d3hack MovementSpeedMultiplier; 1 / unset = off, 0.1..10)
//   DM_D3_ATKSPEED=<x>   attack speed multiplier (d3hack AttackSpeedMultiplier; 1 / unset = off, 0.1..10)
//   DM_D3_CHALLENGE=1    Challenge Rift offline (see "Challenge Rift" below); unset / 0 = off
//   DM_D3_ERRLOG=1 / 0   the game's error messages in our log (d3hack [debug] EnableErrorTraces): on with any mod above, 0 = never, 1 = always
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
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

// ---- code sites: original words -> patch words, grouped by feature (the season group is DM_D3_SEASON, the others DM_D3_CHEATS keys)
#define NW 3
typedef struct { const char *feat; uint32_t off; int n; uint32_t orig[NW], patch[NW]; } site_t;
static const site_t k_sites[] = {
    // offline season (d3hack PatchDynamicSeasonal / PatchBase)
    {"season", 0x5ff0c, 1, {0x12000100}, {0x52800020}},                           // IsSeasonsInitialized: and w0,w8,#1 -> mov w0,#1
    {"signin", 0x56b10, 2, {0xa9be4ff4, 0xa9017bfd}, {0x52800020, 0xd65f03c0}},   // IsSignedInOnline(user) -> 1 (d3hack PatchBase, always on there;
                                                                                  //   here with the season: hero create 0x351160 refuses a seasonal hero
                                                                                  //   offline, "SeasonNotAvailableOnline"; and with the Challenge Rift)
    {"season", 0x351ccc, 1, {0x54000181}, {0x52800021}},                          // hero create's season-confirm callback 0x351cc0: b.ne -> mov w1,#1
    {"season", 0x1bf5f0, 2, {0xd10403ff, 0xf90063f7}, {0x52800000, 0xd65f03c0}},  // ValidateHeroForPartyMember -> 0 (no error)
    {"season", 0x61620, 2, {0xf81e0ff3, 0xa9017bfd}, {0x52800000, 0xd65f03c0}},   // main / pause menu "Connect to Diablo Servers" entry -> hidden
    {"season", 0x1bd140, 1, {0xb9409115}, {0x52800015}},                          // SetGameParamsForHero, hero without data: season_created =
                                                                                  //   ldr w21,[x8,#0x90] -> mov w21,#n (n filled in at install)
    // with the offline season (d3hack hooks/season_events.hpp MailboxCheckLobbyConnection; PatchBase "hideonline" 04..11)
    {"mail", 0x1b0600, 2, {0xd10443ff, 0xa90e57fc}, {0x52800020, 0xd65f03c0}},     // the mailbox's lobby-connection check 0x1b0600 (+0) -> 1: the mail opens
                                                                                   //   offline (a hero's items come by mail when its season is swapped)
    {"netlabel", 0x3628dc, 1, {0x320003e1}, {0x52800001}},                         // main menu (d3hack 0x3627ec, +0xf0): "connected" icon SetVisible(1) -> (0)
    {"netlabel", 0x3628f0, 1, {0x320003e1}, {0x52800001}},                         //   "disconnected" icon SetVisible(1) -> (0)
    {"netlabel", 0x362930, 2, {0xf9408908, 0xaa0003e1}, {0xf9402908, 0x52800001}}, //   the status text: SetText(str) -> SetVisible(0)
    {"netlabel", 0x221324, 1, {0x320003e1}, {0x52800001}},                         // pause menu (+0): the same three
    {"netlabel", 0x221338, 1, {0x320003e1}, {0x52800001}},
    {"netlabel", 0x221378, 2, {0xf9408908, 0xaa0003e1}, {0xf9402908, 0x52800001}},
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
    {"easykill", 0x97f65c, 2, {0x528bf529, 0x72ab0c69}, {0x528f78e9, 0x72abb289}},     // damage against an ACTOR_EASYKILL actor's target (god mode sets the
                                                                                       //   bit): 1e15 (0x58635fa9) -> 1.33742e18 (0x5d947bc7)   0x97f55c +0x100
    {"reddmg", 0xbb550, 1, {0xb9404789}, {0x52801fe9}},                                // floating damage numbers: colour = ldr w9,[x28,#0x44] -> red (d3hack's
                                                                                       //   inline hook at 0xbb5e8 writes red through x3; here the load is
                                                                                       //   patched, the fading alpha byte the game writes after it stays)   +0
};
static const char *const k_patch_cheats[] = {"difficulty", "portal", "cooldowns", "craft", "cube", "gem100", "gemfast", "resource", "orbs", "legendary",
                                             "easykill", "reddmg"};

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
    // Challenge Rift (see "Challenge Rift offline" below)
    F_PUBFILE    = 0x066b10,   // h void StorageGetPublisherFile(int user, const char *name, callback **)                              0x066b10 +0
    F_PUBFILE_DONE = 0x070850, // c the storage layer's "file arrived" entry (callback **, StorageResult *, {blz::string *, ctrl *} *)  (not in d3hack)
    F_CR_DATA    = 0x185f70,   // h void sOnGetChallengeRiftData(void *bind, StorageResult *, ChallengeData *, WeeklyChallengeData *)  0x185f70 +0
    F_CR_SAVE    = 0x04edc0,   // h void ChallengeRiftRewardEarned_SaveToProfile(int user, ConsoleChallengeRiftReward *)               0x04edc0 +0
    F_CR_GRANT   = 0x7764e0,   // h void ChallengeRiftGrantRewardToPlayerIfEligible(Player *)                                          0x7763e0 +0x100
    F_CR_REWARDS = 0x775cd0,   // h void ChallengeRiftHandleChallengeRewards(Player *, int total_time_ms, u32 *reward_screen_info)     0x775bd0 +0x100
    F_SEASON_NUM = 0x72e090,   // c u32 OnlineServiceGetSeasonNum(void)                                                                0x72df90 +0x100
    F_IS_SEASONAL = 0x4c8df0,  // c BOOL GameIsSeasonal(void)                                                                          0x4c8cf0 +0x100
    F_GAMEGLOBALS = 0x7b2990,  // c SGameGlobals *SGameGlobalsGet(void)                                                                0x7b2890 +0x100
    F_CR_FAILED  = 0x185aa0,   // h void (const char *error): the Challenge Rift screen's error (d3hack sym_print_challenge_rift_failed)   0x185aa0 +0
    F_ERRMSG     = 0xa2ad70,   // h DisplayErrorMessage(ErrorCode, const char *msg) (d3hack sym_print_error; 0xa2ad60 DisplayInternalError(msg, file,
                               //   line, code) jumps here)                                                                               0xa2ac70 +0x100
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
    {F_PUBFILE,   {0xd10103ff, 0xf9000bf5, 0xa9024ff4, 0xa9037bfd}}, {F_PUBFILE_DONE, {0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
    {F_CR_DATA,   {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}}, {F_CR_SAVE,  {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
    {F_CR_GRANT,  {0xd10203ff, 0xf90023f7, 0xa90557f6, 0xa9064ff4}}, {F_CR_REWARDS, {0xf81a0ffb, 0xa90167fa, 0xa9025ff8, 0xa90357f6}},
    {F_SEASON_NUM, {0x900050e0, 0xf9463000, 0x140be4f6, 0x00000000}}, {F_IS_SEASONAL, {0xa9bf7bfd, 0x910003fd, 0xd0006408, 0xf9446908}},
    {F_GAMEGLOBALS, {0xa9bf7bfd, 0x910003fd, 0x90004cc8, 0xf9446908}}, {F_CR_FAILED, {0xd10103ff, 0xf90013f3, 0xa9037bfd, 0x9100c3fd}},
    {F_ERRMSG,    {0xf81c0ffc, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
};
// attribute ids (d3hack attributes.hpp; checked in 2.7.7 against the attribute table the game builds at 0x67b390's callers: id * 0x40)
enum { A_INVULNERABLE = 0x69, A_APS_TOTAL = 0xc9, A_IMMUNE_KNOCKBACK = 0x12d, A_STUN_IMMUNE = 0x134, A_FREEZE_IMMUNE = 0x13a,
       A_SLOWDOWN_IMMUNE = 0x14b, A_GETHIT_IMMUNE = 0x14c, A_FEAR_IMMUNE = 0x231, A_IMMUNE_CHARM = 0x2c7, A_IMMUNE_BLIND = 0x2c8,
       A_GOD = 0x44d, A_ITEMS_INDESTRUCTIBLE = 0x4c0, A_RECEIVED_SEASONAL_LEGENDARY = 0x519 };
enum { ACTOR_GOD_MODE_BIT = 0, ACTOR_EASYKILL_BIT = 1, ACTOR_NO_COLLISION_BIT = 4 };
// struct offsets (d3hack types/common.hpp). LootSpecifier as LootRollForAncientLegendary 0x88dee0 uses it: +0x68 set = return at once (no
// ancient), +0x64 set = the crafter's fixed chance, +0x6c set = stop after "ancient" (no primal roll), +0xd8 the rank it writes. So the
// SharedLooteeParams flags sit 4 bytes after d3hack's struct (its commented-out first field nLevel exists: +0x28 is compared with 70 there):
// d3hack's bDisableAncientDrops (+0x64) is really bForceCrafterAncientLegendaryChance, its bDisablePrimalAncientDrops (+0x68) is
// bDisableAncientDrops, and bDisablePrimalAncientDrops is +0x6c.
enum { PLAYER_ID_ACD = 0x4, ACD_ID_OWNER = 0x14, LS_CRAFTER_CHANCE = 0x64, LS_NO_ANCIENT = 0x68, LS_NO_PRIMAL = 0x6c, LS_ANCIENT_RANK = 0xd8 };
static const struct { uint32_t off, w; } k_loot_words[] = {   // the reads named above
    {0x88def8, 0xb9406808}, {0x88defc, 0x340000e8}, {0x88df70, 0xb9406668}, {0x88e02c, 0xb900da68}, {0x88e034, 0xb9406e68}, {0x88e038, 0x35fff648},
};

// ---- XVars (main bss; vtable = *(GOT) + 0x10; name copied to +8 by the registration 0xa26b30; value at +0xb0)
enum { GOT_VT_BOOL = 0x115a388, GOT_VT_U32 = 0x115a398, F_BOOL_SET = 0xa26fc0, F_U32_SET = 0xa273c0,
       GOT_SEASON_NUM = 0x114ac60, V_SEASON_NUM = 0x1a1bd78, GOT_SEASON_STATE = 0x114ac68, V_SEASON_STATE = 0x1a1be30,
       GOT_SEASON_OVR = 0x11590d8, V_SEASON_OVR = 0x1a1c1c8,
       GOT_EV_ENABLED = 0x114acd8, V_EV_ENABLED = 0x18ffbe0, GOT_EV_SEASONONLY = 0x114ace8, V_EV_SEASONONLY = 0x18ffc98,
       GOT_BUFF_START = 0x114acb8, V_BUFF_START = 0x1771758, GOT_BUFF_END = 0x114acc0, V_BUFF_END = 0x1771760,
       GOT_EGG_FLAG = 0x114ae38, V_EGG_FLAG = 0x1771820,
       GOT_CR_ENABLED = 0x1158fd8, V_CR_ENABLED = 0x1a1c4a8 };   // d3hack xvar_challengerift_enabled_ptr (same GOT slot)
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
    {"igr",         0x18ff900, "IgrEnabled", 0x1154bf0, 0},              // d3hack event_igr (config IgrEnabled: the Internet Game Room buff)
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
static int g_cr;                  // Challenge Rift offline on (the publisher-file hook is in)
static int g_xv_sb, g_xv_cr;      // XVars checked and ours to set: season / buffs, ChallengeRiftEnabled
static int g_god, g_dbgflags, g_rank;   // hooks' settings (g_rank: 0 off, 1 ancient, 2 primal)
static int g_deny;                      // legendary drops: 1 no primal, 2 no ancient (and so no primal)
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

static int signin(void) {   // IsSignedInOnline -> 1, once for the features that need it
    static int done;
    if (!done) done = patch_feature("signin", "signed in online") ? 1 : -1;
    return done > 0;
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
// d3hack hooks/util.hpp ForceAncient (replaces LootRollForAncientLegendary) + AugmentSpecifier ([loot_modifiers] AncientRank). Only the
// ancient fields are written. d3hack also writes ForcedILevel / TieredLootRunLevel / DisableTormentDrops / DisableTormentCheck /
// SuppressGiftGeneration here; those are not ported: its struct puts them 4 bytes before the real fields (see LS_* above), and at this step
// the item and its level are already chosen, so no effect of them could be confirmed in the code.
static void h_ancient(uint64_t spec, int32_t looter) {
    *(int32_t *)(uintptr_t)(spec + LS_CRAFTER_CHANCE) = 0;    // what d3hack writes as bDisableAncientDrops = 0
    *(int32_t *)(uintptr_t)(spec + LS_NO_ANCIENT) = 0;        // what d3hack writes as bDisablePrimalAncientDrops = 0
    *(int32_t *)(uintptr_t)(spec + LS_ANCIENT_RANK) = g_rank; // eAncientRank = AncientRankValue (1 ancient, 2 primal)
    uint64_t acd = GFN(p_id_t, F_ACDGET)(looter);
    if (acd) GFN(seti_t, F_ATTRSETI)(acd, A_RECEIVED_SEASONAL_LEGENDARY, 0);   // tKey.nValue = RECEIVED_SEASONAL_LEGENDARY; ACD_AttributesSetInt(.., 0)
}
// d3hack [loot_modifiers] DisableAncientDrops / DisablePrimalAncientDrops, at the offsets the game reads (LS_* above): the flag is set and the
// game's own roll runs (no ancient: it returns at once; no primal: the ancient roll stays, the primal one is skipped)
static void (*o_ancient)(uint64_t, int32_t);
static void h_deny(uint64_t spec, int32_t looter) {
    *(int32_t *)(uintptr_t)(spec + (g_deny == 2 ? LS_NO_ANCIENT : LS_NO_PRIMAL)) = 1;
    o_ancient(spec, looter);
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
// ---- the game's error messages in our log (d3hack hooks/debug.hpp Print_Error / Print_ChallengeRiftFailed, [debug] EnableErrorTraces)
static const char *peek_str(uint64_t p, char *buf, size_t n) {   // a guest string that may be a bad pointer: copied through a pipe (EFAULT, no fault)
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static int fd[2] = {-1, -1};
    if (p < 0x10000) return "(null)";
    pthread_mutex_lock(&mu);
    size_t got = 0;
    if (fd[0] >= 0 || !pipe(fd))
        while (got < n - 1) {
            size_t want = 0x1000 - (size_t)((p + got) & 0xfff); if (want > n - 1 - got) want = n - 1 - got;   // one page at a time
            ssize_t w = write(fd[1], (const void *)(uintptr_t)(p + got), want);
            if (w <= 0 || read(fd[0], buf + got, (size_t)w) != w) break;
            if (memchr(buf + got, 0, (size_t)w)) { got += (size_t)w; break; }
            got += (size_t)w;
        }
    pthread_mutex_unlock(&mu);
    buf[got] = 0;
    return got ? buf : "(unreadable)";
}
static void (*o_errmsg)(uint32_t, uint64_t, uint64_t, uint64_t);
static void h_errmsg(uint32_t code, uint64_t msg, uint64_t a2, uint64_t a3) {
    static _Atomic int n; char b[400];
    if (atomic_fetch_add(&n, 1) < 64) LOG("【红】[d3mods] game error message, code %#x: %s", code, peek_str(msg, b, sizeof b));
    o_errmsg(code, msg, a2, a3);
}
static void (*o_cr_failed)(uint64_t);
static void h_cr_failed(uint64_t err) {
    char b[128]; LOG("【红】[d3mods] challenge rift: the game refused it: %s", peek_str(err, b, sizeof b));
    o_cr_failed(err);
}
// ---- Challenge Rift offline (DM_D3_CHALLENGE=1; d3hack [challenge_rifts])
// How the game gets a Challenge Rift (2.7.7): the menu (0x181700) asks 0x69ab0 -> 0x69990 -> StorageGetPublisherFile(user,
// "challengerift_config.dat", cb) 0x66b10; the file's callback 0x726d0 parses a D3.ChallengeRifts.ChallengeData (it must have a challenge
// number), 0x72880 then asks for "challengerift_<number % 10>.dat", 0x72a70 parses that as a D3.Leaderboard.WeeklyChallengeData (rift, hero
// and account snapshot required; a snapshot without its completion time in ms gets it from the seconds) and calls sOnGetChallengeRiftData
// 0x185f70, which wants start <= now < end and opens the Challenge Rift screen. The storage layer answers from its network thread through
// 0x70850, which queues the user's callback for the main thread (0x7b970).
// d3hack hooks sOnGetChallengeRiftData and overwrites a failed answer with its SD-card files (its older, commented-out way hooked
// StorageGetPublisherFile). Here the hook is StorageGetPublisherFile itself: the two challenge files are answered at once from the bundled
// d3hack dumps through the game's own 0x70850, so nothing depends on how the storage layer fails without a network and the game's own two
// parse steps run unchanged. Other publisher files go to the game's function.
//   config: challenge_number = the week number (unix time / 604800, a new one every Thursday 00:00 UTC), start 0, last broadcast 0,
//           end 0x7fffffff (d3hack's values, so the "not yet started / ended" checks pass with any clock), hash / orb multiplier from the dump
//   data:   bundled file (week number % count); DM_D3_CHALLENGE_RIFT=<n> picks file n instead (testing)
// The reward: ChallengeRiftHandleChallengeRewards 0x775cd0 gives it when the run beat the target time and the player's last rewarded
// challenge number (Player +0x98, from the profile) is below the current one (SGameGlobals +0x2198); SaveToProfile 0x4edc0 stores the
// number in the profile only when it is above the stored one. d3hack picks a random number 0..900, so it has to bypass that compare
// (0x4ee18 / 0x4ef28) and rewinds the player's last number around the game's functions, which makes the cache farmable. With the week
// number none of that is needed: the game's own rule gives one cache a week (and the number is above any real one, ~3000 against ~450).
// Ported from d3hack as they are: the season tag (SaveToProfile sets season_earned only when the lobby is connected, the grant in town
// 0x7764e0 wants season_earned == the current season in a seasonal game: tag on save, re-tag an untagged entry before the grant).
typedef struct { const uint8_t *p; uint64_t size, cap; uint8_t sso[16]; } blz_str_t;   // blz::string: m_elements, m_size, m_capacity, m_storage
#include "hle_d3_rift_data.inc"
enum { NRIFT = sizeof k_rifts / sizeof *k_rifts,
       CD_BITS = 0x10, CD_START = 0x18, CD_NUMBER = 0x28, CD_END = 0x30,   // ChallengeData (d3hack symbols/constructors.hpp; reads of 0x185f70 / 0x728e4)
       RW_BITS = 0x10, RW_CHALLENGE = 0x30, RW_SEASON = 0x38,              // ConsoleChallengeRiftReward: _has_bits_, challenge_rift_, season_earned_ (bit 8)
       PL_LAST_REWARD = 0x98, PL_REWARDS_END = 0xdd88, PL_REWARDS_HEAD = 0xdd90, NODE_NEXT = 0x8, NODE_SEASON = 0x48,   // Player, its reward list
       GG_CHALLENGE = 0x2198, GG_TARGET_MS = 0x220c };                     // SGameGlobals
// single words that pin the layouts used below (the hooked / called functions' first words are in k_fns)
static const struct { uint32_t off, w; } k_cr_words[] = {
    {0x66b2c, 0xaa0203f3}, {0x66b30, 0xaa0103f4}, {0x66b40, 0xf9400273},   // StorageGetPublisherFile: x19 = x2, x20 = x1 (name), x19 = *x19 (callback)
    {0x69a08, 0x91116c21}, {0x69a1c, 0x97fff43d},                          // 0x69990: x1 = "challengerift_config.dat"; bl 0x66b10
    {0x729bc, 0x97ffd055},                                                 // 0x72880: bl 0x66b10 ("challengerift_%d.dat")
    {0x70864, 0xb9400035}, {0x70868, 0xa9404c56}, {0x70884, 0xf9400014},   // 0x70850: w21 = *x1 (result), {x22, x19} = *x2 (string, ctrl; ctrl may be 0), x20 = *x0 (callback)
    {0x70a60, 0x91002001}, {0x70a64, 0xf8410408}, {0x70a68, 0xf8418503}, {0x70a6c, 0xaa0003e2}, {0x70a70, 0xaa0803e0}, {0x70a74, 0xd61f0060},
                                                                           //   its queued task: callback->fn(callback + 0x18, &result, &{string, ctrl})
    {0x726e4, 0xb9400036}, {0x726e8, 0xa9404c55}, {0x72740, 0x942d0144}, {0x7274c, 0x34000168},   // config callback: result, {string, ctrl}; bl ParsePartialFromString; number != 0
    {0x728e4, 0xb9402848},                                                 // 0x72880: w8 = ChallengeData +0x28 (challenge_number_)
    {0x72a84, 0xb9400028}, {0x72a88, 0xa9404c55}, {0x72af4, 0x942d0057},   // data callback: the same shape
    {0xbb2c60, 0xa9402428},                                                // ParsePartialFromString 0xbb2c50 (d3hack 0xbb2b50 +0x100): {ptr, size} = *x1
    {0x185f94, 0xb9400013}, {0x185f9c, 0xb9400034}, {0x186014, 0xf9400ea8}, {0x18606c, 0xf9401aa8},   // sOnGetChallengeRiftData: *x0, *x1, start +0x18, end +0x30
    {0x4edf8, 0xb9403288}, {0x4ee10, 0xb9401929}, {0x4ee18, 0x54000ee9}, {0x4ef28, 0x54fff7a8},   // SaveToProfile: reward +0x30 against the profile's number (d3hack's two gates, left alone)
    {0x4eeb4, 0x321d0108}, {0x4eebc, 0xb9003a80},                          //   has bit 8, season_earned +0x38
    {0x7764f8, 0x529bb108}, {0x776500, 0x529bb208}, {0x776518, 0xf9400694}, {0x77652c, 0xb9404a96}, {0x776530, 0x97feded8},   // grant: list end / head, next, season_earned, bl season num
    {0x775d78, 0xb961981a}, {0x775e0c, 0xb9620ec8}, {0x775e14, 0xb900027a}, {0x775e30, 0xb9409a88}, {0x775e60, 0xb9000e68},   // rewards: +0x2198, +0x220c, info[0], Player +0x98, info[3]
};
static uint64_t pb_varint(const uint8_t *b, size_t n, size_t *i, int *ok) {   // d3hack debug.hpp ReadVarint
    uint64_t v = 0;
    for (unsigned sh = 0; *i < n && sh < 64; sh += 7) { uint8_t c = b[(*i)++]; v |= (uint64_t)(c & 0x7f) << sh; if (!(c & 0x80)) return v; }
    *ok = 0; return 0;
}
// Top-level fields of a protobuf message (d3hack debug.hpp ParseWeeklyChallengeTopLevel, generalised): val[f] = the varint / fixed32 value or
// the length of a length-delimited field f < nf; the return value has bit f set for every such field seen and bit 31 when well formed.
static uint32_t pb_top(const uint8_t *b, size_t n, uint64_t *val, unsigned nf) {
    uint32_t seen = 0; size_t i = 0; int ok = 1;
    while (i < n) {
        uint64_t tag = pb_varint(b, n, &i, &ok), v = 0; if (!ok) return 0;
        unsigned f = (unsigned)(tag >> 3), w = (unsigned)(tag & 7);
        if (w == 0) { v = pb_varint(b, n, &i, &ok); if (!ok) return 0; }
        else if (w == 1) { if (n - i < 8) return 0; i += 8; }
        else if (w == 5) { if (n - i < 4) return 0; uint32_t x; memcpy(&x, b + i, 4); v = x; i += 4; }
        else if (w == 2) { v = pb_varint(b, n, &i, &ok); if (!ok || v > n - i) return 0; i += (size_t)v; }
        else return 0;
        if (f < nf && f < 31) { seen |= 1u << f; if (val) val[f] = v; }
    }
    return seen | 1u << 31;
}
static size_t pb_put(uint8_t *o, uint32_t field, uint64_t v) {   // a varint field
    size_t n = 0; o[n++] = (uint8_t)(field << 3);
    do { uint8_t c = v & 0x7f; v >>= 7; o[n++] = (uint8_t)(c | (v ? 0x80 : 0)); } while (v);
    return n;
}
static uint8_t g_cr_cfg[64]; static blz_str_t g_cr_cfg_str, g_cr_dat_str;   // what the game's parsers read (never freed: the answers carry no owner)
static uint64_t g_cr_week; static unsigned g_cr_file;
static unsigned cr_pick(uint64_t week) {
    const char *e = getenv("DM_D3_CHALLENGE_RIFT");
    return e && *e ? (unsigned)strtoul(e, NULL, 10) % NRIFT : (unsigned)(week % NRIFT);
}
typedef void (*pubdone_t)(uint64_t *cb, int32_t *result, uint64_t *str);
static void (*o_pubfile)(int32_t, const char *, uint64_t *);
static void h_pubfile(int32_t user, const char *name, uint64_t *cb) {
    int cfg = name && strstr(name, "challengerift_config.dat") != NULL, dat = !cfg && name && strstr(name, "challengerift_") != NULL;
    if ((!cfg && !dat) || !cb || !*cb) {
        static int n; if (n++ < 16) LOG("[d3mods] publisher file \"%s\" asked (user %d): left to the game", name ? name : "(null)", user);
        o_pubfile(user, name, cb); return;
    }
    blz_str_t *s;
    if (cfg) {
        g_cr_week = (uint64_t)time(NULL) / 604800u; g_cr_file = cr_pick(g_cr_week);
        uint64_t f[7] = {0}; uint32_t seen = pb_top(k_rift_cfg, sizeof k_rift_cfg, f, 7);
        size_t n = pb_put(g_cr_cfg, 1, g_cr_week);                 // challenge_number
        n += pb_put(g_cr_cfg + n, 2, 0);                           // challenge_start_unix_time
        n += pb_put(g_cr_cfg + n, 3, 0);                           // challenge_last_broadcast_unix_time
        n += pb_put(g_cr_cfg + n, 4, 0x7fffffff);                  // challenge_end_unix_time_console
        n += pb_put(g_cr_cfg + n, 5, f[5]);                        // challenge_hash
        if (seen >> 6 & 1) { g_cr_cfg[n++] = 6 << 3 | 5; uint32_t x = (uint32_t)f[6]; memcpy(g_cr_cfg + n, &x, 4); n += 4; }   // challenge_nephalem_orb_multiplier
        g_cr_cfg_str.p = g_cr_cfg; g_cr_cfg_str.size = g_cr_cfg_str.cap = n; s = &g_cr_cfg_str;
        LOG("[d3mods] challenge rift: \"%s\" asked (user %d) -> config answered: challenge %llu (this week), rift file %02u of %d", name, user,
            (unsigned long long)g_cr_week, g_cr_file, (int)NRIFT);
    } else {
        if (!g_cr_week) { g_cr_week = (uint64_t)time(NULL) / 604800u; g_cr_file = cr_pick(g_cr_week); }
        g_cr_dat_str.p = k_rifts[g_cr_file].p; g_cr_dat_str.size = g_cr_dat_str.cap = k_rifts[g_cr_file].n; s = &g_cr_dat_str;
        LOG("[d3mods] challenge rift: \"%s\" asked (user %d) -> bundled challengerift_%02u.dat answered (%u bytes)", name, user, g_cr_file, k_rifts[g_cr_file].n);
    }
    uint64_t str[2] = {(uint64_t)(uintptr_t)s, 0}; int32_t result = 0;   // STORAGE_SUCCESS; no owner: the string is static
    GFN(pubdone_t, F_PUBFILE_DONE)(cb, &result, str);
}
// d3hack ChallengeRiftCallback's place; here it only reports (the files were answered above)
static void (*o_cr_data)(uint64_t, int32_t *, uint64_t, uint64_t);
static void h_cr_data(uint64_t bind, int32_t *res, uint64_t conf, uint64_t data) {
    int32_t r = res ? *res : -1;
    if (r || !conf || !data) LOG("【红】[d3mods] challenge rift: the data callback got result %d (0 = ok; 2 = not connected, 3 = no challenge, 4 = bad file)", r);
    else LOG("[d3mods] challenge rift: data callback ok: challenge %u, start %lld, end %lld, config bits %#x, weekly data bits %#x (7 = rift + hero + account)",
             *(const uint32_t *)(uintptr_t)(conf + CD_NUMBER), (long long)*(const int64_t *)(uintptr_t)(conf + CD_START),
             (long long)*(const int64_t *)(uintptr_t)(conf + CD_END), *(const uint32_t *)(uintptr_t)(conf + CD_BITS), *(const uint32_t *)(uintptr_t)(data + CD_BITS));
    o_cr_data(bind, res, conf, data);
}
typedef uint32_t (*u_void_t)(void);
// d3hack ChallengeRiftHandleChallengeRewardsHook's place, without its re-arming of the player's last rewarded challenge: report only
static void (*o_cr_rewards)(uint64_t, int32_t, uint32_t *);
static void h_cr_rewards(uint64_t player, int32_t ms, uint32_t *info) {
    uint64_t g = GFN(p_void_t, F_GAMEGLOBALS)();
    uint32_t last = player ? *(const uint32_t *)(uintptr_t)(player + PL_LAST_REWARD) : 0, cur = g ? *(const uint32_t *)(uintptr_t)(g + GG_CHALLENGE) : 0;
    int32_t target = g ? *(const int32_t *)(uintptr_t)(g + GG_TARGET_MS) : 0;
    o_cr_rewards(player, ms, info);
    if (info) LOG("[d3mods] challenge rift finished: challenge %u, time %d ms, target %d ms, the player's last rewarded challenge %u -> status %u "
                  "(1 = reward earned, 2 = already rewarded for this challenge, 0 = slower than the target), reward item %#x", cur, ms, target, last, info[3], info[1]);
}
// d3hack ChallengeRiftRewardEarned_SaveToProfileHook: tag the reward with the season when there is one (the game does it only with the lobby connected)
static void (*o_cr_save)(int32_t, uint64_t);
static void h_cr_save(int32_t user, uint64_t reward) {
    if (!reward) { o_cr_save(user, reward); return; }
    uint32_t *bits = (uint32_t *)(uintptr_t)(reward + RW_BITS), *earned = (uint32_t *)(uintptr_t)(reward + RW_SEASON);
    uint32_t season = GFN(u_void_t, F_SEASON_NUM)(), was = *bits;
    if (season) { *bits |= 8u; *earned = season; }
    o_cr_save(user, reward);
    LOG("[d3mods] challenge rift: reward of challenge %u for user %d passed to the profile save: season %u, season_earned %u, bits %#x -> %#x%s",
        *(const uint32_t *)(uintptr_t)(reward + RW_CHALLENGE), user, season, *earned, was, *bits,
        was & 4 ? "" : *bits & 4 ? " (stored: time stamp set)" : " (NOT stored: no signed-in profile, not a challenge game, or the profile already has this challenge number or a higher one)");
}
// d3hack ChallengeRiftGrantRewardToPlayerIfEligibleHook: in a season, an untagged reward (season_earned 0) gets the current season when no
// reward carries it, so a seasonal hero can take it
static void (*o_cr_grant)(uint64_t);
static void h_cr_grant(uint64_t player) {
    if (player) {
        uint32_t season = GFN(u_void_t, F_SEASON_NUM)();
        uint64_t end = player + PL_REWARDS_END, node = *(const uint64_t *)(uintptr_t)(player + PL_REWARDS_HEAD), untagged = 0;
        int n = 0, current = 0;
        for (; node && node != end && n < 4096; node = *(const uint64_t *)(uintptr_t)(node + NODE_NEXT), n++) {
            uint32_t earned = *(const uint32_t *)(uintptr_t)(node + NODE_SEASON);
            if (season && earned == season) { current = 1; break; }
            if (!earned && !untagged) untagged = node;
        }
        int retag = season && n < 4096 && !current && untagged;
        if (retag) *(uint32_t *)(uintptr_t)(untagged + NODE_SEASON) = season;
        static int logs; if ((n || current) && logs++ < 32)
            LOG("[d3mods] challenge rift: reward grant check: reward(s) waiting in the profile (%d looked at), season %u, seasonal game %d%s", n + current, season,
                GFN(v_int_t, F_IS_SEASONAL)(), retag ? ", an untagged reward tagged with the season" : "");
    }
    o_cr_grant(player);
}
static void cr_install(void) {
    for (unsigned i = 0; i < sizeof k_cr_words / sizeof *k_cr_words; i++)
        if (g_m->text_end < k_cr_words[i].off + 4u || *(const uint32_t *)(g_m->base + k_cr_words[i].off) != k_cr_words[i].w) {
            LOG("【红】[d3mods] challenge rift: main+%#x differs (not D3 update 2.7.7, build 2607a74f): Challenge Rift offline off", k_cr_words[i].off); return; }
    if (!fn_ok(F_PUBFILE_DONE) || !fn_ok(F_SEASON_NUM) || !fn_ok(F_IS_SEASONAL) || !fn_ok(F_GAMEGLOBALS)) { LOG("【红】[d3mods] Challenge Rift offline off"); return; }
    uint64_t f[7] = {0};
    if ((pb_top(k_rift_cfg, sizeof k_rift_cfg, f, 7) & 0x22) != 0x22) { LOG("【红】[d3mods] challenge rift: bundled config malformed: Challenge Rift offline off"); return; }
    for (unsigned i = 0; i < NRIFT; i++)
        if ((pb_top(k_rifts[i].p, k_rifts[i].n, NULL, 5) & 0xe) != 0xe) { LOG("【红】[d3mods] challenge rift: bundled file %02u malformed: Challenge Rift offline off", i); return; }
    if (!signin()) return;
    if (!hook(F_PUBFILE, (void *)h_pubfile, (void **)&o_pubfile, "challenge rift files from the bundled dumps")) return;
    g_cr = 1;
    hook(F_CR_DATA, (void *)h_cr_data, (void **)&o_cr_data, "challenge rift data callback (log)");
    hook(F_CR_FAILED, (void *)h_cr_failed, (void **)&o_cr_failed, "challenge rift error (log)");
    hook(F_CR_REWARDS, (void *)h_cr_rewards, (void **)&o_cr_rewards, "challenge rift rewards (log)");
    hook(F_CR_SAVE, (void *)h_cr_save, (void **)&o_cr_save, "challenge rift reward save (season tag)");
    hook(F_CR_GRANT, (void *)h_cr_grant, (void **)&o_cr_grant, "challenge rift reward grant (season tag)");
    uint64_t week = (uint64_t)time(NULL) / 604800u;
    LOG("[d3mods] Challenge Rift offline on: %d bundled rifts, this week = challenge %llu -> rift file %02u; one reward a week by the game's own rule",
        (int)NRIFT, (unsigned long long)week, cr_pick(week));
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
    const char *ecr = getenv("DM_D3_CHALLENGE");
    int challenge = ecr && *ecr && strcmp(ecr, "0");
    float move = env_mult("DM_D3_SPEED"), atk = env_mult("DM_D3_ATKSPEED");
    const char *el = getenv("DM_D3_ERRLOG");
    int errlog = el && *el ? strcmp(el, "0") != 0 : -1;   // -1 = with any mod
    if (!season && !buffs && !cheats && !challenge && move == 1.0f && atk == 1.0f && errlog <= 0) { LOG("[d3mods] off (no DM_D3_SEASON / BUFFS / CHEATS / CHALLENGE / SPEED / ATKSPEED)"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_m = &g_mod[i];
    if (!g_m) { LOG("【红】[d3mods] no main module: mods off"); return; }
    if (season && signin() && patch_feature("season", "offline season")) {
        g_season = season;
        patch_feature("mail", "mailbox offline");
        patch_feature("netlabel", "Diablo Network status label hidden");
    }
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
        int deny = rank ? 0 : has_key(ec, "noancient") ? 2 : has_key(ec, "noprimal") ? 1 : 0, loot_ok = 1;
        for (unsigned i = 0; (rank || deny) && i < sizeof k_loot_words / sizeof *k_loot_words; i++)
            if (*(const uint32_t *)(g_m->base + k_loot_words[i].off) != k_loot_words[i].w) { LOG("【红】[d3mods] legendary drops: main+%#x differs: off", k_loot_words[i].off); loot_ok = 0; break; }
        if (rank && loot_ok && fn_ok(F_ACDGET) && fn_ok(F_ATTRSETI) && hook(F_ANCIENT, (void *)h_ancient, NULL, rank == 2 ? "every legendary primal" : "every legendary ancient")) g_rank = rank;
        if (deny && loot_ok && hook(F_ANCIENT, (void *)h_deny, (void **)&o_ancient, deny == 2 ? "no ancient legendaries" : "no primal legendaries")) g_deny = deny;
        int god = has_key(ec, "god"), dbg = has_key(ec, "dbgflags");
        if ((god || dbg) && called_fns_ok()) {
            if (*(const uint64_t *)(g_m->base + GOT_APPGLOBALS) != (uint64_t)(uintptr_t)(g_m->base + O_APPGLOBALS)) LOG("【红】[d3mods] GOT main+%#x does not point at AppGlobals main+%#x: god / debug flags off", GOT_APPGLOBALS, O_APPGLOBALS);
            else if (hook(F_INITWORLD, (void *)h_initworld, (void **)&o_initworld, "god mode / debug flags at world setup")) { g_god = god; g_dbgflags = dbg; }
        }
    }
    if (move != 1.0f && hook(F_MOVESPEED, (void *)h_movespeed, (void **)&o_movespeed, "movement speed")) { g_move = move; LOG("[d3mods] movement speed x%.2f", move); }
    if (atk != 1.0f && fn_ok(F_ATTRSETF) && hook(F_ATKSPEED, (void *)h_atkspeed, (void **)&o_atkspeed, "attack speed")) { g_atk = atk; LOG("[d3mods] attack speed x%.2f", atk); }
    if (challenge) cr_install();
    if (errlog) hook(F_ERRMSG, (void *)h_errmsg, (void **)&o_errmsg, "game error messages (log)");
    if (g_season || g_buffs || g_cr) {
        atomic_store(&g_state, 1);
        char b[512] = ""; size_t o = 0;
        for (int i = 0; i < NBUFF; i++) if (g_buffs >> i & 1) o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", o ? "," : "", k_buffs[i].key);
        LOG("[d3mods] XVars at the first npad poll: season %d, buffs %s%s", g_season, o ? b : "none", g_cr ? ", ChallengeRiftEnabled" : "");
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
    if (g_xv_cr) {
        int was = *(volatile uint8_t *)(g_m->base + V_CR_ENABLED + 0xb0) & 1;
        set_bool(V_CR_ENABLED, 1);   // d3hack PatchBase: XVarBool_Set(&g_varChallengeRiftEnabled, true, 3)
        if (first) LOG("[d3mods] ChallengeRiftEnabled XVar %d -> 1", was);
    }
    if (!g_xv_sb) return;
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
        g_xv_sb = ok && (g_season || g_buffs);
        if (!ok && (g_season || g_buffs)) LOG("【红】[d3mods] season / buff XVars not set (the season code patches stay in)");
        g_xv_cr = g_cr && !memcmp(g_m->base + F_BOOL_SET, k_fn_bool_set, sizeof k_fn_bool_set) && xvar_ok(GOT_CR_ENABLED, V_CR_ENABLED, GOT_VT_BOOL, "ChallengeRiftEnabled");
        if (g_cr && !g_xv_cr) LOG("【红】[d3mods] ChallengeRiftEnabled XVar not set (the game's default is on; the Challenge Rift hooks stay in)");
        if (!g_xv_sb && !g_xv_cr) { atomic_store(&g_state, -1); return; }
        apply(1);
        atomic_store(&g_state, 2);
        return;
    }
    static unsigned polls;
    if (++polls % 300 == 0) apply(0);   // ~5 s at 60 polls a second
}
#endif
