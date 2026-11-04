# Game-derived information in this repository

Diablo III Mobile contains **no game code and no game data**: no executables or parts of them, no RomFS / CPK archives or
files from them, no shaders (binary or translated), no textures, art, audio, video, text or fonts, and no keys. The player
supplies the game ([GAME_FILES.md](GAME_FILES.md)) and every game-derived output (ExeFS, patched modules, extracted RomFS,
extracted `.shd` files, shader jobs, translated `.xgs` bundles, language-pack RomFS, UI name tables) is produced on the
player's machine into the git-ignored `work/` directory. The app's launcher icon (`dm/android/d3/icon.png`) is a neutral
placeholder made for this repository, not the game's icon.

To make the original code run on Android, the repository does contain *facts about* the game, found by analysing the
author's own copy: addresses, short instruction encodings, structure offsets, identifiers and format descriptions. This
page lists them so their extent is clear. They apply only to Diablo III: Eternal Collection for Switch, update v1441792
(game patch 2.7.7; `main` build id `2607a74f5df7754c…`, `sdk` build id `3ae34e689d7a35f1…`, SDK 8.2.0); the host checks the
original instruction words before it changes anything, and setup checks the build ids.

| What | Where | Extent |
|---|---|---|
| **Widescreen sites**: the offsets and original instruction words of the window-size initialisation (`mov` / `movk` of 1600×900, 1280×720 and the 16:9 aspect) and of the letterbox check, rewritten at load time when widescreen is on | `dm/host/hle_d3.c` (`dm_d3_wide_install`), [WIDESCREEN.md](WIDESCREEN.md) | ~14 instruction words, each checked before it is replaced |
| **Loading-screen detector**: addresses of the game object, its loading-screen state fields and the UI→pixel map; a few original instruction words checked first | `dm/host/hle_d3.c`, `dm/gfx/nvn/hle_nvn.c` (present crop) | Read-only, used to stretch the loading art in widescreen |
| **UI reading for touch** (focus / active control, the UI-set-focus entry point, control class vtables, the 960×720 UI space) | `dm/host/hle_d3_ui.c`, [TOUCH_DESIGN.md](TOUCH_DESIGN.md) | ~15 offsets + 3 entry points checked word for word; read-only except the documented focus call |
| **HUD geometry** (positions of the skill slots and their letter row in the console HUD, measured on screenshots) | `dm/android/app/.../D3SkillIcons.java` | Numbers only; the icons are copied from the running game's own picture on the phone, nothing is stored |
| **SDK 8.2 internals**: offsets / overloads of nnSdk functions the host serves (account, fs, oe, nfp, ldn, aoc, applets), language indices of the add-on content | `dm/host/hle_d3.c`, `hle_misc.c`, `hle_oe.c`, `hle_hid.c` | Symbol names (from the exports of the Switch modules) and constants |
| **NVN usage facts**: depth mode (`NEAR_IS_ZERO` set at a quoted address), present / window behaviour, defaults chosen for this game | `tools/d3_shader_jobs.py`, `dm/android/jni_android.c`, `dm/gfx/nvn/*.c` | Comments and constants |
| **CPK archive format** (Blizzard's console CPK: header, tables, sector compression, the game's archive load order) | [CPK.md](CPK.md), `tools/d3_cpk.py`, `tools/d3cpklib.py` (D3CPKUnpack, MIT) | Format description and readers; no archive data |
| **`.shd` shader container layout** (SNO header, per-pass vertex / pixel control sections + code areas) | [SHADERS.md](SHADERS.md), `tools/d3_shader_jobs.py` | Parser; shader identifiers (16-hex-digit keys) appear in notes |
| **UI path hashing and callback tables** (FNV-1a of UI paths, the address ranges of two static initialisers) | `tools/d3_uiid_names.py`, `tools/d3_ui_callbacks.py` | Tools that build name tables from the player's own files into `work/` for debugging; the tables themselves are not in the repository |
| **Analysis notes**: function names, addresses, call chains, measured performance | `docs/d3/*.md` | Text analyses |

Function and type names come from the symbols the Switch modules export (or are descriptive names given by the author).

What the repository does **not** contain, and the tooling never writes into the source tree: `*.nsp / *.nsz / *.nca /
*.xci`, ExeFS (`main`, `sdk`, `rtld`, `*.raw`, `main.npdm`), `*.patched.bin`, RomFS files (`*.cpk`), `.shd` files, shader
jobs (`*.rec`, `jobs.tsv`, `shader_map.tsv`…), `.xgs` bundles, UI name / callback tables, saves, console keys.

The tree also carries the records of the sibling ports built on the same host: DOOM (2016) patch sites and gates
(`dm/tools/patch_nso.py`, `DM_GAME_DOOM` code; described in the DOOM Mobile repository's `docs/doom/DERIVED.md`) and the
DOOM Eternal / Xenoblade 2 code paths (`DM_GAME_ETERNAL` / `DM_GAME_XC2`), each bound to its own game's build id and unused by
the Diablo III build except where noted in [D3_GATES.md](D3_GATES.md). The base d2r-mobile tree carries its own D2R-derived
tables; they are described in [`docs/DERIVED.md`](../DERIVED.md) and are not used by the Diablo III build.
