#!/usr/bin/env python3
"""
RMAC HAS NO OPERATOR PRECEDENCE: it evaluates left to right.

    OBJ_SHADOW + OBJ_I_MASK * OBJ_STRIDE

is not $1056 + 4*16 = $1096, it is ($1056+4)*16 = $105A0.  No warning, no error:
the assembly passes and the program writes in the wrong place.

It cost us a hardware round (M2): the curtain and the status strip were composed
outside the area the video handler copies back, so the Object Processor found in
their place what the DRAM held at reset and stopped there.  On screen: two bands
showing the BG register instead of their contents.  No part of the symptom
resembled "a bracket is missing".

This step finds the same thing at build time.  The method is not a list of
suspect patterns - that would be a list to keep up to date by hand - but a
COMPARISON: every expression is evaluated two ways, as RMAC evaluates it and as
whoever wrote it would have, substituting distinct numbers for the symbols.  If
the two results coincide the expression is harmless however it is written, and it
does not matter which operators it contains; if they differ, brackets are simply
missing.  That way there are no false alarms on things like a*b/c, which are
equal in both orders.
"""

import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TOKEN = re.compile(r"""
    \$[0-9A-Fa-f]+          # RMAC hexadecimal
  | %[01]+                  # RMAC binary
  | \d+
  | [A-Za-z_.][A-Za-z0-9_.]*
  | <<|>>|[-+*/&|^~()]
""", re.X)

BINOPS = {'<<', '>>', '+', '-', '*', '/', '&', '|', '^'}

# Addressing modes and registers: they are not constant expressions.
REG = re.compile(r'^[-+]?\(?[aAdD][0-7]\)?[+]?$|^\(sp\)[+]?$|^-\(sp\)$|^sr$|^ccr$|^usp$',
                 re.I)
ADDRMODE = re.compile(r'\([aApPsS][cCpP0-7]', re.I)

# A movem register list - d0-d7/a0-a1 - is made of the same signs as an
# expression and is not one: it has to be taken out of the way first, or the
# comparison would shout on every movem in the file.  The filter looks at the
# mnemonic too, so it does not depend on the shape alone.
REGLIST = re.compile(r'^[adAD][0-7](-[adAD][0-7])?(/[adAD][0-7](-[adAD][0-7])?)*$')


def tokenize(expr):
    out, i = [], 0
    while i < len(expr):
        if expr[i].isspace():
            i += 1
            continue
        m = TOKEN.match(expr, i)
        if not m:
            return None                     # something we cannot read
        out.append(m.group(0))
        i = m.end()
    return out


def symvalue(name, table):
    """A distinct number for every symbol.  Large primes, so that two orders of
    evaluation are unlikely to give the same result by chance."""
    if name not in table:
        table[name] = 1000003 + 2 * len(table) * 7919 + 13
    return table[name]


def to_python(tokens, table):
    out = []
    for t in tokens:
        if t.startswith('$'):
            out.append(str(int(t[1:], 16)))
        elif t.startswith('%'):
            out.append(str(int(t[1:], 2)))
        elif t[0].isdigit():
            out.append(t)
        elif t in BINOPS or t in '()~':
            out.append('//' if t == '/' else t)
        else:
            out.append(str(symvalue(t, table)))
    return ' '.join(out)


def eval_ltr(tokens, table):
    """How RMAC evaluates: left to right, the brackets count."""
    pos = [0]

    def atom():
        t = tokens[pos[0]]
        if t == '(':
            pos[0] += 1
            v = expr()
            if pos[0] >= len(tokens) or tokens[pos[0]] != ')':
                raise ValueError('parentesi')
            pos[0] += 1
            return v
        if t == '-':
            pos[0] += 1
            return -atom()
        if t == '~':
            pos[0] += 1
            return ~atom()
        pos[0] += 1
        if t.startswith('$'):
            return int(t[1:], 16)
        if t.startswith('%'):
            return int(t[1:], 2)
        if t[0].isdigit():
            return int(t)
        return symvalue(t, table)

    def expr():
        v = atom()
        while pos[0] < len(tokens) and tokens[pos[0]] in BINOPS:
            op = tokens[pos[0]]
            pos[0] += 1
            r = atom()
            if op == '+':
                v = v + r
            elif op == '-':
                v = v - r
            elif op == '*':
                v = v * r
            elif op == '/':
                v = v // r
            elif op == '<<':
                v = v << r
            elif op == '>>':
                v = v >> r
            elif op == '&':
                v = v & r
            elif op == '|':
                v = v | r
            else:
                v = v ^ r
        return v

    v = expr()
    if pos[0] != len(tokens):
        raise ValueError('avanzo')
    return v


def split_operands(s):
    parts, depth, cur = [], 0, ''
    for c in s:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += c
    parts.append(cur)
    return parts


def strip_comment(line):
    out, q = '', None
    for c in line:
        if q:
            out += c
            if c == q:
                q = None
            continue
        if c in '"\'':
            q = c
            out += c
            continue
        if c == ';':
            break
        out += c
    return out


def candidates(line):
    """A line's constant expressions: the right-hand side of an EQU, and the
    operands that are not registers or addressing modes."""
    code = strip_comment(line).rstrip()
    if not code.strip():
        return
    m = re.match(r'^\s*\S+\s+(?:EQU|=|SET)\s+(.+)$', code, re.I)
    if m:
        yield m.group(1).strip()
        return
    m = re.match(r'^\s+(\.?[A-Za-z][A-Za-z0-9_.]*?)(?:\.[bwslBWSL])?\s+(.+)$', code)
    if not m:
        return
    if m.group(1).lower() in ('movem', 'reg', 'equr'):
        return
    for part in split_operands(m.group(2)):
        p = part.strip()
        if p.startswith('#'):
            p = p[1:].strip()
        if not p or REG.match(p) or ADDRMODE.search(p) or REGLIST.match(p):
            continue
        yield p


def check(path):
    bad = []
    for n, line in enumerate(open(path, encoding='latin1'), 1):
        for expr in candidates(line):
            toks = tokenize(expr)
            if not toks or not any(t in BINOPS for t in toks):
                continue
            table = {}
            try:
                ltr = eval_ltr(toks, table)
                std = eval(to_python(toks, table), {'__builtins__': {}}, {})
            except Exception:
                continue                    # we cannot read it: not our business
            if ltr != std:
                bad.append((n, expr, std, ltr))
    return bad


def main():
    files = sorted(glob.glob(os.path.join(REPO, 'src', '*.s')) +
                   glob.glob(os.path.join(REPO, 'include', '*.inc')))
    total = 0
    for f in files:
        for n, expr, std, ltr in check(f):
            total += 1
            rel = os.path.relpath(f, REPO)
            print(f"{rel}:{n}: RMAC evaluates LEFT TO RIGHT")
            print(f"    {expr}")
            print(f"    as written it looks like ${std & 0xFFFFFFFF:X}, "
                  f"RMAC makes it ${ltr & 0xFFFFFFFF:X}")
            print(f"    -> add the brackets")
    if total:
        print(f"\ncheck_expr: {total} ambiguous expressions. "
              f"None of these raises an assembly error: they give a program "
              f"that writes in the wrong place.")
        return 1
    print(f"check_expr: {len(files)} files, no ambiguous expression")
    return 0


if __name__ == '__main__':
    sys.exit(main())
