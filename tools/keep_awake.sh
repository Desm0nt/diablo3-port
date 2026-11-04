#!/bin/bash
# Keep the device's screen from timing out during long unattended test runs: a harmless key event (Scroll Lock, ignored by the app) every 50 s
# counts as user activity. No settings are changed; stop with: touch work/stop_awake. Device: ADB_SERIAL / ADB as in tools/run.sh.
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."; mkdir -p work
rm -f work/stop_awake
while [ ! -f work/stop_awake ]; do ${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554} shell input keyevent 116 >/dev/null 2>&1; sleep 50; done
