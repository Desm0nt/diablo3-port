# Vulkan drivers

No driver binaries are stored in this repository.

**Bundled packages.** `tools/build_apk.ps1` runs `tools/fetch_drivers.py` (the shared copy of
`frontend-common/tools/drivers/fetch_drivers.py`, the same pinned list as the DOOM Mobile port), which downloads these adrenotools
packages with pinned SHA-256s and packages them into the APK as `assets/drivers/<name>/`: the four Mesa **Turnip v27** builds (MIT;
build by [The412Banner](https://github.com/The412Banner/Banners-Turnip)), **RADV Xclipse 1.8.0** (Samsung Xclipse, Android 14+;
[JimVulkan](https://github.com/JimVulkan/radv-xclipse)), **PanVK JimVulkan 1.6.0** and **PanVK zenith 26.3.1** (Mali;
[JimVulkan](https://github.com/JimVulkan/mali-panvk), [zenithblue-oss](https://github.com/zenithblue-oss/panvk-kbase-android)).
On every start after an install or update the app copies to its private storage only the packages that fit the phone's GPU
(GPU family, glibc build, `meta.json` `minApi`) and replaces older copies of them. `turnip27` is the default on Adreno 6xx/7xx; on
Mali, PowerVR and Xclipse the system driver stays the default and PanVK / RADV are experimental options in the gear dialog.
Without the downloads the APK simply uses the system driver.

**Driver manager.** The gear dialog's *Manage drivers / Download…* lists the installed packages (origin, GPU family, size, why one
cannot run here), installs a package zip picked with the system file picker, and downloads packages for this GPU from the GitHub
releases listed in `drivers/sources.json` (the app reads the DOOM Mobile repository's copy, cached for 6 h; this file is the copy the
APK carries for offline use).

**Your own driver.** Copy an adrenotools driver package into `Download/Diablo3Mobile/drivers/` on the phone, as the zip
or unpacked:

```text
Download/Diablo3Mobile/drivers/
  my-turnip.zip              <- the package zip as released (meta.json + the driver .so), unpacked on the next launch
  my-turnip/                 <- or the unpacked folder
    meta.json
    libvulkan_freedreno.so
```

Restart the game, pick it in the gear dialog under *Vulkan driver* and restart again (*System* = the phone's own driver).
If a chosen driver keeps failing, the app returns to the system driver and says so.

Packages that are known to work with the DOOM Mobile port of the same engine (all from
[Banners-Turnip v27.0.0-20261029](https://github.com/The412Banner/Banners-Turnip/releases/tag/v27.0.0-20261029); use the
plain Android builds, not `-Linux` / `-Wayland`):

| Package | For | SHA-256 |
|---|---|---|
| `Turnip-v27.0.0-20261029.zip` (the bundled default) | Adreno 6xx / 7xx | `d413e41d53508cab28698548f77852f37846003727550b09814018e5b9a352b1` |
| `Turnip-v27.0.0-20261029-710-720-Test.zip` | Adreno 710 / 720 / 722 (experimental) | `4927023e818b81fe19e451549cf83290dc3545f28b537a9bf1f348b060747615` |
| `Turnip-v27.0.0-20261029-A8xx.zip` | Adreno 8xx / Snapdragon 8 Elite (experimental) | `b1126d1a320624101757c7728179b80590d19d17d38a2eb5d85eb5dfd50df82a` |
| `Turnip-v27.0.0-20261029-8G2-OneUI.zip` | Adreno 740 on newer firmware (Samsung One UI) | `e7a9c8c12ccddd39b0316fae747350964096a6622630f1191bbab02208f356b3` |

Diablo III Mobile itself has been tested only on Adreno 750 (Snapdragon 8 Gen 3) with the bundled Turnip v27.
`TU_DEBUG` (Turnip's debug options) can be set in `Download/Diablo3Mobile/dm/env`.
