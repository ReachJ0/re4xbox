#!/usr/bin/env python3
"""Genera xbox/alt.inc: lineas `-alternatename:<indefinido>=<definido>` para el enlazador.

GCC 2.95 no ponia el tipo en el nombre de las variables globales (ni los argumentos de algunas
declaraciones `extern` contradictorias); clang/MSVC si. Un simbolo indefinido se empareja con uno
definido de igual nombre base (y mismo ambito). Los alias `asm("etiqueta")` del codigo original
(`void ScreenReSizeI(...) asm("ScreenReSize__FUsUs")`) se resuelven con la etiqueta.
Uso: gen_alternatenames.py <re4> <objs...> < lista_de_indefinidos"""
import re, subprocess, sys, os
re4 = sys.argv[1]; objs = sys.argv[2:]
def key(sym):
    s = sym.lstrip('\x01')
    if s.startswith('?'):
        m = re.match(r'\?([^@]+)@((?:[^@]+@)*)@', s)
        if m: return (m.group(2), m.group(1))
        m = re.match(r'\?([^@]+)@@', s)
        if m: return ('', m.group(1))
    return ('', s[1:] if s.startswith('_') else s)
defined = {}
out = subprocess.run(['llvm-nm', '-g', '--defined-only'] + objs, capture_output=True, text=True).stdout
for ln in out.splitlines():
    p = ln.split()
    if len(p) == 3 and p[1] in 'TtDdBbRrCc': defined.setdefault(key(p[2]), p[2])
# alias asm("label") -> real name
alias = {}
rx = re.compile(r'\b(\w+)\s*(?:\((?:[^;{()]|\([^()]*\))*\))?\s*asm\("(\w+)"\)\s*;')
for root in ('src', 'include'):
    for d, _, fs in os.walk(os.path.join(re4, root)):
        for f in fs:
            if f.endswith(('.cpp', '.h', '.c')):
                for m in rx.finditer(open(os.path.join(d, f), encoding='latin-1').read()):
                    lab = m.group(2); real = lab.split('__')[0] if '__' in lab[1:] else lab
                    alias[m.group(1)] = real
res, miss = [], []
for sym in (l.strip() for l in sys.stdin if l.strip()):
    k = key(sym); target = defined.get(k)
    if not target and k[1] in alias: target = defined.get(('', alias[k[1]])) or next((v for kk, v in defined.items() if kk[1] == alias[k[1]]), None)
    if not target and k[0] == '': target = next((v for kk, v in defined.items() if kk[1] == k[1]), None)
    if target and target != sym: res.append('-alternatename:%s=%s' % (sym, target))
    else: miss.append(sym)
print('\n'.join(res))
sys.stderr.write('resueltos %d, sin definicion %d\n' % (len(res), len(miss)))
open('/dev/stderr', 'w').write('\n'.join('  falta ' + m for m in miss) + '\n') if '-v' in os.environ.get('V', '') else None
