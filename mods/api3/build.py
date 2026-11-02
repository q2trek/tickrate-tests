#!/usr/bin/env python3
"""Build the API3 harness against an OpenFFA or OpenTDM checkout.

The module is every *.c of the mod root except the optional and platform
files its Makefile leaves out of a plain Linux build. config.h and the
project-wide C flags come from a plain native Meson build of q2pro.

With --engine the harness is linked with the objects of the q2proded build
(system.c.o with `main` renamed, as engine/scheduler does) and engine.c:
the engine collision fixture.

usage: build.py --engine-build DIR --out DIR --mod openffa|opentdm --mod-src DIR
                [--engine] [--replace FILE=COPY]...
--replace compiles COPY instead of FILE of the mod root (negative controls).
"""
import argparse
import json
import shlex
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent

# left out of the module: optional features (curl, sqlite, udp) and Windows
SKIP = {'openffa': {'g_curl.c', 'g_sqlite.c', 'g_udp.c'}, 'opentdm': {'sys_win32.c'}}
MOD_ARGS = {'openffa': ['-DOPENFFA_VERSION="tickrate-tests"', '-DOPENFFA_REVISION=0'],
            'opentdm': ['-DC_ONLY', '-DOPENTDM_VERSION="tickrate-tests"']}

# T_Damage normalizes `dir` into an uninitialized local (VectorNormalize2
# leaves it untouched for a zero vector); hurt/crush sources pass vec3_origin,
# so in any build their knockback reads stale stack and can add falling
# damage at random. The test build pins that stale value to 0.
UB_PIN = ['-ftrivial-auto-var-init=zero']

# gcc test_args of q2pro/meson.build that become project-wide common_args
PROJECT_F_FLAGS = ('-fms-extensions', '-fno-math-errno', '-fno-trapping-math', '-fsigned-char')


def project_flags(build):
    """-std and project-wide arguments as Meson used them for a common source."""
    for entry in json.loads((build / 'compile_commands.json').read_text()):
        if entry['file'].endswith('src/common/cmd.c'):
            argv = shlex.split(entry['command'])
            break
    else:
        raise SystemExit('no compile command for src/common/cmd.c')
    keep = [x for x in argv[1:] if x.startswith(('-std=', '-O')) or x == '-g' or
            x in ('-DHAVE_CONFIG_H', '-D_GNU_SOURCE') or x in PROJECT_F_FLAGS]
    assert '-DHAVE_CONFIG_H' in keep and any(a.startswith('-std=') for a in keep), keep
    return keep


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


def run(cmd, cwd=None):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        print(' '.join(map(str, cmd)))
        print(result.stdout + result.stderr)
        raise SystemExit('build failed')


def engine_objects(build, out):
    """q2proded objects (main renamed) plus engine.c, compiled like src/server/main.c"""
    line = subprocess.run(['ninja', '-C', str(build), '-t', 'commands', 'q2proded'],
                          capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    line = shlex.split(line)
    objects = [str(build / x) for x in line if x.endswith('.o')]
    libs = [x for x in line if x.startswith(('-l', '-Wl,')) or x.endswith(('.so', '.a'))]
    system = next(o for o in objects if o.endswith('src_unix_system.c.o'))
    renamed = out / 'system-main-renamed.o'
    run(['objcopy', '--redefine-sym', 'main=q2pro_real_main', system, str(renamed)])
    objects[objects.index(system)] = str(renamed)
    entry = next(e for e in json.loads((build / 'compile_commands.json').read_text())
                 if e['file'].endswith('src/server/main.c') and 'q2proded.p/' in e['output'])
    flags = [x for x in strip_output(shlex.split(entry['command']))
             if x not in ('-fvisibility=hidden', '-Wmissing-prototypes')]
    run(flags + ['-c', str(HERE / 'engine.c'), '-o', str(out / 'engine.o')], cwd=entry['directory'])
    return [line[0]], [str(out / 'engine.o')] + objects, libs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine-build', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--mod', required=True, choices=SKIP)
    ap.add_argument('--mod-src', required=True, help='checkout of the mod')
    ap.add_argument('--replace', action='append', default=[])
    ap.add_argument('--cc', default='cc')
    ap.add_argument('--engine', action='store_true', help='link the engine collision fixture')
    a = ap.parse_args()
    build = Path(a.engine_build).resolve()
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = Path(a.mod_src).resolve()
    sources = sorted(p for p in root.glob('*.c') if p.name not in SKIP[a.mod])
    replace = {}
    for item in a.replace:
        name, copy = item.split('=', 1)
        assert root / name in sources, f'replacement target not in module: {name}'
        # a copy alone in its directory, so that its quoted includes are the
        # checkout's headers, not the ones next to an upstream file
        flat = out / 'replaced' / name
        flat.parent.mkdir(exist_ok=True)
        shutil.copyfile(copy, flat)
        replace[root / name] = flat

    flags = project_flags(build)
    mod_args = ['-include', str(HERE / 'namespace.h')] + MOD_ARGS[a.mod]
    incs = ['-I' + str(build), '-I' + str(root)]

    def compile_one(src):
        obj = out / (src.stem + '.o')
        run([a.cc] + flags + mod_args + UB_PIN + incs + ['-iquote', str(root), '-c', str(replace.get(src, src)),
                                                          '-o', str(obj)])
        return str(obj)

    with ThreadPoolExecutor() as pool:
        objects = list(pool.map(compile_one, sources))
    define = '-DHARNESS_OPENTDM' if a.mod == 'opentdm' else '-DHARNESS_OPENFFA'
    engine = ['-DHARNESS_ENGINE'] if a.engine else []
    # API3 const-ness differs between the two mods' headers (same ABI)
    run([a.cc] + flags + mod_args + incs + engine + ['-Wno-incompatible-pointer-types', define,
                                                      f'-DHARNESS_MOD="{a.mod}"', '-I' + str(HERE), f'-ffile-prefix-map={HERE.parent.parent}/=',
                                                      '-c', str(HERE / 'harness.c'), '-o', str(out / 'harness.o')])
    link, libs = [a.cc], ['-lm', '-ldl']
    if a.engine:
        link, extra, libs = engine_objects(build, out)
        objects += extra
    run(link + ['-o', str(out / 'harness'), str(out / 'harness.o')] + objects + libs)
    print(f'built {out / "harness"} ({a.mod}, {len(objects)} objects{", engine collision" if a.engine else ""})')


if __name__ == '__main__':
    main()
