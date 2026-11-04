# Diablo III (DM_GAME=d3): gate decisions

`DM_GAME=d3` = the lean DOOM core (`DM_GAME_DOOM`) + `DM_GAME_D3` (src commit a4357e8 on branch `d3`). D3 is SDK 8.2.0 (sdk build
3ae34e68, the same as nxnative's Link's Awakening), NVN API 53 (DOOM 2016 object layouts), VAMM on (npdm SystemResourceSize 0x1000000).
Rule: Eternal/XC2 gates that are newer-SDK or any-game NVN behaviour include D3; id Tech 7 / NVN 55 / bindless stay Eternal-only;
Monolith-specific stay XC2-only; DOOM (2016) main-offset hacks exclude D3.

| Gate (file:area) | Was | D3 | Why |
|---|---|---|---|
| hle_svc.c `ALIAS_BIG` 32 GiB, reserve below 1 TiB | Eternal / E+X | **on** | VAMM: AllocateAddressRegion + MapPhysicalMemory in the alias region; 1 GB node area + 1 GB alignment fit like on the Switch's 64 GiB, so nxnative's iOS sdk_patches (62 MB node area, 2 MB align) are not needed |
| hle_svc.c GetInfo SystemResourceSize | Eternal 0x800000 | **0x1000000** | npdm value (SDK turns VAMM on when non-zero) |
| hle_svc.c ProgramId | per game | 0x01001B300B9BE000 | npdm ACI0 |
| hle_svc.c / crash.c / hle_thread.c NVN trace dump, stack scan, libdm frames | E+X | on | diagnostics |
| hle_doom.c ec shop pages, ro (no NRO), oe ActiveTime/PerfConf/CpuBoost, settings::operator== | E+X | on | newer-SDK service HLE; D3 imports settings==, SetCpuBoostMode |
| hle_doom_audio.c active audio device queries | E+X | on | harmless (D3 uses AudioOut) |
| hle_fs.c MountAddOnContent / cache size | E+X | on | keeps AOC mounts off IPC (D3 reports no AOC unless DM_AOC_OWNED=1) |
| hle_args.c DOOM cvar presets; Eternal Bink args | DOOM / Eternal | off | id Tech only |
| hle_doom_tick.c, hle_hid.c DM_INPUT_OBS, ReadNpad probe, aim-assist / gyro cvar gates | DOOM (!XC2) | off | DOOM 2016 main offsets |
| hle_hid.c XC2 block (mouse/keyboard, SetNpadJoyHoldType, ShowControllerSupport+result) | XC2 | on | D3 imports SetNpadJoyHoldType |
| hle_oe.c GetDesiredLanguage | XC2 list | own list | NACP languages; en-US when RomFS has only `enUS_*.cpk` |
| hle_misc.c AOC | per game | DOOM default | no AOC unless DM_AOC_OWNED=1 |
| front_android.c gyro default off | XC2 | on | D3 imports six-axis reads but has no gyro aim; DM_GYRO=1 enables |
| jni_android.c DM_NVN_SYNC=0, boot without shaders | E+X | on | async translation; partial shader sets boot |
| jni_android.c subsdk0 load order, XC2 AOC dir, DRS | XC2 | off | D3 has main + sdk only |
| hle_nvn.c trace/bigstack wrapper, newer NVN stubs (k_et_nvn), ZCull storage size | E+X | on | generic NVN |
| hle_nvn.c origin model (device vs queue origin), PackagedTextureData, misaligned-object snapshots | XC2 | on | nxnative's generic NVN semantics; D3 sets UPPER_LEFT + NEAR_IS_ZERO right after device init |
| hle_nvn.c / nvn_obj.h vpool (sparse pools), view16_t, 32-byte buffer, snap_by_serial | Eternal | off | NVN 55 / id Tech 7 |
| nvn_tex.c cube-array depth, layer bytes, packaged layout nibbles | XC2 | on | NVN driver rules (nxnative) |
| xbg_sampler.cpp / xbg_game.cpp nearest canonical border, custom border colour ext, NEAREST for non-filterable | XC2 | on | generic |
| xbg_game.cpp zero-copy default off, UBO snapshot min 1 | XC2 | on (off / 1) | correctness first; DM_GFX_ZEROCOPY=1 / DM_GFX_ZC_UBO_MIN=0 for A/B |
| xbg_game.cpp 3D slices as RTs, alias-by-layout, fragment-less draws, counters tolerated | XC2 | on | generic NVN |
| xbg_game.cpp warp mode (lone-lane on 64/128-lane subgroups) | XC2 | on | translated Maxwell code assumes 32-lane warps |
| xbg_game.cpp program key = nxnative control+code | XC2 | on | matches work/shaders-vk keys |
| xbg_game.cpp missing bundle: log + skip | E+X | on | partial shader sets |
| xbg_game.cpp graphics pipeguard + two-boot rule | E+X / XC2 | on | Adreno compiler crashes; run.sh's SIGSEGV dumps |
| xbg_game.cpp D2R / DOOM Adreno shader keys in pipeguard | !XC2 | off | foreign keys |
| xbg_game.cpp DM_XC2_WIDE UI shift | XC2 | off | XC2 only |
| xbg_game.cpp bindless, virtual pools, desc_abandon, VAMM-unmapped UBO zeros, gpu_merge_pending | Eternal | off | id Tech 7 bindless / sparse. Candidates if D3 shows "vkCmd* before vkBeginCommandBuffer" (desc_abandon) or a SIGSEGV reading a uniform block in unmapped VAMM memory (uniform snapshot zeros) |
| vulkan_present.cpp Vulkan 1.2 + descriptor indexing | Eternal | off | bindless only |
| vulkan_present.cpp device caps line, custom border colour | XC2 | on | generic |

## D3-only HLE (`dm/host/hle_d3.c`, `hle_hid.c` D3 block)
hid GetNpadStates x5 (history = current state repeated), GetSixAxisSensorStates, StopSixAxisSensor, BindNpadStyleSetUpdateEvent (signalled when
connected and on opmode switch), ShowControllerSupport(arg), ShowControllerStrapGuide; nfp (one device, never a tag; nxnative); ldn (Initialize fails;
nxnative); nifm::SubmitNetworkRequestAndWait (no-op), socket::Finalize / InetNtop / InetNtoa, friends::NotificationQueue::Finalize,
pctl::CheckFreeCommunicationPermission (OK = no restriction), ec::ShowShopAddOnContentList(app, user), audio::OpenAudioIn (3-arg, fails),
oe::FinishStartupLogo, Enter/LeaveExitRequestHandlingSection, account::ListAllUsers / CloseUser, fs::IsSaveDataExisting(uid) = true.
Offline stance otherwise comes from the existing DOOM/D2R HLE: nifm IsNetworkAvailable false, HandleNetworkRequestErrorResult fails,
C nnsocket* (libcurl) all fail ENETDOWN, C++ nn::socket = host sockets limited to loopback (sa_offline), GetAddrInfo fails, ssl fails,
nsd Resolve fails, NSA unavailable / token fails, friends empty / failing.

## Not determinable statically
- How D3 reacts to ldn::Initialize failing (it may call it only when local play is chosen, or at boot).
- Whether Battle.net login shows an "offline" path cleanly with NSA unavailable + nifm unavailable (expected; check the first menu).
- Whether D3 locales other than enUS exist anywhere (the extracted RomFS has only enUS_*.cpk).
- Debug-marker / transform-feedback / tessellation NVN entry points: 44 names in main's table are unimplemented (red log if called); debug ones are nops.

## Front end (Java / argv), src commit after a4357e8
| Piece | DOOM | D3 |
|---|---|---|
| DoomPadView layout | FIRE (ZR) + R3 bubble, look by dragging free screen / held FIRE, move stick above d-pad | Joy-Con-like: left stick (floating in the lower left) + d-pad below, A/B/X/Y + right stick (evade) below, L/ZL R/ZR top corners, - ⚙ + top centre; tap on a stick knob = L3/R3; no look, no FIRE |
| gear dialog | DRS, 3D resolution, aim assist, look sensitivity | hidden; title "Настройки"; dead-zone compensation applies to both sticks |
| DM_DOOM_DRS / DM_DOOM_RES env | from prefs | always unset |
| hle_args.c argv[0] | "doom" | "d3" (DM_DOOM_ARGS still appended) |
| touch | D2R TouchOverlay only (not for DOOM family) | not forwarded: D3 main imports no TouchScreen / mouse / keyboard hid API |
