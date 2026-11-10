"""JSRF: faz o link com /LTCG, tirando do /GL os arquivos em que o compilador tem erro interno na geracao de codigo.
Grava a lista em x360/tools/ltcg_skip.txt, que build_gen.ps1 le. Uso: python ltcg_loop.py [rodadas]"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
x360 = os.path.dirname(here)
skip = os.path.join(here, 'ltcg_skip.txt')
ICE = re.compile(r'x86_recomp\.(\d+)\.cpp\(\d+\) : fatal error C1001')


def run(script):
    return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(x360, script)],
                          capture_output=True, text=True)


for attempt in range(int(sys.argv[1]) if len(sys.argv) > 1 else 8):
    r = run('build_gen.ps1')
    if r.returncode:
        print('build_gen falhou', r.stdout[-600:])
        sys.exit(1)
    r = run('build_xex.ps1')
    out = r.stdout + r.stderr
    print('rodada', attempt, 'link', r.returncode, flush=True)
    if r.returncode == 0:
        sys.exit(0)
    m = ICE.search(out)
    if not m:
        print(out[-800:])
        sys.exit(1)
    with open(skip, 'a') as f:
        f.write(m.group(1) + '\n')
    print('sem /GL: x86_recomp.%s.cpp' % m.group(1), flush=True)
sys.exit(1)
