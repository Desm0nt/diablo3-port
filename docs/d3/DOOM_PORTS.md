# What came over from the DOOM 2016 port (05.11.2026)

Device compatibility, diagnostics and generic fixes of DOOM Mobile (public repository `Desm0nt/doom-port`) ported to the
Diablo III host. Everything is built (normal APK minSdk 31, `-Android10` APK minSdk 29); on the phone only Adreno 750
can be checked, so everything non-Adreno is **built, not tested on hardware**. D3's tuned defaults are unchanged:
`DM_GFX_DEFER_EVENTS=2`, `DM_GFX_IDLE_RETIRE=2`, `DM_GFX_SUBMIT_CHUNK_US=0`, zero-copy on Adreno with the chunk retire,
`DM_DIRTY_UNPROT_KB=64` and the VMA watch, REPORT_INPASS / NAN_SAT / LAZY_CLEAR / LDC_CANON / F2I_NOTRUNC, the loading
screen stretch, widescreen, the D3 touch pad and the unified pause menu.

## Logs, breadcrumbs, crash reports

| DOOM | D3 | What |
|---|---|---|
| 7c0da0e, fc66617 | 0a395cc | `boot.log` per run (previous: `boot.prev.log`), cut back to its first 4 MB past `DM_LOG_MAX_MB` (32); header lines (`boot: log`, `boot: device`, `boot: gpu`); every `__android_log_print` of the library in the file (`-Wl,--wrap`); crash marker `files/dm-crash.txt` + fsync of the log from the signal handler; SIGABRT / SIGFPE reported too; fault-loop detector; `[life]`, `boot: Vulkan initialized in N ms` and other boot breadcrumbs; `[res] first open <archive>.cpk` |
| 7c0da0e, fc66617 | e5f64e0 | `CrashLog.java`: at the next start after a native crash, a Java exception, a start error or a death while playing (`dm-session.txt`, Android 11+ `ApplicationExitInfo`) → `Download/Diablo3Mobile/logs/crash.txt` (previous `crash.prev.txt`; fallback `Android/data/org.diablo3.port/files/logs/`), a toast; `boot.log` / `boot.prev.log` mirrored there on stop; `[java] …` breadcrumbs (driver decision, status, first frames, trim memory, nativeStart) |
| 7c0da0e | 9fff6df | `DM_GFX_CRUMBS=1|2` (default 0): GPU progress markers; on a fatal or a GPU stall (`DM_GFX_CRUMBS_STALL_MS`, 2000) `NVN crumbs …` lines name the in-flight work |
| 3a0d4a7 | dab7fda | on-demand dump: create the file `dm/dump` in the runtime folder → the next 300-present check prints every guest thread's wait into boot.log |
| — (D3) | 0a395cc, 5b1ca71 | D3 breadcrumbs: `[d3ui] crumb t s: focus … | active …` (UI reader, `DM_D3_UI_CRUMBS`, default 1, ≤ 8 lines/s), `[wide] loading screen #n at t s` / `#n over`, `[vma] … at t s maps …`, `NVN idle retire: first submission retired …` and `… not signalled after N s` (once per submission) |

Check on the phone: `grep -E "boot: log|boot: device|boot: gpu|boot: cpu|\[vk\] driver choice|\[java\] driver|first frames|\[life\]|\[res\] first open|\[d3ui\] crumb|loading screen #" files/dm/boot.log`.
Forced crash: `kill -SEGV <pid>` while playing → next start: toast + `Download/Diablo3Mobile/logs/crash.txt` with `reason: native crash (signal=11 …)`.

## Vulkan driver choice

| DOOM | D3 | What |
|---|---|---|
| 60ec064, d002c02, 3865197, 7c0da0e, fc66617 | bd1ec6b (native), e5f64e0 (app) | default driver by GPU (Adreno 6xx/7xx bundled Turnip; 710/720/722 and 8xx their downloaded Turnip package; non-Adreno system), packages downloaded when online from DOOM's public `drivers/manifest.json` (SHA-256 checked; the same manifest is copied into this repository as `drivers/manifest.json`), packages for another GPU family / glibc builds / missing libraries hidden and never loaded (`DM_VK_DRIVER_FORCE=1` tries anyway), the take-over verified (`verified=1`), fallback message with the reason. D3's crash guard (two launches dying before the first frame → system driver) kept and merged |

## Non-Adreno GPUs (Mali, PowerVR, Xclipse)

| DOOM | D3 | Fix | Status |
|---|---|---|---|
| 8ca23ae | d0b0384 | no `geometryShader` / formatless storage without `format_feature_flags2`: module unusable, draws / dispatches skipped (was fatal) | ported |
| 8ca23ae | d0b0384 | no `shaderImageGatherExtended`: capability removed, non-constant offsets dropped | ported |
| 8ca23ae | d0b0384 | no D24 (Xclipse): D32 stand-ins with unorm ↔ float on uploads / readbacks | ported |
| 8ca23ae | d0b0384 | queue without timestamps: CPU clock for game timestamp reports (was fatal); timestamp mask with 0 valid bits | ported |
| 8ca23ae | d0b0384 | storage texture whose format has no STORAGE_IMAGE: logged | ported |
| 8ca23ae | (already in D3) | lone-lane rewrite when the subgroup is not 32 wide (`DM_GFX_WARP_MODE`) | present |
| 7c0da0e | 9fff6df | `xbg_vendor_policy.h`: family by vendorID / name, `DM_GFX_SIM_GPU` simulation, per-family defaults (Adreno = old behaviour, host test passes) | ported |
| 7c0da0e | 9fff6df | sampler on a non-filterable format → NEAREST, anisotropy off (`DM_GFX_SAMPLER_STRICT=1` = fatal) | ported (replaces D3's narrower XC2 fallback) |
| 7c0da0e | 9fff6df | depthBounds / fillModeNonSolid / depthBiasClamp missing → dropped (were fatal) | ported |
| 7c0da0e | 9fff6df | graphics pipeline build failure not fatal, `VK_INCOMPLETE` with a handle = built, compute likewise | ported |
| 7c0da0e | 9fff6df | PowerVR serial pipeline builds (`DM_GFX_PIPE_SERIAL`), `DM_GFX_PIPE_CACHE_LOAD=0` | ported |
| 7c0da0e | — | DOOM's graphics pipeline in-flight markers (`DM_GFX_PIPE_GUARD`) | not used: D3 keeps its Eternal graphics pipeguard for that |
| 7c0da0e | 9fff6df | `vkWaitForFences` → `VK_ERROR_INITIALIZATION_FAILED` retried (engine and presenter) | ported |
| 7c0da0e | 9fff6df | null storage image / texel buffer without `nullDescriptor`: writable dummy (was fatal) | ported |
| 7c0da0e | 9fff6df | image usage refused as a whole → retry without STORAGE, then without attachment use | ported |
| 7c0da0e | 9fff6df | texel buffer views cut to `maxTexelBufferElements`; vertex formats without VERTEX_BUFFER widened | ported |
| 7c0da0e | 9fff6df | zero-copy only on Adreno; PowerVR without dynamic UBOs; colour writes masked for draws without FS outputs on non-Adreno | ported (Adreno defaults unchanged) |
| 7c0da0e | 9fff6df | `NVN fallbacks on this device: …`, `NVN gpu family=… policy …` | ported |
| fc66617 | 3b976dd | chunked uploads (`DM_GFX_BC_BATCH_MB`, 32) for decoded BC and > 256 MB staging | ported |
| fc66617 | 3b976dd | GPU BC decode (`DM_GFX_BC_DECODE=gpu`, Granite BC6H/BC7), BC6H → E5B9G9R9 (`DM_GFX_BC6_E5`), decoded partial uploads (`DM_GFX_BC_PARTIAL`) | ported; host test passes |
| fc66617 | — | setup ASTC / ETC2 / EAC overlay | skipped: DOOM's tool rewrites id Tech BIM textures in `.resources`; D3's textures live in CPK archives with another format. The engine side came along and stays inert (`DM_GFX_ASTC_FORMATS` is never set) |
| fc66617 | bd1ec6b | custom driver really taken over (`verify_custom`), family / glibc / DT_NEEDED checks | ported |
| 26025a1 | — | presenter timestamp queries only with `timestampValidBits` | not needed: D3's presenter queries only with the `gfx-present-timing` marker (off) |

## Other

| DOOM | D3 | What |
|---|---|---|
| a251eb4 | af0f0c4 | ARMv8.0 CPUs: NVN thunks, `DM_NVN_PROF` entry and `DM_HLECOUNT` stubs choose LSE or v8.0 at boot (`DM_CPU_NOLSE=1` forces v8.0); `boot: cpu …` line. libdm.so audit: v8.1+ only in the LSE thunks / profiler entry and the NDK outline-atomics helpers; `patch_nso.py` and the `hle_d3.c` runtime patches emit v8.0 encodings only |
| b573fda | 126f44e, e5f64e0 | Android 10 build: `build_apk.ps1 -Android10` → `work/D3-port-debug-android10.apk` (native tree `work/build-android29`, minSdk 29), legacy storage permission, guarded Android 11+ calls |
| d89997e | fef8b01 | start screens name `Download/Diablo3Mobile` (13 languages) |
| a2e751e | e5f64e0 | the on-screen pad is not hidden by built-in pseudo gamepads (DOOM issue #5); the Grouped / Separate editor toggle is DoomPadView-only (skipped) |
| 8ca23ae | 3c0f516 | `DM_DIRTY_BLOCKS` (default 1): per-64-page summaries make texture dirty checks cheap, same answers |
| 8ca23ae | (already in D3) | NaN-safe saturate (`DM_GFX_NAN_SAT`), frame rate cap (`nvn_pace_cap_hz`) |
| 3a0d4a7 | (already in D3) | cube map array layout (×6 bug) |

## Switches added (dm/env)

| Switch | Default | |
|---|---|---|
| `DM_LOG_MAX_MB` | 32 | boot.log size within a run |
| `DM_STDOUT_FILE` | 0 | 1: stdout → `<runtime>/stdout.txt` |
| `DM_D3_UI_CRUMBS` | 1 | UI focus breadcrumbs |
| `DM_GFX_CRUMBS` / `DM_GFX_CRUMBS_STALL_MS` | 0 / 2000 | GPU progress markers |
| `DM_CPU_NOLSE` | 0 | force the ARMv8.0 variants |
| `DM_GFX_SIM_GPU` | — | `mali` / `powervr` / `xclipse` / tokens: simulate a GPU family |
| `DM_GFX_SAMPLER_STRICT` | 0 | 1: non-filterable linear sampler is fatal again |
| `DM_GFX_PIPE_SERIAL` | PowerVR | one pipeline build at a time |
| `DM_GFX_PIPE_CACHE_LOAD` | 1 | 0: start from an empty pipeline cache |
| `DM_GFX_UBO_DYNAMIC` / `DM_GFX_NOOUT_MASK` / `DM_GFX_ZEROCOPY` | per family | Adreno: 1 / 0 / 1 as before |
| `DM_GFX_BC_DECODE` / `DM_GFX_BC6_E5` / `DM_GFX_BC_PARTIAL` / `DM_GFX_BC_BATCH_MB` | gpu / 1 / 1 / 32 | BC without device support |
| `DM_DIRTY_BLOCKS` | 1 | 0: plain page scan |
| `DM_VK_DRIVER_FORCE` / `DM_VK_DRIVER_DIRECT` | 0 / 0 | load a package for another family anyway / direct ICD experiment |

## Tile-based GPUs: Mali, PowerVR (13.11.2026, issue #4)

A Helio G99 phone (Mali-G57 MC2) runs the game at 27-31 fps with the GPU as the limit. A tile-based GPU reads every attachment a
render pass begins with LOAD back into its tile memory and writes every STORE one out again; D3 draws 36-45 passes a frame. The
tile-GPU paths of the DOOM 2016 port and two additions for D3's clear passes are on by default on Mali and PowerVR and off on
every other family (Adreno records the same commands as before). Built; checked only with the host test
(`tests/vendor_policy_test.cpp`); **not run on a Mali device**.

| Switch | Default | What |
|---|---|---|
| `DM_GFX_NOLOAD` | 1 on Mali / PowerVR, else 0 (marker `gfx-noload` still wins) | a pass whose first draw provably covers every pixel (`first_draw_covers`; candidates are the single full-screen draws of the post-process chain, about half of a frame's passes) begins with colour loadOp DONT_CARE. From DOOM: also refused with rasterizer discard, a viewport the draw would skip, zero instances, and a first draw that samples its own colour attachment |
| `DM_GFX_TILE_BARRIER` | 1 on Mali / PowerVR | pass dependencies and the read barriers of draws that sample only in the fragment shader wait in the fragment stages only, so the vertex / tiling work of the next pass overlaps the fragment work of the previous one; reads from earlier stages keep the full barrier |
| `DM_GFX_STORE_NONE` | 1 on Mali / PowerVR (needs `VK_EXT` / `VK_KHR_load_store_op_none`, enabled off Adreno; 2 = DONT_CARE without it, test only) | an attachment aspect no draw or clear of the pass instance writes (depth of the depth-tested, depth-reading passes) ends with STORE_OP_NONE. Predicted by reading ahead in the guest command stream (`store_scan`), enforced per draw / clear: a write into such an aspect ends the instance first |
| `DM_GFX_STORE_DEAD` | 1 on Mali / PowerVR | STORE_OP_DONT_CARE for an aspect the command stream provably clears in full before anything can read it. A contradicted proof is logged (`NVN store dead VIOLATION`) and turns the switch off for the rest of the run |
| `DM_GFX_LAZY_TWIN_FLUSH` | 1 on Mali / PowerVR (acts with `DM_GFX_LAZY_CLEAR`) | D3: a pass that holds only full clears (~8 a frame: shadow maps, bloom targets, the scene target) is recorded through its load-op twin (loadOp CLEAR) instead of LOAD + `vkCmdClearAttachments` |
| `DM_GFX_LAZY_KEEP` | 1 on Mali / PowerVR (acts with `DM_GFX_LAZY_CLEAR`) | D3: a read barrier for an image that is not an attachment of the pending clear pass is recorded before it, so the draw after the clear begins the pass with the clears as load ops (one pass instead of a clear pass and a LOAD pass) |
| `DM_GFX_TILE_POISON` | 0 | test: the noload twin clears to magenta and a dead store is overwritten after its pass, so a wrong proof shows on any GPU |

Log: `NVN tile policy: ...` and `NVN store ops: ...` at start; per window `NVN PERF noload_passes_per_frame=`, `NVN PERF tile
frag_barriers= ... store none_passes= ... dead_violations=` and `twin_flushes= kept=` at the end of `NVN PERF d3perf`.

Check on an Adreno phone: `DM_GFX_SIM_GPU=mali` turns the same policies on (Turnip runs with `TU_DEBUG=sysmem`, where load / store
ops cost nothing, so this checks the picture and the counters, not the gain); add `DM_GFX_TILE_POISON=1` and look for magenta.

Not brought over: `DM_GFX_TILE_AREA` (render area = bounding box; D3's passes cover their targets), `DM_GFX_LEAN_USAGE` (D3 binds no
storage images; colour targets are already created without MUTABLE_FORMAT), `DM_GFX_CHUNK_MID_SPLIT` (D3 submits once a frame),
zero-copy on Mali (a CPU-side saving; DOOM's players on Mali got no frame rate from it and one crash regression), the ASTC overlay
(needs a converter for the CPK texture format).
