#!/bin/bash
# tools/shots.sh <name> [seconds=75] [ENV=VALUE ...]: tools/run.sh plus a screenshot every 10 s (work/logs/<name>_<t>.png)
# Device: ADB_SERIAL / ADB as in tools/run.sh.
cd "$(dirname "$0")/.."
NAME=${1:?name}; SECS=${2:-75}
export MSYS_NO_PATHCONV=1; A="${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554}"
bash tools/run.sh "$@" > work/logs/$NAME.run.txt 2>&1 & rp=$!
for ((t=10; t<SECS; t+=10)); do sleep 10; kill -0 $rp 2>/dev/null || break; $A exec-out screencap -p > work/logs/${NAME}_$t.png 2>/dev/null; done
wait $rp; cut -c1-300 work/logs/$NAME.run.txt | tail -12
