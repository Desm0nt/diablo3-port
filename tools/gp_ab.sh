#!/bin/bash
# tools/gp_ab.sh <name> [ENV...]: scripted route into Act 1 gameplay (existing hero; A presses), 95 s; prints median of the last 15 NVN PERF lines.
# Device: ADB_SERIAL / ADB as in tools/run.sh; PYTHON (default python3, else python).
cd "$(dirname "$0")/.."
PY=${PYTHON:-$(command -v python3 >/dev/null 2>&1 && echo python3 || echo python)}
N=$1; shift
K="DM_KEYS=A@18,A@26,A@34,A@42,A@50,A@58,LSU@72+3,LSR@76+3,LSD@80+3,LSL@84+3"
bash tools/run.sh "$N" 95 "$K" "$@" > work/logs/$N.run.txt 2>&1
"$PY" - "$N" "$*" <<'PY'
import re,sys,statistics as st
L=[l for l in open(f'work/logs/{sys.argv[1]}.dm.log',encoding='utf-8',errors='ignore') if 'PERF v=1' in l][-15:]
g=lambda k:[float(re.search(k+r'=([0-9.]+)',l).group(1)) for l in L if re.search(k+r'=([0-9.]+)',l)]
crash=any(re.search('Fatal signal|FATAL|die:',l) for l in open(f'work/logs/{sys.argv[1]}.dm.log',encoding='utf-8',errors='ignore'))
print(sys.argv[1], sys.argv[2], 'n=%d'%len(L), ' '.join('%s=%.1f'%(k,st.median(g(k))) for k in ('fps','submit_cpu_ms','gpu_ms','sync_q','draws') if g(k)), 'CRASH' if crash else '')
PY
