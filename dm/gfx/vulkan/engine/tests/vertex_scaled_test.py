# [vtxscaled] Host run of the scaled-vertex-format fallback (xbg_vertex_retype.h conversion kinds, xbg_vendor_policy.h `noscaled`):
# builds and runs vertex_retype_test.cpp and vendor_policy_test.cpp, then spirv-val (--target-env vulkan1.1) over every synthetic
# module and its conversion variants (float / sint / uint inputs of 1..4 components, sint -> float and uint -> float).
#   python3 vertex_scaled_test.py     env: CXX (default g++), SPIRV_VAL (default spirv-val), VULKAN_INCLUDE (a directory holding
#                                     vulkan/vulkan.h, e.g. <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include)
import os, subprocess, sys, tempfile
from pathlib import Path
here = Path(__file__).resolve().parent
cxx = os.environ.get('CXX', 'g++'); val = os.environ.get('SPIRV_VAL', 'spirv-val'); inc = os.environ.get('VULKAN_INCLUDE')
def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: print('FAIL:', ' '.join(map(str, cmd))); print(r.stdout + r.stderr); sys.exit(1)
    return r.stdout.strip()
with tempfile.TemporaryDirectory() as tmp:
    t = Path(tmp)
    run([cxx, '-std=c++17', '-Wall', '-o', t / 'vrt', here / 'vertex_retype_test.cpp'])
    print(run([t / 'vrt']))
    run([cxx, '-std=c++17', '-Wall'] + (['-idirafter', inc] if inc else []) + ['-o', t / 'vpt', here / 'vendor_policy_test.cpp'])
    print(run([t / 'vpt']))
    spv = t / 'spv'; spv.mkdir()
    print(run([t / 'vrt', '--emit', spv]))
    files = sorted(spv.glob('*.spv'))
    # a Windows spirv-val.exe started from WSL needs Windows paths
    win = val.lower().endswith('.exe') and os.path.exists('/usr/bin/wslpath')
    for f in files: run([val, '--target-env', 'vulkan1.1', run(['wslpath', '-w', f]) if win else f])
    print('PASS: spirv-val clean: %d modules (%d conversion variants)' % (len(files), sum(1 for f in files if '2f' in f.name or '.out' in f.name)))
