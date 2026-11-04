#!/bin/sh
# Diablo III (Switch): NVN shaders (Shaders/*.shd in ClientCommon.cpk + patch CPKs) -> XGS1 Vulkan bundles <key>.xgs (offline; no device).
# Notes: docs/d3/SHADERS.md. tools/setup_d3.py runs the same four steps itself; this script is the developer's shortcut.
#   1. tools/d3_cpk.py mextract       $WORK/romfs (base + Update/CPKs, later patch wins) -> $WORK/cpk-shaders/Shaders/*.shd + _sources.tsv
#      [EXTRACT=1 or missing]. ALL=1: every stored version instead (each archive's .shd into $WORK/cpk-shaders-all/<archive>/), a superset
#      (+41 vertex keys of base .shd versions that the patches replace)
#   2. tools/d3_shader_jobs.py        -> $WORK/shaders-jobs (rec/<key>.rec, jobs.tsv, programs.tsv, shader_map.tsv, shader_facts.tsv, report.txt)
#   3. tools/build_dmsh_xlate_d3.py   -> $WORK/build/dmsh_xlate_d3/bin/dmsh_xlate(.exe) (the XC2 translator source; rebuilt when it changes)
#   4. dm/gfx/tools/dmsh_vk_xc2.py    -> <out>/<key>.xgs, <out>/_vk_build.txt / _vk_build.json / _chain.json
# Usage: tools/d3_shaders_vk.sh [out dir] [keys... | @file]   (default out: $WORK/shaders-vk; no keys = every key; a full run empties out first)
# Environment: WORK (default <repo>/work; the RomFS is read from $WORK/romfs), PYTHON (default python3, else python), J (parallel translator
#   processes, default 12), EXTRACT=1, ALL=1, DOTNET (.NET 10 SDK), SPIRV_OPT / SPIRV_VAL (optional; default: the NDK's shader-tools when
#   ANDROID_NDK_HOME is set), D3_DEPTH_ZERO=0 (see d3_shader_jobs.py)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$ROOT/work}
OUT=${1:-$WORK/shaders-vk}; [ $# -gt 0 ] && shift
PY=${PYTHON:-$(command -v python3 >/dev/null 2>&1 && echo python3 || echo python)}
export J=${J:-12} PYTHONUTF8=1
w() { cygpath -w "$1" 2>/dev/null || echo "$1"; }
if [ -z "$ALL" ]; then
  SHD=$WORK/cpk-shaders
  if [ -n "$EXTRACT" ] || [ ! -f "$SHD/_sources.tsv" ]; then
    rm -rf "$SHD"; "$PY" "$ROOT/tools/d3_cpk.py" mextract "$(w "$WORK/romfs")" "$(w "$SHD")" '*.shd'
  fi
else
  SHD=$WORK/cpk-shaders-all
  if [ -n "$EXTRACT" ] || [ ! -d "$SHD" ]; then
    rm -rf "$SHD"
    for c in "$WORK"/romfs/CPKs/*.cpk "$WORK"/romfs/Update/CPKs/*.cpk; do
      "$PY" "$ROOT/tools/d3_cpk.py" extract "$(w "$c")" "$(w "$SHD/$(basename "$c" .cpk)")" '*.shd' 2>&1 | grep -v '^0 files' || true
    done
  fi
fi
"$PY" "$ROOT/tools/d3_shader_jobs.py" "$(w "$SHD")" "$(w "$WORK/shaders-jobs")"
"$PY" "$ROOT/tools/build_dmsh_xlate_d3.py" "$(w "$WORK/build/dmsh_xlate_d3")" > /dev/null
EXE=$WORK/build/dmsh_xlate_d3/bin/dmsh_xlate; [ -f "$EXE.exe" ] && EXE=$EXE.exe
export XLATE=$(w "$EXE")
if [ -z "${SPIRV_VAL:-}" ] && [ -n "${ANDROID_NDK_HOME:-}" ]; then
  for t in "$ANDROID_NDK_HOME"/shader-tools/*/spirv-val*; do [ -f "$t" ] && SPIRV_VAL=$(w "$t"); done
  for t in "$ANDROID_NDK_HOME"/shader-tools/*/spirv-opt*; do [ -f "$t" ] && SPIRV_OPT=$(w "$t"); done
fi
[ -n "${SPIRV_VAL:-}" ] && export SPIRV_VAL; [ -n "${SPIRV_OPT:-}" ] && export SPIRV_OPT
[ $# -eq 0 ] && rm -rf "$OUT"
mkdir -p "$OUT"
"$PY" "$ROOT/dm/gfx/tools/dmsh_vk_xc2.py" "$(w "$WORK/shaders-jobs")" "$(w "$OUT")" "$@"
