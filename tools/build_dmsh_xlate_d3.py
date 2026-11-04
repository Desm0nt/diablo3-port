"""Build the shader translator (dmsh_xlate) for Diablo III: the XC2 variant (dm/gfx/tools/dmsh_xlate_xc2) built into work/build/dmsh_xlate_d3.

D3 needs nothing translator-specific (graphics vs + ps only, see docs/d3/SHADERS.md). Source: dm/gfx/tools/dmsh_xlate_xc2/{Program.cs,
dmsh_xlate.csproj} (the DOOM translator with an XC2 header; see its top comment). Built with the .NET 10 SDK against dm/third_party/ryujinx
(unmodified Ryujinx 1.3.2); the first build downloads NuGet packages.
Usage: build_dmsh_xlate_d3.py [out dir] (default work/build/dmsh_xlate_d3); DOTNET=<dotnet executable> (default: dotnet on PATH)
Result: <out>/bin/dmsh_xlate(.exe); <out>/bin/source.sha = the embedded source sha (dmsh_xlate --source-sha), checked against the tree.
"""
import hashlib, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'dm' / 'gfx' / 'tools' / 'dmsh_xlate_xc2'
RYU = ROOT / 'dm' / 'third_party' / 'ryujinx'
FILES = ('Program.cs', 'dmsh_xlate.csproj')
EXE = 'dmsh_xlate.exe' if os.name == 'nt' else 'dmsh_xlate'


def source_sha():   # same convention as dmsh_xlate --source-sha: the embedded Program.cs and csproj concatenated in that order
    return hashlib.sha256(b''.join((SRC / f).read_bytes() for f in FILES)).hexdigest()[:16]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'work' / 'build' / 'dmsh_xlate_d3'
    dotnet = os.environ.get('DOTNET') or shutil.which('dotnet') or sys.exit('[red] the .NET 10 SDK is needed: set DOTNET=<dotnet> or put dotnet on PATH')
    exe = out / 'bin' / EXE
    if exe.exists() and (out / 'bin' / 'source.sha').exists() and (out / 'bin' / 'source.sha').read_text().strip() == source_sha():
        print('up to date', exe, 'source', source_sha()); return
    src = out / 'src'
    if src.exists(): shutil.rmtree(src)
    src.mkdir(parents=True)
    for f in FILES: shutil.copyfile(SRC / f, src / f)   # build a copy: obj/ and bin/ stay out of the source tree
    r = subprocess.run([dotnet, 'build', str(src / 'dmsh_xlate.csproj'), '-c', 'Release', f'-p:RyuSrc={RYU.as_posix()}',
                        '-o', str(out / 'bin'), '--artifacts-path', str(out / 'obj')], capture_output=True, text=True, errors='replace')
    print(r.stdout[-1500:], r.stderr[-1500:])
    if r.returncode: sys.exit(r.returncode)
    got = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True).stdout.strip()
    if got != source_sha(): sys.exit(f'[red] built translator embeds source {got}, tree has {source_sha()}')
    (out / 'bin' / 'source.sha').write_text(got + '\n')
    print('built', exe, 'source', got)


if __name__ == '__main__':
    main()
