"""JSRF: instrucoes SSE do jogo em VMX128, com os registradores xmm em variaveis locais (depois de x87_local, antes de call_local).

A biblioteca de matrizes do jogo (TransformVertices, MultiMatrix, Rotate...) e SSE. O gerado passa cada lane por ctx.xmm, na memoria,
entre registradores de inteiro e de ponto flutuante: quatro stalls de load-hit-store por instrucao, na CPU do 360. VMX128 faz as
mesmas contas em registrador, em precisao simples, como o SSE.

Lane i do xmm = elemento i do vetor: a carga de 16 bytes em big endian poe a palavra mais baixa no elemento 0, e x86_xs_swap inverte os
bytes de cada palavra.

O que nao tem forma aqui continua como foi gerado: os xmm da funcao vao para ctx.xmm, a instrucao roda, e voltam. Funcoes sem
X86_LOAD_REGS (regs_skip, SEH) ficam como estao.

Uso: sse_local.py <pasta do codigo gerado>
"""
import glob
import os
import re
import sys

FUNC_RE = re.compile(r'^X86_FUNC\(__imp__sub_[0-9A-F]{8}\) \{', re.M)
X = r'ctx\.xmm\[(\d)\]'
EA = r'uint32_t ea = \(uint32_t\)\(([^;]*?)\);'


def shuf(stmt):
    m = re.match(r'\{ X86Xmm a = ' + X + r', b = ' + X + r'; ctx\.xmm\[(\d)\]\.u\[0\] = a\.u\[(\d)\]; ctx\.xmm\[\d\]\.u\[1\] = a\.u\[(\d)\]; '
                 r'ctx\.xmm\[\d\]\.u\[2\] = b\.u\[(\d)\]; ctx\.xmm\[\d\]\.u\[3\] = b\.u\[(\d)\]; \}$', stmt)
    if not m:
        return None
    a, b, d, s0, s1, s2, s3 = m.groups()
    s0, s1, s2, s3 = int(s0), int(s1), int(s2), int(s3)
    imm = s0 << 6 | s1 << 4 | s2 << 2 | s3
    used = {a, b, d}
    if a == b:
        return '{ xv%s = __vpermwi(xv%s, %d); }' % (d, a, imm), used, {d}
    return '{ __vector4 l = __vpermwi(xv%s, %d), h = __vpermwi(xv%s, %d); xv%s = __vrlimi(l, h, 3, 0); }' % (a, imm, b, imm, d), used, {d}


PATTERNS = []


def pat(regex, fmt, uses, writes):
    PATTERNS.append((re.compile(regex), fmt, uses, writes))


# carga e gravacao
pat(r'\{ ' + EA + r' ctx\.xmm\[(\d)\]\.u\[0\] = X86_MEM32\(ea\); ctx\.xmm\[\d\]\.u\[1\] = ctx\.xmm\[\d\]\.u\[2\] = ctx\.xmm\[\d\]\.u\[3\] = 0; \}$',
    '{{ uint32_t ea = (uint32_t)({0}); xv{1} = x86_xs_lds(base, ea); }}', [1], [1])
pat(r'\{ ' + EA + r' X86_MEM32\(ea\) = ctx\.xmm\[(\d)\]\.u\[0\]; \}$',
    '{{ uint32_t ea = (uint32_t)({0}); x86_xs_sts(base, ea, xv{1}); }}', [1], [])
pat(r'\{ ' + EA + r' ctx\.xmm\[(\d)\] = x86_ldxmm\(base, ea\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); xv{1} = x86_xs_ldu(base, ea); }}', [1], [1])
pat(r'\{ ' + EA + r' x86_stxmm\(base, ea, ctx\.xmm\[(\d)\]\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); x86_xs_stu(base, ea, xv{1}); }}', [1], [])
pat(r'\{ ' + EA + r' X86_MEM64\(ea\) = x86_getq\(ctx\.xmm\[(\d)\], 0\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); x86_xs_stq(base, ea, xv{1}, 0); }}', [1], [])
pat(r'\{ ' + EA + r' X86_MEM64\(ea\) = x86_getq\(ctx\.xmm\[(\d)\], 1\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); x86_xs_stq(base, ea, xv{1}, 1); }}', [1], [])
pat(r'\{ ' + EA + r' x86_setq\(ctx\.xmm\[(\d)\], 0, X86_MEM64\(ea\)\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); xv{1} = __vrlimi(xv{1}, x86_xs_ldu(base, ea), 12, 0); }}', [1], [1])
pat(r'\{ ' + EA + r' x86_setq\(ctx\.xmm\[(\d)\], 1, X86_MEM64\(ea\)\); \}$',
    '{{ uint32_t ea = (uint32_t)({0}); xv{1} = __vrlimi(xv{1}, x86_xs_ldu(base, ea), 3, 2); }}', [1], [1])
# movimento
pat(r'\{ ctx\.xmm\[(\d)\]\.f\[0\] = ctx\.xmm\[(\d)\]\.f\[0\]; \}$', '{{ xv{0} = __vrlimi(xv{0}, xv{1}, 8, 0); }}', [0, 1], [0])
pat(r'\{ ctx\.xmm\[(\d)\] = ctx\.xmm\[(\d)\]; \}$', '{{ xv{0} = xv{1}; }}', [0, 1], [0])
# escalar, entre registradores e com memoria
for sym, fn in (('*', '__vmulfp'), ('+', '__vaddfp'), ('-', '__vsubfp')):
    s = re.escape(sym)
    pat(r'\{ float a = ctx\.xmm\[(\d)\]\.f\[0\], b = ctx\.xmm\[(\d)\]\.f\[0\]; \(void\)a; ctx\.xmm\[\d\]\.f\[0\] = a ' + s + r' b; \}$',
        '{{ xv{0} = __vrlimi(xv{0}, ' + fn + '(xv{0}, xv{1}), 8, 0); }}', [0, 1], [0])
    pat(r'\{ ' + EA + r' float a = ctx\.xmm\[(\d)\]\.f\[0\], b = X86_MEMF32\(ea\); \(void\)a; ctx\.xmm\[\d\]\.f\[0\] = a ' + s + r' b; \}$',
        '{{ uint32_t ea = (uint32_t)({0}); xv{1} = __vrlimi(xv{1}, ' + fn + '(xv{1}, x86_xs_lds(base, ea)), 8, 0); }}', [1], [1])
    # empacotado
    pat(r'\{ X86Xmm v = ctx\.xmm\[(\d)\]; for \(int i = 0; i < 4; i\+\+\) \{ float a = ctx\.xmm\[(\d)\]\.f\[i\], b = v\.f\[i\]; \(void\)a; ctx\.xmm\[\d\]\.f\[i\] = a ' + s + r' b; \} \}$',
        '{{ xv{1} = ' + fn + '(xv{1}, xv{0}); }}', [0, 1], [1])
    pat(r'\{ ' + EA + r' X86Xmm v = x86_ldxmm\(base, ea\); for \(int i = 0; i < 4; i\+\+\) \{ float a = ctx\.xmm\[(\d)\]\.f\[i\], b = v\.f\[i\]; \(void\)a; ctx\.xmm\[\d\]\.f\[i\] = a ' + s + r' b; \} \}$',
        '{{ uint32_t ea = (uint32_t)({0}); xv{1} = ' + fn + '(xv{1}, x86_xs_ldu(base, ea)); }}', [1], [1])
# logica
for sym, fn in (('^', '__vxor'), ('&', '__vand')):
    s = re.escape(sym)
    pat(r'\{ X86Xmm v = ctx\.xmm\[(\d)\]; for \(int i = 0; i < 4; i\+\+\) \{ uint32_t a = ctx\.xmm\[(\d)\]\.u\[i\], b = v\.u\[i\]; ctx\.xmm\[\d\]\.u\[i\] = a ' + s + r' b; \} \}$',
        '{{ xv{1} = ' + fn + '(xv{1}, xv{0}); }}', [0, 1], [1])
    pat(r'\{ ' + EA + r' X86Xmm v = x86_ldxmm\(base, ea\); for \(int i = 0; i < 4; i\+\+\) \{ uint32_t a = ctx\.xmm\[(\d)\]\.u\[i\], b = v\.u\[i\]; ctx\.xmm\[\d\]\.u\[i\] = a ' + s + r' b; \} \}$',
        '{{ uint32_t ea = (uint32_t)({0}); xv{1} = ' + fn + '(xv{1}, x86_xs_ldu(base, ea)); }}', [1], [1])
# estimativas: mesma classe de precisao do SSE
pat(r'\{ float a = ctx\.xmm\[(\d)\]\.f\[0\], b = ctx\.xmm\[(\d)\]\.f\[0\]; \(void\)a; ctx\.xmm\[\d\]\.f\[0\] = 1\.0f / std::sqrt\(b\); \}$',
    '{{ xv{0} = __vrlimi(xv{0}, __vrsqrtefp(xv{1}), 8, 0); }}', [0, 1], [0])
pat(r'\{ float a = ctx\.xmm\[(\d)\]\.f\[0\], b = ctx\.xmm\[(\d)\]\.f\[0\]; \(void\)a; ctx\.xmm\[\d\]\.f\[0\] = 1\.0f / b; \}$',
    '{{ xv{0} = __vrlimi(xv{0}, __vrefp(xv{1}), 8, 0); }}', [0, 1], [0])
pat(r'\{ ' + EA + r' float a = ctx\.xmm\[(\d)\]\.f\[0\], b = X86_MEMF32\(ea\); \(void\)a; ctx\.xmm\[\d\]\.f\[0\] = 1\.0f / b; \}$',
    '{{ uint32_t ea = (uint32_t)({0}); xv{1} = __vrlimi(xv{1}, __vrefp(x86_xs_lds(base, ea)), 8, 0); }}', [1], [1])
# intercalar
pat(r'\{ X86Xmm a = ctx\.xmm\[(\d)\], b = ctx\.xmm\[(\d)\]; ctx\.xmm\[\d\]\.u\[0\] = a\.u\[0\]; ctx\.xmm\[\d\]\.u\[1\] = b\.u\[0\]; ctx\.xmm\[\d\]\.u\[2\] = a\.u\[1\]; ctx\.xmm\[\d\]\.u\[3\] = b\.u\[1\]; \}$',
    '{{ xv{0} = __vmrghw(xv{0}, xv{1}); }}', [0, 1], [0])
# comparacao
pat(r'\{ float a = ctx\.xmm\[(\d)\]\.f\[0\], b = ctx\.xmm\[(\d)\]\.f\[0\]; bool u = a != a \|\| b != b; zf = u \|\| a == b; pf = u; cf = u \|\| a < b; of = 0; sf = 0; \}$',
    '{{ x86_xs_comi(xv{0}, xv{1}, zf, pf, cf); of = 0; sf = 0; }}', [0, 1], [])
pat(r'\{ ' + EA + r' float a = ctx\.xmm\[(\d)\]\.f\[0\], b = X86_MEMF32\(ea\); bool u = a != a \|\| b != b; zf = u \|\| a == b; pf = u; cf = u \|\| a < b; of = 0; sf = 0; \}$',
    '{{ uint32_t ea = (uint32_t)({0}); x86_xs_comi(xv{1}, x86_xs_lds(base, ea), zf, pf, cf); of = 0; sf = 0; }}', [1], [])


def convert_line(stmt):
    r = shuf(stmt)
    if r:
        return r
    for regex, fmt, uses, writes in PATTERNS:
        m = regex.match(stmt)
        if m:
            g = m.groups()
            # com EA o primeiro grupo e o endereco: os indices de uses e writes contam dentro de g
            return fmt.format(*g), {g[i] for i in uses}, {g[i] for i in writes}
    return None


def convert(body):
    lines = body.split('\n')
    conv = []
    used, written = set(), set()
    fallback_used = False
    for line in lines:
        s = line.strip()
        if 'ctx.xmm[' not in s or s.startswith('//'):
            conv.append(line)
            continue
        r = convert_line(s)
        if r:
            conv.append('\t' + r[0])
            used |= r[1]
            written |= r[2]
        else:
            regs = set(re.findall(r'ctx\.xmm\[(\d)\]', s))
            used |= regs
            written |= regs
            fallback_used = True
            conv.append('\tXS_OUT_MARK ' + s + ' XS_IN_MARK')
    return conv, used, written, fallback_used


def out_of(regs):
    return ''.join('x86_xs_save(ctx, %s, xv%s), ' % (r, r) for r in sorted(regs))


def in_of(regs):
    return ''.join(', xv%s = x86_xs_load(ctx, %s)' % (r, r) for r in sorted(regs))


def transform(body, stats):
    if 'X86_LOAD_REGS()' not in body or 'ctx.xmm[' not in body:
        return body
    conv, used, written, fb = convert(body)
    text = '\n'.join(conv)
    stats['funcs'] += 1
    stats['fallback'] += text.count('XS_OUT_MARK')
    decl = ''.join('\t__vector4 xv%s = x86_xs_load(ctx, %s);\n' % (r, r) for r in sorted(used))
    text = text.replace('\tX86_LOAD_REGS();\n', '\tX86_LOAD_REGS();\n' + decl, 1)
    # antes de chamadas e na saida, os alterados vao para ctx; depois de chamadas, todos voltam
    text = text.replace('X86_SAVE_REGS()', '(%sX86_SAVE_REGS())' % out_of(written))
    text = text.replace('X86_RELOAD_REGS()', '(X86_RELOAD_REGS()%s)' % in_of(used))
    text = text.replace('XS_OUT_MARK ', '{ %s(void)0; ' % out_of(used)).replace(' XS_IN_MARK', ' (void)0%s; }' % in_of(used))
    return text


def main():
    gen = sys.argv[1]
    stats = {'funcs': 0, 'fallback': 0}
    for f in sorted(glob.glob(os.path.join(gen, 'x86_recomp.*.cpp'))):
        text = open(f, encoding='utf-8').read()
        starts = [m.start() for m in FUNC_RE.finditer(text)]
        if not starts:
            continue
        out = [text[:starts[0]]]
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else len(text)
            out.append(transform(text[s:e], stats))
        with open(f, 'w', encoding='utf-8', newline='') as o:
            o.write(''.join(out))
    print('sse_local: %d funcoes, %d instrucoes ainda pela memoria' % (stats['funcs'], stats['fallback']))


if __name__ == '__main__':
    main()
