"""Keeps the guest's eight general registers in C++ locals inside each recompiled function.

The recompiler writes every register access as ctx.eax and the like: a load or a store through the context on each
use. On the Xbox 360's in-order CPU that is slow twice over: a store and a load of the same field soon after stall
(load-hit-store), and every guest memory store may alias the context, so the compiler reloads registers after it.
This pass rewrites each function so the registers live in locals (eax_ ...), which the compiler keeps in machine
registers, and writes them back to ctx only where code outside the function can see them:

  - entry: loaded from ctx
  - before a call that takes ctx and may read registers (guest functions, indirect calls, kernel and HLE calls,
    rep movs/stos, traps, privileged instructions, resume): all eight stored; after it, all eight loaded again
    (not only eax/ecx/edx/esp: helpers such as _EH_prolog change the caller's ebp on purpose)
  - before every return: stored
  x87, MMX and SSE helpers take ctx too but never touch the general registers, so they stay as they are.
Functions with an SEH try block are left alone (an exception leaves through no return). A function that is left
alone and one that is rewritten agree at every call, as ctx holds the registers there.

Functions named in regs_skip.txt (beside this script) are left alone too: the VS2010 Xbox 360 optimizer has an
internal error (C1001) on them once rewritten.

  python regs_local.py <generated dir> <output dir>
"""
import os
import re
import shutil
import sys

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG_RE = re.compile(r'\bctx\.(eax|ecx|edx|ebx|esp|ebp|esi|edi)\b')
CALL_RE = re.compile(r'\b(sub_[0-9A-Fa-f]{8}|__imp__sub_[0-9A-Fa-f]{8}|x86_icall|x86_kernel|x86_rep_movs|x86_rep_stos|'
                     r'x86_trap|x86_priv|x86_resume|x86_trace)\(ctx\b')
RETURN_RE = re.compile(r'\breturn;')

HEADER = '''
// regs_local.py: the guest's general registers in locals (see x360/tools/regs_local.py)
#define X86_LOAD_REGS() uint32_t eax_ = ctx.eax, ecx_ = ctx.ecx, edx_ = ctx.edx, ebx_ = ctx.ebx, esp_ = ctx.esp, \\
    ebp_ = ctx.ebp, esi_ = ctx.esi, edi_ = ctx.edi; const uint32_t esp0_ = esp_
#define X86_SAVE_REGS() (ctx.eax = eax_, ctx.ecx = ecx_, ctx.edx = edx_, ctx.ebx = ebx_, ctx.esp = esp_, \\
    ctx.ebp = ebp_, ctx.esi = esi_, ctx.edi = edi_, (void)0)
#define X86_RELOAD_REGS() (eax_ = ctx.eax, ecx_ = ctx.ecx, edx_ = ctx.edx, ebx_ = ctx.ebx, esp_ = ctx.esp, \\
    ebp_ = ctx.ebp, esi_ = ctx.esi, edi_ = ctx.edi, (void)0)
// functions left alone keep the push through ctx
#define X86_PUSH32_CTX(v)        \
    do {                         \
        uint32_t _v = (v);       \
        ctx.esp -= 4;            \
        X86_MEM32(ctx.esp) = _v; \
    } while (0)
#undef X86_PUSH32
#define X86_PUSH32(v)            \\
    do {                         \\
        uint32_t _v = (v);       \\
        esp_ -= 4;               \\
        X86_MEM32(esp_) = _v;    \\
    } while (0)
'''


def call_end(text, start):
    """index just past the ';' ending the call whose name starts at start, or -1"""
    i = text.index('(', start)
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                j = i + 1
                while j < n and text[j] in ' \t':
                    j += 1
                return j + 1 if j < n and text[j] == ';' else -1
        i += 1
    return -1


def rewrite_body(body):
    """body: the text between a function's braces"""
    body = REG_RE.sub(lambda m: m.group(1) + '_', body)
    out = []
    pos = 0
    while True:
        m = CALL_RE.search(body, pos)
        if not m:
            break
        end = call_end(body, m.start())
        if end < 0:
            raise ValueError('call not a statement: ' + body[m.start():m.start() + 80])
        out.append(body[pos:m.start()])
        out.append('{ X86_SAVE_REGS(); ' + body[m.start():end] + ' X86_RELOAD_REGS(); }')
        pos = end
    out.append(body[pos:])
    body = ''.join(out)
    body = RETURN_RE.sub('{ X86_SAVE_REGS(); return; }', body)
    return body


FUNC_RE = re.compile(r'^(X86_FUNC\([^)]*\)) \{\n', re.M)


SKIP = set()


def rewrite_file(text):
    out = []
    pos = 0
    changed = skipped = 0
    for m in FUNC_RE.finditer(text):
        if m.start() < pos:
            continue
        # the body ends at the first line that is a lone '}' at column 0
        end = text.index('\n}\n', m.end())
        body = text[m.end():end + 1]
        out.append(text[pos:m.end()])
        name = m.group(1)[len('X86_FUNC('):-1]
        if 'try {' in body or 'catch' in body or name in SKIP:
            out.append(body.replace('X86_PUSH32(', 'X86_PUSH32_CTX('))
            skipped += 1
        else:
            out.append('\tX86_LOAD_REGS();\n' + rewrite_body(body))
            # falling off the end of the function stores too
            out.append('\tX86_SAVE_REGS();\n')
            changed += 1
        pos = end + 1
    out.append(text[pos:])
    text = ''.join(out)
    # the macros after the includes
    inc = text.rfind('#include')
    eol = text.index('\n', inc) + 1
    return text[:eol] + HEADER + text[eol:], changed, skipped


def main():
    src, dst = sys.argv[1], sys.argv[2]
    skip = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'regs_skip.txt')
    if os.path.exists(skip):
        for line in open(skip):
            line = line.split('#')[0].strip()
            if line:
                SKIP.add(line)
    os.makedirs(dst, exist_ok=True)
    total_c = total_s = 0
    for name in sorted(os.listdir(src)):
        p = os.path.join(src, name)
        if not os.path.isfile(p):
            continue
        if re.match(r'x86_recomp\.\d+\.cpp$', name):
            text = open(p, encoding='utf-8').read()
            text, c, s = rewrite_file(text)
            total_c += c
            total_s += s
            open(os.path.join(dst, name), 'w', encoding='utf-8', newline='\n').write(text)
        else:
            shutil.copy2(p, os.path.join(dst, name))
    print('%d functions with registers in locals, %d left alone (SEH or regs_skip.txt)' % (total_c, total_s))


if __name__ == '__main__':
    main()
