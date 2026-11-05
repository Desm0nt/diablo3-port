# Build libdm.so (DM_GAME=d3) and the adrenotools hook libraries, package the Android app around them and sign it.
# Usage (from anywhere): powershell -File tools/build_apk.ps1 [-Install <adb serial>] [-Validation] [-Android10]
# -Android10 (from the DOOM 2016 port, b573fda): the Android 10 build (minSdk 29) from work/build-android29, a native tree configured with
#   -DANDROID_PLATFORM=android-29 (configured here from work/build-android's cache when missing); the same sources (Android 11+ calls are
#   guarded), output work/D3-port-debug-android10.apk. Its native objects and jni folder (work/jni29) are separate, so both APKs can be built
#   side by side.
#
# Needs a configured native build tree in work/build-android (see README.md, "Building"), and:
#   JAVA_HOME        JDK 17+ (Gradle's Android plugin)
#   ANDROID_HOME     Android SDK with build-tools (apksigner) and platform-tools (adb)
#   GRADLE           Gradle launcher (default: gradle on PATH)
#   PYTHON           Python 3 used by the Gradle string generator (default: python on PATH)
#   DM_KEYSTORE      signing keystore (default: the Android debug keystore, ~/.android/debug.keystore)
#   DM_KEYSTORE_PASS keystore and key password (default: android); DM_KEY_ALIAS (default: androiddebugkey)
#   DM_BUILD_TOOLS   build-tools version to sign with (default: the newest installed)
# Keystores never go into this repository (.gitignore excludes *.keystore / *.jks).
# Output: work/D3-port-debug.apk (-Android10: work/D3-port-debug-android10.apk), application id org.diablo3.port ("Diablo III Mobile"; data
# folder Download/Diablo3Mobile on the phone). Both variants have the same id and signature: one replaces the other on a phone.
param([string]$Install = '', [switch]$Validation, [switch]$Android10)
# Continue: Windows PowerShell turns any stderr line of a native tool (cmake GLOB notes, javac deprecation notes) into a stop;
# every step checks $LASTEXITCODE instead
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
function Env-Or([string]$name, [string]$default) { $v = [Environment]::GetEnvironmentVariable($name); if ($v) { $v } else { $default } }
if (-not $env:ANDROID_HOME) { throw 'Set ANDROID_HOME to the Android SDK' }
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
$env:PYTHONUTF8 = '1'
$gradle = Env-Or 'GRADLE' 'gradle'
$python = Env-Or 'PYTHON' 'python'
$keystore = Env-Or 'DM_KEYSTORE' (Join-Path $HOME '.android/debug.keystore')
$kspass = Env-Or 'DM_KEYSTORE_PASS' 'android'
$alias = Env-Or 'DM_KEY_ALIAS' 'androiddebugkey'
$bt = Env-Or 'DM_BUILD_TOOLS' ((Get-ChildItem "$env:ANDROID_HOME/build-tools" -Directory | Sort-Object { [version]($_.Name -replace '[^0-9.].*$', '') } | Select-Object -Last 1).Name)
$apksigner = Join-Path $env:ANDROID_HOME "build-tools/$bt/apksigner$(if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.bat' })"
$adb = Join-Path $env:ANDROID_HOME 'platform-tools/adb'
$bdir = if ($Android10) { 'work/build-android29' } else { 'work/build-android' }
$jdir = if ($Android10) { 'work/jni29' } else { 'work/jni' }
$minsdk = if ($Android10) { '29' } else { '31' }
$apk = if ($Android10) { 'work/D3-port-debug-android10.apk' } else { 'work/D3-port-debug.apk' }
if ($Android10 -and -not (Test-Path $bdir/CMakeCache.txt) -and (Test-Path work/build-android/CMakeCache.txt)) {
    # The Android 10 tree: the same generator, toolchain, ninja, build type and game as work/build-android, platform android-29
    $cache = Get-Content work/build-android/CMakeCache.txt
    function Cache-Value([string]$key) { $l = $cache | Where-Object { $_ -match "^$([regex]::Escape($key)):[A-Z]+=" } | Select-Object -First 1; if ($l) { $l -replace '^[^=]*=', '' } else { '' } }
    $cfg = @('-S', 'dm/android', '-B', $bdir, '-G', (Cache-Value 'CMAKE_GENERATOR'), "-DCMAKE_TOOLCHAIN_FILE=$(Cache-Value 'CMAKE_TOOLCHAIN_FILE')",
        '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29', "-DCMAKE_BUILD_TYPE=$(Cache-Value 'CMAKE_BUILD_TYPE')", '-DDM_GAME=d3')
    $mk = Cache-Value 'CMAKE_MAKE_PROGRAM'; if ($mk) { $cfg += "-DCMAKE_MAKE_PROGRAM=$mk" }
    New-Item -ItemType Directory -Force work/logs | Out-Null
    & cmake @cfg *> work/logs/native-configure29.log
    if ($LASTEXITCODE) { throw "Configuring $bdir failed (work/logs/native-configure29.log)" }
}
if (-not (Test-Path $bdir/CMakeCache.txt)) { throw "Configure the native build first: cmake -S dm/android -B $bdir ... -DDM_GAME=d3$(if ($Android10) { ' -DANDROID_PLATFORM=android-29' }) (README.md, Building)" }
if (-not (Select-String -Path $bdir/CMakeCache.txt -Pattern '^DM_GAME:STRING=d3$' -Quiet)) { throw "$bdir is not configured with -DDM_GAME=d3" }
New-Item -ItemType Directory -Force work/logs, $jdir/arm64-v8a | Out-Null

& cmake --build $bdir --target dm *> work/logs/native-dm.log
if ($LASTEXITCODE) { throw 'Native build failed (work/logs/native-dm.log)' }
Copy-Item $bdir/libdm.so $jdir/arm64-v8a/libdm.so
# adrenotools hook libraries (loaded from nativeLibraryDir when a custom Vulkan driver such as Turnip is chosen)
& cmake --build $bdir --target main_hook file_redirect_hook gsl_alloc_hook hook_impl *> work/logs/native-hooks.log
if ($LASTEXITCODE) { throw 'Hook build failed (work/logs/native-hooks.log)' }
Get-ChildItem $bdir/adrenotools -Recurse -Filter '*.so' | Copy-Item -Destination $jdir/arm64-v8a
# -Validation: package the Khronos validation layer for DM_VK_VALIDATION=1 runs. Download the Android release of
# Vulkan-ValidationLayers yourself and put arm64-v8a/libVkLayer_khronos_validation.so under work/vvl (not redistributed here).
$vvl = "$jdir/arm64-v8a/libVkLayer_khronos_validation.so"
if ($Validation) {
    $layer = Get-ChildItem work/vvl -Recurse -Filter libVkLayer_khronos_validation.so | Where-Object { $_.FullName -match 'arm64-v8a' } | Select-Object -First 1
    if (-not $layer) { throw 'No arm64-v8a libVkLayer_khronos_validation.so under work/vvl' }
    Copy-Item $layer.FullName $vvl
} elseif (Test-Path $vvl) { Remove-Item $vvl }

$jni = (Resolve-Path $jdir).Path -replace '\\', '/'
# Default Vulkan driver bundled as assets: Turnip (Mesa, MIT) fetched by tools/fetch_turnip.py with a pinned SHA-256;
# without it the APK simply uses the system driver.
& $python tools/fetch_turnip.py *> work/logs/fetch-turnip.log
# Gamepad themes and the default on-screen layout of the front end (dm/android/front-assets, nx2android themes, MIT) go in beside the driver
$fa = 'work/front-assets'
if (Test-Path $fa) { Remove-Item -Recurse -Force $fa }
New-Item -ItemType Directory -Force $fa | Out-Null
if (Test-Path work/driver-assets/drivers) { Copy-Item -Recurse work/driver-assets/drivers $fa }
Copy-Item -Recurse dm/android/front-assets/* $fa
$drv = '-PdmDriverAssets=' + ((Resolve-Path $fa).Path -replace '\\', '/')
if (-not (Get-Command $gradle -ErrorAction SilentlyContinue)) { throw "Gradle not found: '$gradle' (put gradle on PATH or set GRADLE to gradle.bat)" }
$built = Join-Path $PWD 'dm/android/app/build/outputs/apk/debug/app-debug.apk'; if (Test-Path $built) { Remove-Item $built }   # never re-sign a stale APK
& $gradle -p dm/android :app:assembleDebug '-PdmGame=d3' '-PdmAppId=org.diablo3.port' "-PdmPython=$python" "-PdmPrebuiltJni=$jni" "-PdmMinSdk=$minsdk" --console plain $drv *> work/logs/gradle-apk.log
if ($LASTEXITCODE) { throw 'APK build failed (work/logs/gradle-apk.log)' }
if (-not (Test-Path $built)) { throw 'APK build produced no app-debug.apk (work/logs/gradle-apk.log)' }
if (-not (Test-Path $keystore)) { throw "Keystore not found: $keystore (set DM_KEYSTORE; Android Studio creates ~/.android/debug.keystore, or: keytool -genkeypair -keystore <file> -alias androiddebugkey -storepass android -keypass android -keyalg RSA -validity 10000 -dname CN=Android)" }
& $apksigner sign --ks $keystore --ks-pass "pass:$kspass" --key-pass "pass:$kspass" --ks-key-alias $alias --out $apk dm/android/app/build/outputs/apk/debug/app-debug.apk
if ($LASTEXITCODE) { throw 'APK signing failed' }
Get-Item $apk | Select-Object FullName, Length
if ($Install) {
    & $adb -s $Install install -r $apk
    if ($LASTEXITCODE) { throw 'APK installation failed' }
}
