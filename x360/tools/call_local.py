"""JSRF: deixa as chamadas do codigo gerado mais baratas (depois de regs_local, x87_local e backedge).

Na CPU do Xbox 360, ler de volta um valor recem-gravado na memoria custa dezenas de ciclos (load-hit-store), e o codigo
gerado gravava os 8 registradores do x86 em ctx antes e depois de cada chamada. O perfil da thread do jogo e plano, entao
isso pesa no jogo inteiro. Duas mudancas, sem alterar o que o codigo faz:

  1. X86_SAVE_REGS() so grava os registradores que a funcao escreve: os outros seguem iguais ao que esta em ctx, porque
     a copia local so muda por escrita ou por X86_RELOAD_REGS, que le de ctx.
  2. Chamada direta vai para __imp__sub_X, e nao pelo nome fraco sub_X, quando o runtime nao substitui a funcao: o nome
     fraco e resolvido no link e impede o compilador de expandir funcoes pequenas no lugar da chamada.

Uso: call_local.py <pasta do codigo gerado> <game.toml> <pasta do runtime>
Grava hooks.txt ao lado do codigo, com as funcoes substituidas. rebuild.sh confere essa lista: se um gancho novo aparecer
num build so-runtime, o codigo antigo nao o chamaria.
"""
import glob
import os
import re
import sys

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG_ALT = '|'.join(REGS)
ASSIGN_RE = re.compile(r'\b(' + REG_ALT + r')_\s*(?:=(?!=)|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|\+\+|--)')
PREINC_RE = re.compile(r'(?:\+\+|--)\s*(' + REG_ALT + r')_\b')
ADDR_RE = re.compile(r'&\s*(' + REG_ALT + r')_\b')
# um registrador como argumento de macro ou funcao, fora de cast: pode ser escrita, conta como escrita
ARG_RE = re.compile(r'\b(?!uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|double|float|if|while|return)'
                    r'([A-Za-z_][A-Za-z0-9_]*)\s*\(([^()]*\b(?:' + REG_ALT + r')_\b[^()]*)\)')
REG_IN_RE = re.compile(r'\b(' + REG_ALT + r')_\b')
FUNC_RE = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\) \{', re.M)
CALL_RE = re.compile(r'(?<![A-Za-z0-9_])sub_([0-9A-F]{8})\(ctx, base\)')
# leem o registrador, nao escrevem
READ_ONLY_CALLS = {'X86_MEM8', 'X86_MEM16', 'X86_MEM32', 'X86_MEM64', 'X86_MEMF32', 'X86_MEMF64', 'X86_ADDR', 'X86_PTR',
                   'X86_PUSH32', 'X86_PARITY', 'x86_icall', 'X86_PUSH32_CTX'}


def hooked(toml_path, runtime):
    out = set()
    for line in open(toml_path, encoding='utf-8'):
        m = re.match(r'\s*[A-Za-z_][A-Za-z0-9_]*\s*=\s*0x([0-9A-Fa-f]+)', line)
        if m:
            out.add('%08X' % int(m.group(1), 16))
    for root, _, files in os.walk(runtime):
        for name in files:
            if not name.endswith(('.cpp', '.h')):
                continue
            text = open(os.path.join(root, name), encoding='utf-8', errors='replace').read()
            out.update(re.findall(r'\bsub_([0-9A-F]{8})\b', text))
            out.update(re.findall(r'JSRF_LOG_RET\(([0-9A-F]{8})', text))
    return out


def written(body):
    w = set(ASSIGN_RE.findall(body)) | set(PREINC_RE.findall(body)) | set(ADDR_RE.findall(body))
    for m in ARG_RE.finditer(body):
        if m.group(1) not in READ_ONLY_CALLS:
            w.update(REG_IN_RE.findall(m.group(2)))
    if 'X86_PUSH32(' in body:
        w.add('esp')
    return w


def save_of(w):
    parts = ['ctx.%s = %s_' % (r, r) for r in REGS if r in w]
    return '(' + ', '.join(parts + ['(void)0']) + ')'


def main():
    if sys.argv[1] == '--check':
        gen, toml_path, runtime = sys.argv[2:5]
        old = set(open(os.path.join(gen, 'hooks.txt')).read().split())
        new = hooked(toml_path, runtime) - old
        if new:
            print('GANCHO NOVO sem regerar o codigo: ' + ' '.join(sorted(new)))
            sys.exit(1)
        return
    gen, toml_path, runtime = sys.argv[1:4]
    hooks = hooked(toml_path, runtime)
    files = sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp')))
    defined = set()
    for f in files:
        defined.update(FUNC_RE.findall(open(f, encoding='utf-8').read()))
    decl = ''.join('extern "C" void __imp__sub_%s(X86Context& ctx, uint8_t* base);\n' % a for a in sorted(defined))
    with open(os.path.join(gen, 'x86_imp_decls.h'), 'w', encoding='utf-8') as h:
        h.write('#pragma once\n' + decl)
    saved = total = direct = 0
    for f in files:
        text = open(f, encoding='utf-8').read()
        starts = [m.start() for m in FUNC_RE.finditer(text)]
        if not starts:
            continue
        out = [text[:starts[0]]]
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else len(text)
            body = text[s:e]
            if 'X86_LOAD_REGS()' in body:
                w = written(body.replace('X86_LOAD_REGS()', '').replace('X86_RELOAD_REGS()', ''))
                n = body.count('X86_SAVE_REGS()')
                total += n * 8
                saved += n * (8 - len(w))
                body = body.replace('X86_SAVE_REGS()', save_of(w))

            def direct_call(m):
                nonlocal direct
                if m.group(1) in defined and m.group(1) not in hooks:
                    direct += 1
                    return '__imp__sub_%s(ctx, base)' % m.group(1)
                return m.group(0)
            out.append(CALL_RE.sub(direct_call, body))
        text = ''.join(out).replace('#include "x86_recomp_shared.h"\n', '#include "x86_recomp_shared.h"\n#include "x86_imp_decls.h"\n', 1)
        with open(f, 'w', encoding='utf-8', newline='') as o:
            o.write(text)
    with open(os.path.join(gen, 'hooks.txt'), 'w') as o:
        o.write('\n'.join(sorted(hooks)) + '\n')
    print('call_local: %d de %d gravacoes de registrador retiradas, %d chamadas diretas, %d ganchos' % (saved, total, direct, len(hooks)))


if __name__ == '__main__':
    main()
