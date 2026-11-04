#!/bin/bash
# Push everything Diablo III Mobile needs to a phone without starting the game: patched modules (-> dm/out), shaders (-> dm/shaders),
# the RomFS (-> romfs: 37 .cpk files, 16 GiB), language-pack DLC (-> aoc/<index>), the Turnip 27 adrenotools package (also bundled in the
# APK) and the APK. Layout on the phone: /sdcard/Download/Diablo3Mobile/{romfs,aoc,dm,drivers}.
# Sources (defaults = the folder tools/setup_d3.py prepares, work/Diablo3Mobile):
#   PREP=<dir> (default work/Diablo3Mobile) with romfs/, aoc/<index>/, dm/out/, dm/shaders/; APK=<file> (default work/D3-port-debug.apk);
#   DRIVER=<dir> (default work/driver-assets/drivers/turnip27, from tools/fetch_turnip.py). Device: ADB_SERIAL / ADB as in tools/run.sh.
# Big files: plain adb push first, tools/push_big.sh (resumable 256 MiB chunks) when it fails or the size is wrong; files already on
# the phone with the right size are skipped, so the script can simply be run again after an interruption. Never kills the adb server.
# (setup push [serial] does the same for the data folder without the APK / driver.)
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
export ADB_SERIAL=${ADB_SERIAL:-emulator-5554}
ADB="${ADB:-adb} -s $ADB_SERIAL"
R=/sdcard/Download/Diablo3Mobile
P=${PREP:-work/Diablo3Mobile}; APK=${APK:-work/D3-port-debug.apk}; DRV=${DRIVER:-work/driver-assets/drivers/turnip27}
rsize() { $ADB shell "stat -c %s '$1' 2>/dev/null" | tr -d '\r'; }
big() {   # <local> <remote>
  local want=$(stat -c %s "$1")
  local have=$(rsize "$2")
  [ "$have" = "$want" ] && { echo "ok $2"; return; }
  # a partial file from an earlier run: resume in chunks instead of starting the plain push over
  if [ -n "$have" ] && [ "$have" -gt 0 ] 2>/dev/null; then bash tools/push_big.sh "$1" "$2" 256 | tail -1; return; fi
  $ADB push "$1" "$2" 2>&1 | tail -1
  [ "$(rsize "$2")" = "$want" ] || { echo "plain push failed, chunks: $2"; $ADB shell "rm -f '$2'"; bash tools/push_big.sh "$1" "$2" 256 | tail -1; }
}
$ADB shell "mkdir -p $R/romfs/CPKs $R/romfs/Update/CPKs $R/dm/out $R/dm/shaders $R/drivers $R/aoc"
# small things first, so a first boot can start while the RomFS is still going (it will fail on the missing archives, not on the host)
(cd "$P/dm/out" && $ADB push . $R/dm/out/ 2>&1 | tail -1)
(cd "$P/dm/shaders" && $ADB push . $R/dm/shaders/ 2>&1 | tail -1)
[ -d "$DRV" ] && $ADB push "$DRV" $R/drivers/ 2>&1 | tail -1
[ -f "$APK" ] && $ADB install -r "$APK" 2>&1 | tail -1
$ADB shell appops set org.diablo3.port MANAGE_EXTERNAL_STORAGE allow
# RomFS and DLC: smallest archives first (the core ones a boot reads early), each resumable
# (a for loop, not "while read": adb shell would eat the list from stdin; .cpk names have no spaces)
for f in $(cd "$P/romfs" && find . -type f -printf '%s %P\n' | sort -n | cut -d' ' -f2-); do big "$P/romfs/$f" "$R/romfs/$f" < /dev/null; done
if [ -d "$P/aoc" ]; then
  for f in $(cd "$P/aoc" && find . -type f -printf '%s %P\n' | sort -n | cut -d' ' -f2-); do
    $ADB shell "mkdir -p '$R/aoc/$(dirname "$f")'" < /dev/null; big "$P/aoc/$f" "$R/aoc/$f" < /dev/null
  done
fi
echo "checks:"
$ADB shell "find $R/romfs -name '*.cpk' | wc -l; ls $R/dm/out | wc -l; ls $R/dm/shaders | wc -l; ls $R/aoc; du -sh $R"
echo "expected: $(cd "$P/romfs" && find . -name '*.cpk' | wc -l) cpk, $(ls "$P/dm/out" | wc -l) out files, $(ls "$P/dm/shaders" | wc -l) shader files"
