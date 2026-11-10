"""JSRF: acrescenta X86_BACKEDGE() antes de cada salto para tras do codigo gerado, para que um laco que espera outra
thread do jogo possa ceder o bloqueio (kernel/guestlock.cpp). Mesma ideia de --back-edge-yield do Unleashed.
  python backedge.py <pasta do codigo gerado>
"""
import os
import re
import sys

COMMENT = re.compile(r'^\s*// ([0-9A-F]{8}) ')
GOTO = re.compile(r'goto loc_([0-9A-F]{8});')
total = 0
for name in sorted(os.listdir(sys.argv[1])):
    if not re.match(r'x86_recomp\.\d+\.cpp$', name):
        continue
    path = os.path.join(sys.argv[1], name)
    out = []
    cur = 0
    with open(path, encoding='utf-8', newline='') as f:
        for line in f:
            m = COMMENT.match(line)
            if m:
                cur = int(m.group(1), 16)
            elif 'goto loc_' in line:
                def fix(g):
                    global total
                    if int(g.group(1), 16) > cur:
                        return g.group(0)
                    total += 1
                    return '{ X86_BACKEDGE(); %s }' % g.group(0)
                line = GOTO.sub(fix, line)
            out.append(line)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(out)
print('%d saltos para tras com X86_BACKEDGE' % total)
