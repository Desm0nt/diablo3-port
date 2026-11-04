#!/bin/bash
# Push one big file to the device in CHUNK-MiB pieces, resuming from the remote size: each piece is streamed with adb exec-in
# into <remote>.part, checked by size, then appended on the device (cat >>) - a dropped adb connection costs one piece.
# (Some emulators' toybox has no dd.) Paths must not contain spaces. Usage: push_big.sh <local file> <remote file> [chunk MiB=256]
# Device: ADB_SERIAL / ADB as in tools/run.sh.
export MSYS_NO_PATHCONV=1
ADB="${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554}"
L=$1; R=$2; C=${3:-256}; MB=$((1<<20)); P=$R.part
size=$(stat -c %s "$L"); n=$(( (size + C*MB - 1) / (C*MB) ))
rs() { $ADB shell "stat -c %s $1 2>/dev/null || echo 0" | tr -d '\r'; }
for ((fails=0; fails<60; )); do
  have=$(rs $R); case "$have" in (*[!0-9]*|"") echo "stat failed: $have"; fails=$((fails+1)); sleep 5; continue;; esac
  [ "$have" -ge "$size" ] && { echo "done $have"; exit 0; }
  [ $((have % (C*MB))) -ne 0 ] && { echo "remote size $have is not on a chunk boundary"; exit 1; }
  i=$(( have / (C*MB) )); want=$(( size - i*C*MB < C*MB ? size - i*C*MB : C*MB ))
  dd if="$L" bs=1M skip=$((i*C)) count=$C 2>/dev/null | $ADB exec-in "cat > $P"
  got=$(rs $P)
  if [ "$got" = "$want" ]; then $ADB shell "cat $P >> $R && rm $P"; echo "chunk $((i+1))/$n ok"
  else fails=$((fails+1)); echo "chunk $((i+1)) short: $got/$want ($fails)"; sleep 3; fi
done
echo "gave up"; exit 1
