// 零散服务层：追加内容 / 商店 / 错误小程序 / 敏感词过滤 / 服务器名解析 / 家长控制 / 软键盘。
// 真机上各自走 IPC 或系统小程序；宿主按单机离线给结果。纯计算的 SDK 部分（hash、util、软件 Opus）不接管，直接用原实现。
// Miscellaneous service layer: add-on content / shop / error applet / profanity filter / server name resolution / parental controls / software keyboard.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "hle.h"

// ---------- aoc 追加内容（术士君临，保留）/ ec 商店（排除）----------
// 术士君临（Reign of the Warlock）是 Switch 包里唯一的追加内容：AOC 0100726014353001 = 本体 0100726014352000 的追加内容序号 1。
// 该 DLC 只是约 100 KB 的解锁凭证，内容本体已在主 RomFS；main 不导入 MountAddOnContent，只按「有没有序号 1」判断。
// 复刻范围要求保留术士君临，所以报告已拥有序号 1；排除的是商店（ec），不是 DLC 本身。
// ---------- aoc add-on content (Reign of the Warlock, kept) / ec shop (excluded) ----------
// Reign of the Warlock is the only add-on content in the Switch package: AOC 0100726014353001 = add-on content index 1 of the base 0100726014352000.
// This DLC is only an unlock ticket of about 100 KB, the content itself is already in the main RomFS; main does not import MountAddOnContent and only judges by "is index 1 present".
// The reimplementation scope requires keeping Reign of the Warlock, so index 1 is reported as owned; what is excluded is the shop (ec), not the DLC itself.
enum { AOC_WARLOCK = 1 };
// DM_AOC_OWNED=0：测试开关，回到「无 DLC」前提（只用于对拍未装 DLC 时录的真值；缺省 = 拥有）
// DM_AOC_OWNED=0: test switch, back to the "no DLC" premise (only for lockstep comparison against ground truth recorded without the DLC installed; default = owned)
#if defined(DM_GAME_ETERNAL)   // [ETERNAL] AOC 1..5 = Rip and Tear, Deluxe, Campaign (the update makes the campaign an add-on), The Ancient Gods 1 / 2;
// their content is in the update's RomFS (base/game/dlc*): owned unless DM_AOC_OWNED=0
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); } return v; }
#elif defined(DM_GAME_D3)
// [D3] AOC index = a language pack (main 0x1099aa0: 1 english_eu, 2 english_southeast_asia, 3 spanish_eu, 4 spanish_latam, 5 french,
// 6 italian, 7 german, 8 korean, 9 portuguese_brazil, 10 russian, 11 chinese_china, 12 chinese_taiwan, 14 polish, 15 portuguese_portugal,
// 16 japanese, 17 english_us; 100..102 cosmetic dlc.*). The game lists the add-ons, mounts each as "aoc_<index>" (hle_fs.c
// h_fs_MountAoc -> DM_AOC/<index>) and reads a locale's archives there. Owned = DM_AOC/<index> holds a *_Common.cpk (the extracted
// pack RomFS, tools/d3_dlc_romfs.py); the cosmetic item packs 100..102 (30th Anniversary, Prime Evil Collection, Heritage of Arreat)
// hold only an entitlement marker NX64_AOC_<name>.data (34-35 bytes) that the game looks for in aoc_<index>:/, owned when it is
// there; DM_AOC_OWNED=0 reports none.
#include <dirent.h>
static int g_d3_aoc[32], g_d3_aoc_n = -1;
static void d3_aoc_scan(void) {
    if (g_d3_aoc_n >= 0) return;
    int n = 0; const char *a = getenv("DM_AOC");
    for (int idx = 1; idx <= 102 && a && *a && n < (int)(sizeof g_d3_aoc / sizeof *g_d3_aoc); idx = idx == 17 ? 100 : idx + 1) {
        char d[1100]; snprintf(d, sizeof d, "%s/%d", a, idx);
        DIR *dir = opendir(d); struct dirent *de; int hit = 0;
        while (dir && !hit && (de = readdir(dir))) { size_t l = strlen(de->d_name);
            hit = idx < 100 ? l > 11 && !strcmp(de->d_name + l - 11, "_Common.cpk") : !strncmp(de->d_name, "NX64_AOC_", 9) && l > 14 && !strcmp(de->d_name + l - 5, ".data"); }
        if (dir) closedir(dir);
        if (hit) { g_d3_aoc[n++] = idx; LOG("aoc: %s %d in %s", idx < 100 ? "language pack" : "item pack", idx, d); }
    }
    g_d3_aoc_n = n;
    if (!n) LOG("aoc: no language or item packs in %s/<index>", a && *a ? a : "(DM_AOC unset)");
}
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); } if (!v) return 0; d3_aoc_scan(); return g_d3_aoc_n > 0; }
#elif defined(DM_GAME_DOOM)   // [DOOM] AOC 1 = D2R "Reign of the Warlock"; Doom: no add-on content unless DM_AOC_OWNED=1
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = e && !strcmp(e, "1"); } return v; }
#else
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_AOC_OWNED=0：aoc 报告 0 个追加内容（对拍测试用）"); } return v; }
#endif
#ifdef DM_GAME_ETERNAL
static const int g_aoc_owned[] = {1, 2, 3, 4, 5};
#elif defined(DM_GAME_XC2)
// [XC2] AOC 0100E9500403900N: 1 Torna - The Golden Country, 2 New Quests, 3 New Rare Blade, 4 New Challenge Mode, 5 Helpful Items,
// 6 My Nintendo Bonus Items, 99 (0x63) Japanese voice; owned only with DM_AOC_OWNED=1 (their RomFS must be in DM_AOC/<index>)
static const int g_aoc_owned[] = {1, 2, 3, 4, 5, 6, 99};
#elif !defined(DM_GAME_D3)
static const int g_aoc_owned[] = {AOC_WARLOCK};
#endif
#ifdef DM_GAME_D3
#define AOC_LIST g_d3_aoc
#define AOC_COUNT g_d3_aoc_n
#else
#define AOC_LIST g_aoc_owned
#define AOC_COUNT (int)(sizeof g_aoc_owned / sizeof *g_aoc_owned)
#endif
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "追加内容数：已拥有术士君临 1 个（复刻范围必须保留）", int, h_aoc_Count, void) { return aoc_owned() ? AOC_COUNT : 0; }
// ListAddOnContent(out, 起始下标, 最多个数)：按已拥有序号列表 {1} 从起始下标起写出，返回写出个数
// ListAddOnContent(out, start index, max count): writes out from the owned index list {1} starting at the start index, returns the number written
HLE_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "追加内容列表：{1}（术士君临）", int, h_aoc_List, int *out, int off, int n) {
    int w = 0;
    if (!aoc_owned()) return 0;
    const int *owned = AOC_LIST;
    for (int i = off; i < AOC_COUNT && w < n; i++) out[w++] = owned[i];
    return w;
}
// 事件对象按需由 nn::os 旁表建（未触发），游戏等它只会超时 / 取到未触发
// The event object is created on demand by the nn::os side table (not signaled), the game waiting on it only times out / gets not-signaled
HLE_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", "追加内容变更事件，排除范围；绑内核事件、恒不触发（游戏会 TryWait 轮询，未初始化即中止）", void, h_aoc_GetEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListEv", "商店小程序，排除范围；剪口：空操作", void, h_ec_ShowShop, void) { LOG("ec::ShowShopAddOnContentList（剪）"); }

// ---------- err 错误小程序：宿主打印 ----------
// ---------- err error applet: printed by the host ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "错误显示小程序；宿主打印模块 / 描述号", void, h_err_ShowError, Result r) {
    LOG("err::ShowError Result=%#x（模块 %u，描述 %u）", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc 敏感词过滤：词库在系统数据存档（经 IPC 挂载）；单机无他人可见文本，恒判无敏感词 ----------
// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterC1Ev", "敏感词过滤，词库走系统存档；宿主不建", void, h_ngc_Ctor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "敏感词过滤", void, h_ngc_Dtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", "敏感词过滤初始化；宿主无词库", Result, h_ngc_Init, void *self, void *work, uint64_t sz) { (void)self; (void)work; (void)sz; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "词库版本；宿主 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "敏感词检查；恒无命中", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}

// ---------- nsd 服务器名解析 / pctl 自由通信：联网，排除范围 ----------
// ---------- nsd server name resolution / pctl free communication: networking, out of scope ----------
HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_Resolve, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool 参数只看低 8 位；返回 bool
// bool arguments only look at the low 8 bits; returns bool
HLE_FORCE("_ZN2nn4pctl25TryBeginFreeCommunicationEb", "家长控制·与他人通信，联网排除；恒不允许", unsigned, h_pctl_TryBegin, unsigned show_ui) { (void)show_ui; return 0; }
HLE_FORCE("_ZN2nn4pctl20EndFreeCommunicationEv", "家长控制·与他人通信", void, h_pctl_End, void) {}

// swkbd 软键盘：见 hle_swkbd.c
// swkbd software keyboard: see hle_swkbd.c
