"""Finds the generated functions the Xbox 360 compiler (VS2010) crashes on with C1001 when optimizing,
adds them to [xbe] noopt in config/game.toml, regenerates and retries until every file compiles.
Run: python -I x360/tools/fix_ice.py   (from BRE-X360, with XEDK set)"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PC = os.path.join(ROOT, 'pc-recomp')
GEN = os.path.join(PC, 'generated', 'game')
OBJ = os.path.join(ROOT, 'x360', 'build', 'gen')
CFG = os.path.join(PC, 'config', 'game.toml')
XEDK = os.environ['XEDK']
CL = os.path.join(XEDK, 'bin', 'win32', 'cl.exe')
env = dict(os.environ, INCLUDE=os.path.join(XEDK, 'include', 'xbox'), PATH=os.path.join(XEDK, 'bin', 'win32') + ';' + os.environ['PATH'])
FLAGS = ['/nologo', '/c', '/O2', '/DNDEBUG', '/EHs', '/bigobj', '/GF', '/Gy', '/D_XBOX', '/TP', '/wd4102', '/wd4244', '/wd4101',
         '/wd4146', '/wd4800', '/I' + os.path.join(ROOT, 'x360', 'runtime'), '/I' + GEN]


def compile_one(name):
    r = subprocess.run([CL] + FLAGS + ['/Fo' + os.path.join(OBJ, name.replace('.cpp', '.obj')), os.path.join(GEN, name)],
                       env=env, capture_output=True, text=True, errors='replace')
    return r.returncode, r.stdout + r.stderr


def function_at(name, line):
    lines = open(os.path.join(GEN, name), encoding='utf-8', errors='replace').read().split('\n')
    for i in range(line - 1, -1, -1):
        m = re.match(r'X86_FUNC\(__imp__sub_([0-9A-F]{8})\)', lines[i])
        if m: return int(m.group(1), 16)
    return None


def add_noopt(va):
    text = open(CFG, encoding='utf-8').read()
    m = re.search(r'^noopt = \[(.*)\]$', text, re.M)
    if m:
        items = [x.strip() for x in m.group(1).split(',') if x.strip()]
        if '0x%X' % va in items: sys.exit('sub_%08X is already unoptimized and still crashes the compiler' % va)
        items.append('0x%X' % va)
        text = text[:m.start()] + 'noopt = [' + ', '.join(items) + ']' + text[m.end():]
    else:
        text = re.sub(r'^(code_sections = .*)$', lambda k: k.group(1) + '\n# functions the Xbox 360 compiler builds unoptimized (it crashes on them)\nnoopt = [0x%X]' % va,
                      text, count=1, flags=re.M)
    open(CFG, 'w', encoding='utf-8', newline='\n').write(text)


def regenerate():
    subprocess.run([os.path.join(ROOT, 'x360', 'tools', 'build', 'xbe_recomp.exe'), 'config/game.toml', '--xdk', 'sdk/xdk'],
                   cwd=PC, capture_output=True, check=True)


os.makedirs(OBJ, exist_ok=True)
files = sorted(f for f in os.listdir(GEN) if f.endswith('.cpp'))
if len(sys.argv) > 1: files = sys.argv[1:]
for name in files:
    while True:
        rc, out = compile_one(name)
        if rc == 0:
            print('ok', name, flush=True)
            break
        m = re.search(re.escape(name) + r'\((\d+)\) : fatal error C1001', out, re.I)
        if not m:
            print('FAILED', name)
            print('\n'.join(l for l in out.split('\n') if 'error' in l)[:3000])
            sys.exit(1)
        va = function_at(name, int(m.group(1)))
        print('  C1001 in sub_%08X (%s), building it unoptimized' % (va, name), flush=True)
        add_noopt(va)
        regenerate()
