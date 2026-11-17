#!/usr/bin/env python3
"""Copia un .c del SDK quitando las funciones PS* (asm de Gekko), que clang no compila.
Las C_* gemelas se quedan. Uso: strip_ps.py in.c out.c"""
import re, sys
src = open(sys.argv[1], encoding='latin-1').read()
out, i, n = [], 0, len(src)
hdr = re.compile(r'^(?:[A-Za-z_][\w\s\*]*?\s)?\**_*PS\w+\s*\([^;{]*\)\s*\{', re.M)
while i < n:
    m = hdr.search(src, i)
    if not m:
        out.append(src[i:]); break
    out.append(src[i:m.start()])
    j, d = m.end(), 1
    while j < n and d:
        c = src[j]; d += (c == '{') - (c == '}'); j += 1
    i = j
open(sys.argv[2], 'w', encoding='latin-1').write(''.join(out))
