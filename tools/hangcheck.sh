#!/bin/bash
# tools/hangcheck.sh <name> [secs=45] [ENV...]: one tools/run.sh run, then report whether game frames (NVN PERF lines) kept coming until the end.
# Device: ADB_SERIAL / ADB as in tools/run.sh.
cd "$(dirname "$0")/.."
NAME=$1; SECS=${2:-45}; shift; [ $# -gt 0 ] && shift
bash tools/run.sh "$NAME" "$SECS" "$@" > work/logs/$NAME.run.txt 2>&1
L=work/logs/$NAME.dm.log
first=$(grep -m1 "present #1 " $L | awk '{print $2}'); last=$(grep "PERF v=1" $L | tail -1 | awk '{print $2}'); end=$(grep -m1 "CRASH" $L | awk '{print $2}')
n=$(grep -c "PERF v=1" $L); crash=$(grep -m1 -E "Fatal signal|FATAL|die:" $L | cut -c 50-160)
echo "$NAME env=[$*] first=$first lastperf=$last end=$end perf_lines=$n ${crash:+CRASH:$crash}"
