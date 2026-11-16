package com.xiaoboluo.d2r;

import android.app.Activity;
import android.content.SharedPreferences;
import java.io.File;

// [D3] The part of the shared gear's port interface (frontend-common/java/GearHost.java) that the shared Vulkan driver code needs:
// VkDrivers (bundled packages copied when they fit the phone, the .dm_origin marker, the helpers), DriverManager ("Manage drivers…":
// installed packages, install from a zip, "Download…" from drivers/sources.json) and GearText (their texts). Diablo III keeps its own
// gear dialog (DmActivity.showD3Settings), so the shared one's other methods (pad, pause menu, gear sections, safe mode) are left out here;
// the same names and signatures as the shared file, so the full file replaces this one unchanged when D3 moves to GearPages
// (frontend-common/migration/other-ports.md, "Diablo III"). DmActivity.driverHost implements it.
interface GearHost {
    Activity activity();
    String dataDirName();                       // Download/<this> ("Diablo3Mobile"): driver import fallback, the download User-Agent
    SharedPreferences gamePrefs();              // "doom_graphics": the next-launch settings (pref "driver", "driver_removed")
    boolean libraryLoaded();                    // libdm.so is loaded: native calls may be made
    boolean started();                          // the game runs
    void crumb(String line);                    // a "[java] ..." line in boot.log
    void immersive();                           // the system bars hidden again (after a dialog)
    VkDrivers drivers();                        // the port's one VkDrivers

    // ---- optional, with the behaviour of DOOM 2016 as the default (as in the shared file)
    default boolean gearUsesPortResources() { return false; }
    default String driverStatus() { return null; }
    default float[] frameStats() { return null; }
    default File driverImportDir() { return new File(android.os.Environment.getExternalStorageDirectory(), "Download/" + dataDirName() + "/drivers"); }
    default String defaultDriver(VkDrivers d) { return d.gpuDefault(); }
    default String forcedDriver() { return null; }
    default String driverManifestUrl() { return VkDrivers.MANIFEST_URL; }
    default String driverSourcesUrl() { return VkDrivers.SOURCES_URL; }
    default int driverZipRequest() { return DriverManager.REQ_ZIP; }
    default boolean driverMultiPick() { return false; }   // true: onActivityResult calls DriverManager.picked(host, Intent) (several zips)
}
