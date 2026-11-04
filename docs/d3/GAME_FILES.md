# Game files: what setup does, and how to do it by hand

Players only need `setup.bat` / `setup.sh` ([INSTALL.md](../INSTALL.md)). This page describes what `tools/setup_d3.py`
does with your dumps, for developers and for troubleshooting.

## Inputs

| Title | Id | Version | Needed for |
|---|---|---|---|
| Diablo III: Eternal Collection | `01001B300B9BE000` | v0 | RomFS (base of the BKTR patch) |
| Update (game patch 2.7.7) | `01001B300B9BE800` | **v1441792** | ExeFS (`main` build id `2607a74f5df7754cc0357b5df7e49693`, `sdk` `3ae34e689d7a35f13cbf42bfd672af8b`) + the BKTR RomFS patch |
| Language packs (optional) | `01001B300B9BF000` + index, e.g. `…BF001` (1, English UK), `…BF00A` (10, Russian) | v0 | `aoc/<index>` |
| `prod.keys` | — | — | decrypting the packages (your own console's) |

Packages may be NSP, NSZ, XCI or XCZ, also split dumps. They are identified by their content meta (CNMT), not by the file
name. The cosmetic DLC (index 100..102) is not used and is skipped.

## Output (`work/Diablo3Mobile`, copied to `Download/Diablo3Mobile` on the phone)

```text
Diablo3Mobile/
  romfs/CPKs/*.cpk, romfs/Update/CPKs/Patch*.cpk    the merged RomFS (37 files, ~16 GiB)
  dm/out/{main,sdk}.{patched.bin,fixups.tsv,gates.tsv,sites.tsv,...}   the patched modules
  dm/shaders/<key>.xgs + _vk_build.json             1323 SPIR-V bundles (~31 MB)
  aoc/<index>/*.cpk                                 language packs (~5-6 GiB each), optional
```

The app also reads `dm/env` (KEY=VALUE lines, debugging switches) and `drivers/` (Vulkan driver packages) there.

## Steps

| # | Step | Tool | Result |
|---|---|---|---|
| 3 | ExeFS of the update | `content_reader.py` (plain NSP / XCI) or `extract_exefs_nsz.py` (NSZ / XCZ); by hand: `tools/exefs_nsp.py <update> work/exefs-update` | `work/exefs-update/{main, sdk, rtld, main.npdm}`; the build ids are checked |
| 4 | Offline patch | `DM_TARGET=android python dm/tools/patch_nso.py work/exefs-update/<mod> <out> <mod>` for `main`, `sdk` | `dm/out/*` (main: 4420 sites, sdk: 11277 sites + 93 host gates) |
| 5 | Patch check | `python dm/verify/equiv.py <out> <mod> work/exefs-update/<mod> --jobs N` | every rewritten site against the original instruction under random states, plus a 300-mutation self-check; must print `EQUIV <mod> OK`. Needs `unicorn` |
| 6 | RomFS | `tools/romfs_d3.py extract <base> <update> <dest>` (NSZ / XCZ: program expanded first with `tools/ncz2nsp.py`) | IVFC hashes verified, BKTR applied, files + `work/romfs-manifest.json` (sha256 of every file) |
| 7 | Shaders | the four steps of `tools/d3_shaders_vk.sh`: `tools/d3_cpk.py mextract <romfs> work/cpk-shaders '*.shd'` → `tools/d3_shader_jobs.py` → `tools/build_dmsh_xlate_d3.py` (.NET 10 SDK, Ryujinx 1.3.2 from `dm/third_party/ryujinx`) → `dm/gfx/tools/dmsh_vk_xc2.py` | `dm/shaders/*.xgs`; details in [SHADERS.md](SHADERS.md) |
| 8 | Language packs | `tools/d3_dlc_romfs.py extract <dlc> [--dest DIR]` | `aoc/<index>/` + `work/aoc-manifest.json` |

Every step is skipped when its output is complete; delete an output to redo it. `setup --steps exefs,patch,verify` (etc.)
runs a subset, `--skip-verify` / `--skip-shaders` leave those out, `--keep-nsp` keeps the expanded NSPs, `--work DIR`
moves the whole work folder. The results are deterministic: the same dumps give byte-identical patched modules and
shader bundles.

## Copying to the phone

`setup push [serial]` copies only missing or size-mismatched files over adb (an interrupted copy resumes) and grants the
app *All files access*. By hand:

```sh
adb shell mkdir -p /sdcard/Download/Diablo3Mobile/dm /sdcard/Download/Diablo3Mobile/aoc
adb push work/Diablo3Mobile/romfs /sdcard/Download/Diablo3Mobile/
adb push work/Diablo3Mobile/dm/out /sdcard/Download/Diablo3Mobile/dm/
adb push work/Diablo3Mobile/dm/shaders /sdcard/Download/Diablo3Mobile/dm/
adb push work/Diablo3Mobile/aoc/10 /sdcard/Download/Diablo3Mobile/aoc/      # each language pack you prepared
adb shell appops set org.diablo3.port MANAGE_EXTERNAL_STORAGE allow          # after installing the APK
```

Large files over a flaky USB / wireless adb link: `tools/push_phone.sh` (and `tools/push_big.sh`) resume in chunks.

## Developer tools (not needed to play)

| Tool | What |
|---|---|
| `tools/nso_symbols.py <exefs dir> main sdk` | decompressed images (`*.raw`) and import / export tables (`*.symbols.json`) |
| `tools/symbt.py <log> [exefs dir]`, `tools/plt.py`, `tools/xref_str.py`, `tools/ipc_reach.py` | backtrace symbolisation, PLT names, string xrefs, SDK IPC reachability |
| `tools/d3_cpk.py info / verify / list / extract / mlist / mextract / scan` | CPK archives ([CPK.md](CPK.md)) |
| `tools/d3_uiid_names.py`, `tools/d3_ui_callbacks.py` | UI id → path table and UI callback table for the host's UI dump (`DM_D3_UIDUMP`), written to `work/` |
| `tools/run.sh`, `hangcheck.sh`, `gp_ab.sh`, `shots.sh`, `menu_shot.sh`, `keep_awake.sh` | device test runs (`ADB_SERIAL`, `ADB`), logs to `work/logs/` |
| `tools/cap_chain.py`, `tools/dump2png.py`, `tools/d3_spv_census.cpp` | GPU capture and SPIR-V analysis |
