"""Repete regs_build ate compilar. Funcao que derruba o cl mesmo sem registradores locais vai para [xbe] noopt, como no BRE."""
import os, re, shutil, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
cfg = os.path.join(here, 'pc-recomp', 'config', 'game.toml')
gen = os.path.join(here, 'pc-recomp', 'generated')
def regen():
    if os.path.isdir(gen): shutil.rmtree(gen)
    subprocess.check_call([os.path.join(here,'x360','tools','build','xbe_recomp.exe'), 'config/game.toml', '--xdk', 'sdk/xdk'], cwd=os.path.join(here, 'pc-recomp'), stdout=subprocess.DEVNULL)
    shutil.copytree(os.path.join(gen, 'game'), os.path.join(gen, 'game_orig'))
if not os.path.isdir(os.path.join(gen,'game_orig')): regen()
for round in range(12):
    r = subprocess.run([sys.executable, '-I', 'x360/tools/regs_build.py', '--x87'], cwd=here, capture_output=True, text=True, errors='replace')
    out = r.stdout + r.stderr
    open(os.path.join(here, 'regs_build.log'), 'a', encoding='utf8').write(out)
    print('rodada', round, 'exit', r.returncode, out.strip().splitlines()[-1][:200], flush=True)
    if r.returncode == 0: sys.exit(0)
    names = set(re.findall(r'__imp__sub_([0-9A-F]{8})', ' '.join(out.strip().splitlines()[-1:])))
    if not names: print(out[-2500:]); sys.exit(1)
    t = open(cfg, encoding='utf8').read()
    cur = re.search(r'noopt = \[(.*?)\]', t).group(1)
    new = cur + ''.join(', 0x%X' % int(n, 16) for n in sorted(names))
    open(cfg, 'w', encoding='utf8').write(t.replace('noopt = [%s]' % cur, 'noopt = [%s]' % new))
    skip = os.path.join(here, 'x360', 'tools', 'regs_skip.txt')
    keep = []; seen = set()
    for l in open(skip):
        if l not in seen and not any(n in l for n in names): keep.append(l); seen.add(l)
    open(skip, 'w').writelines(keep)
    regen()
sys.exit(1)
