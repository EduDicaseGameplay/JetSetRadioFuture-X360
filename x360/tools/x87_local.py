"""Keeps the top of the guest's x87 register stack in C++ locals inside each recompiled function (after regs_local.py).

The recompiler writes every x87 access through ctx: x87_push/x87_pop move ctx.top and X87_ST(i) indexes ctx.st with
it. On the Xbox 360's in-order CPU each of those is a store and a load of the same context field soon after
(load-hit-store), several per FPU instruction. Within a function the stack depth is known statically, so this pass
gives the values the function pushed itself locals (x87s0 = the lowest of them) and rewrites:

  - x87_push(ctx, v)  -> x87sN = v                (N = the depth so far)
  - x87_pop(ctx)      -> nothing                  (a local is dropped)
  - X87_ST(i)         -> the local, or X87_ST(i - depth) when it is older than the function's own values
  - x87_status(ctx)   -> x87_status_at(ctx, depth) (TOP as the guest sees it)
At a call, a return, and an instruction that reads the whole stack (fxam, fnsave/frstor) the locals are stored to
ctx.st below ctx.top first ("spill"), which is then the guest's full stack again; a value a callee returns stays in
ctx and is read from there. With no locals every access is the original one, so a depth this pass cannot know
(unreachable code) is taken as zero and stays correct. A function whose branches reach one label with different
depths, or with an SEH try block, or named in regs_skip.txt, is left as it is.

  python x87_local.py <dir>     (rewrites x86_recomp.*.cpp in place)
"""
import os
import re
import sys

CALL_RE = re.compile(r'\b(sub_[0-9A-Fa-f]{8}|__imp__sub_[0-9A-Fa-f]{8}|x86_icall|x86_kernel|x86_rep_movs|x86_rep_stos|'
                     r'x86_trap|x86_priv|x86_resume|x86_trace)\(ctx\b')
TOKEN_RE = re.compile(r'X87_ST\((\d+)\)|x87_push\(ctx, |x87_pop\(ctx\)|x87_status\(ctx\)|\breturn;|' + CALL_RE.pattern)
WHOLE_RE = re.compile(r'x87_examine\(ctx\)|ctx\.top\b|ctx\.st\b')
GOTO_RE = re.compile(r'\bgoto (loc_[0-9A-F]+);')
LABEL_RE = re.compile(r'^(loc_[0-9A-F]+):')
INSN_RE = re.compile(r'^\t// [0-9A-F]{8} (\w+)')
FUNC_RE = re.compile(r'^(X86_FUNC\([^)]*\)) \{\n', re.M)
NO_FALL = ('jmp', 'ret', 'retn', 'retf', 'iret', 'iretd')

HEADER = '''
// x87_local.py: the top of the x87 stack in locals (see x360/tools/x87_local.py)
inline uint16_t x87_status_at(X86Context& ctx, unsigned depth) {
    return uint16_t((ctx.fsw & ~0x3800) | (((ctx.top - depth) & 7) << 11));
}
'''


class Conflict(Exception):
    pass


def spill(d):
    if d == 0:
        return ''
    parts = ['uint32_t x87t = (ctx.top - %du) & 7;' % d]
    for k in range(d):
        parts.append('ctx.st[(x87t + %du) & 7] = x87s%d;' % (d - 1 - k, k))
    parts.append('ctx.top = x87t;')
    return '{ ' + ' '.join(parts) + ' } '


def paren_end(text, i):
    """index of the ')' closing the '(' just before i"""
    depth = 1
    while i < len(text):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError('unbalanced: ' + text[:80])


def rewrite_text(text, d, gotos):
    """text with the stack accesses at depth d rewritten; returns (text, depth after). gotos gets (label, depth)."""
    out = []
    pos = 0
    if WHOLE_RE.search(text):
        out.append(spill(d))
        d = 0
    while True:
        m = TOKEN_RE.search(text, pos)
        g = GOTO_RE.search(text, pos)
        if g and (not m or g.start() < m.start()):
            gotos.append((g.group(1), d))
            out.append(text[pos:g.end()])
            pos = g.end()
            continue
        if not m:
            break
        out.append(text[pos:m.start()])
        tok = m.group(0)
        if tok.startswith('X87_ST('):
            i = int(m.group(1))
            out.append('x87s%d' % (d - 1 - i) if i < d else 'X87_ST(%d)' % (i - d))
            pos = m.end()
        elif tok == 'x87_pop(ctx)':
            if d > 0:
                out.append('(void)0')
                d -= 1
            else:
                out.append(tok)
            pos = m.end()
        elif tok == 'x87_status(ctx)':
            out.append('x87_status_at(ctx, %du)' % d)
            pos = m.end()
        elif tok.startswith('x87_push('):
            end = paren_end(text, m.end())
            arg, _ = rewrite_text(text[m.end():end], d, gotos)
            if d < 8:
                out.append('(x87s%d = (%s))' % (d, arg))
                d += 1
            else:
                raise Conflict('stack overflow')
            pos = end + 1
        elif tok == 'return;':
            # the path ends here: what follows in the text (a switch's next case) is reached with the same depth
            out.append(spill(d) + tok)
            pos = m.end()
        else:
            # a call: the callee sees the whole stack in ctx and may leave a result there
            out.append(spill(d) + tok)
            d = 0
            pos = m.end()
    out.append(text[pos:])
    return ''.join(out), d


def parse(body):
    """items: ('label', name, line) | ('insn', mnemonic, [lines]) | ('line', None, line)"""
    items = []
    for line in body.split('\n'):
        lm = LABEL_RE.match(line)
        im = INSN_RE.match(line)
        if lm:
            items.append(['label', lm.group(1), line])
        elif im:
            items.append(['insn', im.group(1), [line]])
        elif items and items[-1][0] == 'insn':
            items[-1][2].append(line)
        else:
            items.append(['line', None, line])
    return items


def walk(items, label_d):
    """one pass over the function; label_d: depth at each label from the branches seen. Returns (changed, text)."""
    changed = False
    out = []
    cur = 0  # depth on the fallthrough path; None: not reached by falling through
    for kind, name, payload in items:
        if kind == 'label':
            known = label_d.get(name)
            if cur is not None and known is not None and cur != known:
                raise Conflict(name)
            if known is None and cur is not None:
                label_d[name] = cur
                changed = True
            cur = label_d.get(name)
            out.append(payload)
            continue
        if kind == 'line':
            out.append(payload)
            continue
        # not reached yet (a label only a later branch reaches): depth zero is correct code, and its branches are
        # not taken as facts until a pass reaches it
        reach = cur is not None
        d = cur if reach else 0
        switch_d = None
        lines = []
        for line in payload:
            s = line.lstrip()
            if s.startswith('switch ('):
                switch_d = d
            elif switch_d is not None and (s.startswith('case ') or s.startswith('default:')):
                d = switch_d
            gotos = []
            text, d = rewrite_text(line, d, gotos)
            lines.append(text)
            for target, gd in (gotos if reach else []):
                known = label_d.get(target)
                if known is None:
                    label_d[target] = gd
                    changed = True
                elif known != gd:
                    raise Conflict(target)
        out.extend(lines)
        cur = None if name in NO_FALL or not reach else d
    if cur:
        out.append('\t' + spill(cur))
    return changed, '\n'.join(out)


def rewrite_body(body):
    items = parse(body)
    label_d = {}
    for _ in range(64):
        changed, text = walk(items, label_d)
        if not changed:
            return text
    raise Conflict('no fixed point')


SKIP = set()


def rewrite_file(text):
    out = []
    pos = 0
    changed = skipped = 0
    for m in FUNC_RE.finditer(text):
        if m.start() < pos:
            continue
        end = text.index('\n}\n', m.end())
        body = text[m.end():end + 1]
        out.append(text[pos:m.end()])
        name = m.group(1)[len('X86_FUNC('):-1]
        uses = 'X87_ST(' in body or 'x87_push(' in body or 'x87_pop(' in body
        new = None
        if uses and 'try {' not in body and 'catch' not in body and name not in SKIP and 'X86_LOAD_REGS();' in body:
            try:
                new = rewrite_body(body)
            except Conflict:
                new = None
        if new is not None:
            out.append('\tdouble x87s0 = 0, x87s1 = 0, x87s2 = 0, x87s3 = 0, x87s4 = 0, x87s5 = 0, x87s6 = 0, x87s7 = 0;\n')
            out.append(new)
            changed += 1
        else:
            out.append(body)
            skipped += uses
        pos = end + 1
    out.append(text[pos:])
    text = ''.join(out)
    inc = text.rfind('#include')
    eol = text.index('\n', inc) + 1
    return text[:eol] + HEADER + text[eol:], changed, skipped


def main():
    d = sys.argv[1]
    skip = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'regs_skip.txt')
    if os.path.exists(skip):
        for line in open(skip):
            line = line.split('#')[0].strip()
            if line:
                SKIP.add(line)
    total_c = total_s = 0
    for name in sorted(os.listdir(d)):
        if not re.match(r'x86_recomp\.\d+\.cpp$', name):
            continue
        p = os.path.join(d, name)
        text = open(p, encoding='utf-8').read()
        text, c, s = rewrite_file(text)
        total_c += c
        total_s += s
        open(p, 'w', encoding='utf-8', newline='\n').write(text)
    print('%d functions with the x87 stack in locals, %d with x87 code left alone' % (total_c, total_s))


if __name__ == '__main__':
    main()
