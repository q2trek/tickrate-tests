#!/usr/bin/env python3
"""Link an engine harness from the objects of a plain native q2pro build.

scheduler: the dedicated server (q2proded); client: client + listen server
(q2pro). The object list, compile flags and libraries are read from the
Meson/ninja build directory; no engine source is copied. system.c.o gets
`main` renamed so that the driver owns the loop. Also builds the harness's
test game and a basedir with read-only links to the paks of DATA/baseq2.

usage: build.py scheduler|client --engine-build DIR --out DIR [--data DIR]
                [--replace SRC=COPY]...
--replace compiles COPY with the compile command of SRC (negative controls,
upstream peers).
"""
import argparse
import json
import shlex
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
TARGETS = {
    'scheduler': ('q2proded', 'src/server/main.c',
                  ['Sys_Milliseconds', 'NET_Sleep', 'SV_Frame', 'MSG_WriteDeltaEntity']),
    'client': ('q2pro', 'src/client/main.c',
               ['Sys_Milliseconds', 'NET_Sleep', 'CL_InitRefresh', 'SCR_UpdateScreen', 'SCR_BeginLoadingPlaque',
                'NET_SendPacket', 'CL_DeltaFrame', 'CL_ClearState', 'SV_WriteFrameToClient_Enhanced',
                'SV_ClientAddMessage', 'SV_Frame']),
}


def run(cmd, cwd=None):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        print(' '.join(map(str, cmd)))
        print(result.stdout + result.stderr)
        raise SystemExit('build failed')


def compile_entry(build, target, suffix):
    for entry in json.loads((build / 'compile_commands.json').read_text()):
        if entry['file'].endswith(suffix) and f'{target}.p/' in entry['output']:
            return entry
    raise SystemExit(f'no {target} compile command for {suffix}')


def strip_output(argv):
    """compile command without dependency/output/input arguments"""
    out, skip = [], 0
    for arg in argv:
        if skip:
            skip -= 1
        elif arg in ('-MQ', '-MF', '-o', '-c'):
            skip = 1
        elif arg != '-MD':
            out.append(arg)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('harness', choices=TARGETS)
    ap.add_argument('--engine-build', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--data', help='Quake 2 directory with baseq2/pak*.pak')
    ap.add_argument('--replace', action='append', default=[])
    a = ap.parse_args()
    target, main_src, wraps = TARGETS[a.harness]
    src_dir = HERE / a.harness
    build = Path(a.engine_build).resolve()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    link = subprocess.run(['ninja', '-C', str(build), '-t', 'commands', target],
                          capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    link = shlex.split(link)
    objects = [str(build / x) for x in link if x.endswith('.o')]
    libs = [x for x in link if x.startswith(('-l', '-Wl,')) or x.endswith(('.so', '.a'))]

    for item in a.replace:
        src, copy = item.split('=', 1)
        entry = compile_entry(build, target, src)
        obj = out / ('replaced-' + Path(src).name + '.o')
        # a copy alone in its directory, so that its quoted includes are the
        # build's headers (an upstream file next to upstream headers would
        # compile against another struct layout)
        flat = out / 'replaced' / Path(src).name
        flat.parent.mkdir(exist_ok=True)
        shutil.copyfile(copy, flat)
        quoted = (Path(entry['directory']) / entry['file']).resolve().parent
        run(strip_output(shlex.split(entry['command'])) + ['-iquote', str(quoted), '-c', str(flat),
                                                           '-o', str(obj)], cwd=entry['directory'])
        dest = str(build / entry['output'])
        assert dest in objects, dest
        objects[objects.index(dest)] = str(obj)

    system = next(o for o in objects if o.endswith('src_unix_system.c.o'))
    renamed = out / 'system-main-renamed.o'
    run(['objcopy', '--redefine-sym', 'main=q2pro_real_main', system, str(renamed)])
    objects[objects.index(system)] = str(renamed)

    main_entry = compile_entry(build, target, main_src)
    flags = [x for x in strip_output(shlex.split(main_entry['command']))
             if x not in ('-fvisibility=hidden', '-Wmissing-prototypes')]
    # __FILE__ in assertion lines relative to the repository
    flags += [f'-ffile-prefix-map={HERE.parent}/=']
    run(flags + ['-fdiagnostics-color=never', '-c', str(src_dir / 'driver.c'), '-o', str(out / 'driver.o')],
        cwd=main_entry['directory'])
    run([link[0], '-o', str(out / 'driver'), str(out / 'driver.o')] + objects + ['-rdynamic'] +
        ['-Wl,--wrap=' + w for w in wraps] + libs)

    game_flags = [x for x in flags if x.startswith(('-I', '-std=', '-D', '-f', '-O', '-g'))]
    run([flags[0]] + game_flags + ['-fPIC', '-shared', str(src_dir / 'testgame.c'), '-o', str(out / 'testgame.so')],
        cwd=main_entry['directory'])

    base = out / 'base/baseq2'
    base.mkdir(parents=True, exist_ok=True)
    if a.data:
        for pak in sorted((Path(a.data) / 'baseq2').glob('pak*.pak')):
            if not (base / pak.name).exists():
                (base / pak.name).symlink_to(pak.resolve())
    print(f'built {out / "driver"} ({len(objects)} engine objects, wraps {",".join(wraps)})')


if __name__ == '__main__':
    main()
