"""Builds the recompiled game for the Xbox 360 with its registers in locals (regs_local.py), adding any function the
VS2010 optimizer fails on (C1001) to regs_skip.txt and building again, until it compiles.

  python x360/tools/regs_build.py [--x87]

--x87 also keeps the top of the x87 stack in locals (x87_local.py).
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..'))
gen = os.path.join(root, 'pc-recomp', 'generated')
orig, regs, game = os.path.join(gen, 'game_orig'), os.path.join(gen, 'game_regs'), os.path.join(gen, 'game')
skip_file = os.path.join(here, 'regs_skip.txt')
ICE = re.compile(r'x86_recomp\.(\d+)\.cpp\((\d+)\) : fatal error C1001')

for attempt in range(8):
    subprocess.check_call([sys.executable, os.path.join(here, 'regs_local.py'), orig, regs])
    if '--x87' in sys.argv:
        subprocess.check_call([sys.executable, os.path.join(here, 'x87_local.py'), regs])
    subprocess.check_call([sys.executable, os.path.join(here, 'backedge.py'), regs])
    subprocess.check_call([sys.executable, os.path.join(here, 'sse_local.py'), regs])
    subprocess.check_call([sys.executable, os.path.join(here, 'call_local.py'), regs,
                           os.path.join(root, 'pc-recomp', 'config', 'game.toml'), os.path.join(root, 'x360', 'runtime')])
    for name in os.listdir(regs):
        if re.match(r'x86_recomp\.\d+\.cpp$|x86_imp_decls\.h$', name):
            with open(os.path.join(regs, name), 'rb') as a, open(os.path.join(game, name), 'wb') as b:
                b.write(a.read())
    r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        os.path.join(root, 'x360', 'build_gen.ps1')], capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode == 0:
        print(out.strip().splitlines()[-1] if out.strip() else 'ok')
        sys.exit(0)
    found = []
    for f, line in ICE.findall(out):
        name = None
        with open(os.path.join(game, 'x86_recomp.%s.cpp' % f), encoding='utf-8') as src:
            for i, text in enumerate(src, 1):
                if text.startswith('X86_FUNC('):
                    name = text[len('X86_FUNC('):text.index(')')]
                if i == int(line):
                    break
        if name:
            found.append(name)
    if not found:
        print(out[-3000:])
        sys.exit(1)
    with open(skip_file, 'a') as s:
        for n in found:
            s.write(n + '\n')
    print('attempt %d: %d more functions left alone: %s' % (attempt + 1, len(found), ' '.join(found)))
sys.exit(1)
