#!/bin/bash
# Diablo III Mobile: start the game, wait for the main menu (a dm log line matching DM_MENU_RE; no D3 menu marker is known yet, so by default it
# waits the full 100 s), screenshot after N s (default 15) -> work/logs/<name>.png (+_s, needs Pillow), stop.
# Usage: tools/menu_shot.sh <name> [seconds after the menu]. Device: ADB_SERIAL / ADB as in tools/run.sh.
cd "$(dirname "$0")/.."; export MSYS_NO_PATHCONV=1; A="${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554}"
PY=${PYTHON:-$(command -v python3 >/dev/null 2>&1 && echo python3 || echo python)}
mkdir -p work/logs
$A shell am force-stop org.diablo3.port; $A logcat -c; $A shell monkey -p org.diablo3.port -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
for t in 1 2 3 4 5 6 7 8 9 10; do sleep 10; $A logcat -d -s dm:* | grep -qE "${DM_MENU_RE:-__no_menu_marker_yet__}" && break; done
sleep ${2:-15}; $A exec-out screencap -p > work/logs/$1.png; $A logcat -d > work/logs/$1.log; $A shell am force-stop org.diablo3.port
"$PY" -c "from PIL import Image; im=Image.open('work/logs/$1.png'); im.thumbnail((800,800)); im.save('work/logs/$1_s.png')"
