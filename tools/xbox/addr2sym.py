#!/usr/bin/env python3
"""Translates code addresses of the Xbox build into function names using the link map (xbox/main.map).

usage: addr2sym.py xbox/main.map 0x1475d5 0x2a3f10          one or more addresses
       addr2sym.py xbox/main.map --stack < dump.txt         every 8-digit hex word of the text that falls in
                                                            the code is listed (a stack dump = a call chain)
The addresses are virtual (the .xbe is loaded at 0x10000, the map's "Rva+Base" column). Mangled names are
demangled with llvm-undname when it is installed.
"""
import bisect, re, shutil, subprocess, sys

def load(path):
    syms = []
    row = re.compile(r'^\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8,16})\s+(\S+)')
    for line in open(path, errors='replace'):
        m = row.match(line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1), m.group(3)))
    syms.sort()
    return syms

def demangle(names):
    exe = shutil.which('llvm-undname') or shutil.which('undname')
    if not exe:
        return {n: n for n in names}
    out = {}
    for n in names:
        try:
            r = subprocess.run([exe, n], capture_output=True, text=True, timeout=5).stdout.strip().splitlines()
            out[n] = r[-1] if r else n
        except Exception:
            out[n] = n
    return out

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    syms = load(sys.argv[1])
    keys = [s[0] for s in syms]
    lo, hi = keys[0], keys[-1] + 0x10000
    if sys.argv[2] == '--stack':
        addrs = [int(w, 16) for w in re.findall(r'\b[0-9a-fA-F]{8}\b', sys.stdin.read())]
    else:
        addrs = [int(a, 16) for a in sys.argv[2:]]
    hits = []
    for a in addrs:
        if lo <= a < hi:
            i = bisect.bisect_right(keys, a) - 1
            hits.append((a, syms[i]))
    dm = demangle({h[1][1] for h in hits})
    for a, (base, name, obj) in hits:
        print('%08x  %s+0x%x  [%s]' % (a, dm.get(name, name), a - base, obj))
    if not hits:
        print('no address falls inside the code')
    return 0

if __name__ == '__main__':
    sys.exit(main())
