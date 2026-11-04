# Third-party code in the Diablo III port

The tree in `dm/` is a fork of d2r-mobile (MIT, Copyright (c) 2026 jhaizhou, kept in the root `LICENSE`; its Java package is
`com.xiaoboluo.d2r`), extended first for DOOM Mobile and then for DOOM Eternal, Xenoblade 2 and Diablo III; the third-party
code d2r-mobile brought is listed in [DEPENDENCIES.md](DEPENDENCIES.md) and its contributors in
[upstream/README-d2r-mobile.md](upstream/README-d2r-mobile.md). Code was also ported from other MIT-licensed projects.
Each ported file says so in a comment and keeps the original copyright line.

| Project | License / copyright | Source |
|---|---|---|
| d2r-mobile | MIT, Copyright (c) 2026 jhaizhou | https://github.com/jhaizhou-ops/d2r-mobile — the base of `dm/` |
| nxnative | MIT, Copyright (c) 2026 seedds | https://github.com/seedds/nxnative |
| nx2android | MIT, Copyright (c) 2026 seedds (its embedded `nxnative/LICENSE`) | https://github.com/shirai91/nx2android (the source comments cite it as `seedds/nx2android`) |
| nx2android gamepad themes | MIT, Copyright (c) 2026 seedds (`dm/android/front-assets/LICENSE-nx2android-themes.txt`) | https://github.com/shirai91/nx2android — the on-screen gamepad skins (`themes/`) and the default layout (`overlay_default.tsv`) in `dm/android/front-assets`, unmodified |
| BOTWRecompAndroid | MIT, Copyright (c) 2026 Peiikai + notice mamcjr | https://github.com/mamcjr/BOTWRecompAndroid (studied at 5675618) — ideas only, see below |
| D3CPKUnpack (`cpklib.py`) | MIT, Copyright (c) 2026 Krzysztof Janiszewski (RedMadKnight) — `tools/d3cpklib.LICENSE` | https://github.com/RedMadKnight/D3CPKUnpack at 9fa02146e33d3ef38b2ea6ec5dfdb3aebceba10e — vendored unmodified (below a two-line header) as `tools/d3cpklib.py`; `tools/d3_cpk.py` is our front-end |
| libadrenotools | BSD-2-Clause, bylaws | https://github.com/bylaws/libadrenotools — `third_party/libadrenotools` (git submodule, with its own `lib/linkernsbypass`) |
| Ryujinx shader translator 1.3.2 | MIT, Ryujinx Team and Contributors | vendored by d2r-mobile in `dm/third_party/ryujinx` (unmodified); built into `dmsh_xlate` on the player's PC |
| AMD FidelityFX Super Resolution 1.0 (RCAS) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. | https://github.com/GPUOpen-Effects/FidelityFX-FSR — `dm/android/shaders/rcas.comp` |
| deko3d (layout size algorithm) | zlib, Copyright (C) 2018-2020 fincs | https://github.com/devkitPro/deko3d — `dm/gfx/nvn/nvn_tex.c` (inherited from d2r-mobile, see DEPENDENCIES.md) |
| Mesa Turnip (adrenotools build by The412Banner) | MIT | https://github.com/The412Banner/Banners-Turnip — **not stored here**; `tools/fetch_turnip.py` downloads v27.0.0-20261029 at build time with a pinned SHA-256 and the APK bundles it as its default driver |

Only the parts that Diablo III (and the sibling ports sharing this host) use were ported, on an Adreno 750 target.
Diablo III code is behind `DM_GAME_D3`; the DOOM / DOOM Eternal / Xenoblade 2 code behind `DM_GAME_DOOM` / `DM_GAME_ETERNAL` /
`DM_GAME_XC2` (Diablo III builds `DM_GAME_DOOM` + `DM_GAME_D3`, see [d3/D3_GATES.md](d3/D3_GATES.md)).

## Host (dm/host)

| Our file | Ported from | What |
|---|---|---|
| `hle_thread.c` | nxnative `dm/host/hle_thread.c` | Thread scheduling and sync: same-core wake handoffs, native HLE waits, ideal core / affinity rules, `WaitForAddress` / `SignalToAddress` variants, per-object WaitSynchronization wakes, yield back-off, precise sleeps, SystemEvent binding lifetimes, free thread count |
| `hle_svc.c` | nxnative `dm/host/hle_svc.c` | Memory SVCs: SetHeapSize result, MapMemory checks, QueryMemory over the alias page bitmap and module segments, MapPhysicalMemory page bitmap with zeroing, GetInfo 16 / 17 / 20 / 24 |
| `hle_guestthread.c` | nxnative `dm/host/hle_guestthread.c` | Host-created guest threads are marked as host-owned |
| `hle_audren.c` | nxnative `dm/host/hle_audren.c` | The SDK's own audio renderer runs on the CPU and plays through AAudio |
| `hle_audio.c` | nxnative `dm/host/hle_audio.c` | `dm_pcmout_*` host PCM output |
| `hle_sf.c` | nxnative `dm/host/hle_sf.c` | `nn::sf::hipc` client takeover |
| `hle_oe.c` | nxnative `dm/host/hle_oe.c` | Focus handling mode, language mapping |
| `hle_fs.c` | nxnative `dm/host/hle_fs.c` | Path normalisation, EFAULT retry on ReadFile, directory entry count filter, Delete/RenameDirectory |
| `hle_net.c` | nxnative `dm/host/hle_net.c` | nifm request on hold, Poll/Select idle wait |
| `hle_haptics.c`, `hle_haptics.h` | nxnative `dm/host/hle_haptics.{c,h}` | Vibration state |
| `hle_hid.c` | nxnative `dm/host/hle_hid.c` | Vibration handles and send, SixAxis layout and handles |
| `hle_xc2.c` | nxnative `dm/host/hle_fs.c`, `hle_hid.c`, `hle_account.c`, `hle_sdk16.c`, … | Account / fs / oe / SDK entry points of newer SDKs (Xenoblade 2; Diablo III reuses the pattern) |
| `hle_d3.c` | nxnative `dm/host/hle_sdk16.c` ("nfp: amiibo"), `dm/host/hle_offline.c` ("ldn: local wireless") | The amiibo and local-wireless stubs Diablo III's SDK 8.2 client needs; the rest of the file is our own |
| `hle_os.c` (`guest_clock_read_wait`), `hle_hold.c` | nx2android `hle_os.c` guest_clock_read_wait, hold_sync (design) | Freezing guest time and threads while the pause menu holds the game (re-implemented) |

## Graphics (dm/gfx)

| Our file | Ported from | What |
|---|---|---|
| `gfx/nvn/hle_nvn.c` | nxnative `dm/gfx/nvn/hle_nvn.c` | NVN entry points (binds, draws, clears, raster state, fence sync, texture swizzle, depth-stencil mode, sampler reduction, getters); the sparse-pool design (`vpool_*`, used by DOOM Eternal) |
| `gfx/nvn/hle_nvn.c` (window acquire sync) | nx2android commit 1c6177d | The acquire sync is fenced at the texture's last present |
| `gfx/nvn/nvn_obj.h` | layouts follow nxnative `dm/gfx/nvn/nvn_obj.h` (ideas, no code copied); NVN 55 sizes also from BOTWRecompAndroid | Record ops and fields |
| `gfx/vulkan/engine/xbg_shader_key.c` | nxnative `dm/gfx/nvn/hle_nvn.c` keys_hash / `shader_split.py` skey | The shader program key (control section + code area) the Diablo III bundles are named by |
| `gfx/vulkan/engine/xbg_game.cpp` (LRZ touch) | nx2android `lrz_touch` | Optional (`DM_GFX_LRZ_TOUCH=1`) |
| `gfx/tools/dmsh_xlate_doom/Program.cs`, `dmsh_xlate_eternal/Program.cs`, `dmsh_xlate_xc2/Program.cs` | nxnative `dm/gfx/tools/dmsh_xlate/Program.cs` | Shader translators (Diablo III uses the XC2 variant): stage linking, translator messages, c2 image handle decode, FP64 lowering, image query retry |
| `gfx/tools/dmsh_vk_xc2.py` | nxnative `dm/gfx/nvn/dmsh_abi.h` (the c2 handle layout) | Offline SPIR-V bundle builder used for Diablo III |

## Tools

| Our file | Ported from | What |
|---|---|---|
| `dm/tools/patch_nso.py` | nxnative `dm/tools/patch_nso.py` | Jump-table recognizers and x18 site kinds (`blr` / `br` calls, sp writes, `sub sp` alignment, exclusive-window rules) |
| `dm/tools/nso_code.py` | nxnative `dm/tools/nso_code.py` | Code / data block analysis (copied unchanged) |
| `tools/d3_shader_jobs.py` | nx2android `nxnative/dm/android/tools/shader_vk.py` (`xfb_layout`); nxnative `shader_split.py` (the key) | Transform-feedback layout reader and the program key, re-implemented for the `.shd` files |
| `tools/d3cpklib.py` | D3CPKUnpack `cpklib.py` | The CPK archive reader, vendored unmodified |
| `tools/content_reader.py`, `tools/ncz2nsp.py`, `tools/extract_exefs_nsz.py`, `tools/setup_d3.py`, `tools/bootstrap.ps1` | DOOM Mobile (same author, MIT) | Package / NCA / BKTR / IVFC readers, NSZ expansion, the setup |

## Android (dm/android)

| Our file | Ported from | What |
|---|---|---|
| `haptics_android.c` | nx2android `nxnative/dm/android/haptics_android.c` | Controller and phone rumble |
| `perf_android.c` | nx2android `nxnative/dm/android/perf_android.c` | ADPF hint session, thermal status, display rate vote |
| `front_android.c` | nx2android `nxnative/dm/android/front_android.c` | Gamepad button mapping, pad publish |
| `app/.../PadTheme.java`, `PadSkin.java`, `DoomPadView.java` | nx2android's overlay drawing model (`overlay_layout.h`, `overlay_theme.c`, `ov_theme_draw`, `dm_overlay_scale`) | Drawing the theme packs and the ergonomic Switch layout (re-implemented in Java) |
| `app/.../SaveTransfer.java` | nx2android `saves_android.c` (design) | Save export / import (re-implemented) |

## Ideas from BOTWRecompAndroid (MIT)

Studied on 28.10.2026 for DOOM Mobile. No files were copied; the ideas below were re-implemented in our code, which names
the source in a comment.

| Our code | Idea from | What |
|---|---|---|
| `dm/android/vk_driver.c`, `vk_tramp.S` / `vk_tramp_names.h` | BOTWRecompAndroid `runtime/nvn/vulkan/android_driver.c` (adrenotools + volk) | Vulkan driver chosen at run time (system driver or Turnip through libadrenotools, `DM_VK_DRIVER`); we use trampolines instead of volk |
| `gfx/nvn/hle_nvn.c` `nvnSyncWait` (`DM_NVN_SYNC_LAG=1`, off by default) | BOTWRecompAndroid 82b71e2 "sync lag" | Wait for the fence of frame N-1 instead of N |
| `gfx/nvn/nvn_obj.h` `FITS55` limits | BOTWRecompAndroid NVN object writes | Size limits of NVN 55 objects (DOOM Eternal) |

No code or idea from LASRecomp / LASRecompAndroid (Peiikai, mamcjr) is in this tree; they are credited in the DOOM Mobile
repository, where they apply.

## Not redistributed

- Khronos Vulkan validation layers (Apache-2.0): optional for debugging (`build.bat -Validation`), you download them yourself.
- Mesa Turnip: downloaded at build time (see above).
- The .NET SDK, Python and its packages (cryptography, zstandard, lz4, capstone, unicorn): installed by setup into
  `tools/.runtime` on the player's PC from their official sources.
