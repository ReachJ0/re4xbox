#!/usr/bin/env python3
"""Reports which global symbols the REL modules define more than once (and which the DOL also defines).

The GameCube loads every module into its own address range, so two modules may both define `MakeCol(float x4)` or
the same template instance. In one Xbox executable such names collide unless the build keeps them module-local
(partial link per module, or weak/COMDAT linkage for the inline and template ones). This tool lists them so the
build step can decide, from config/G4BE08/modules/*/symbols.txt and config/G4BE08/symbols.txt.

usage: tools/xbox/rel_symbols.py [--all]       (default: summary and the names that are not templates / inline-like)
"""
import collections, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BASE = os.path.join(ROOT, 'config', 'G4BE08', 'modules')
LINE = re.compile(r'^(\S+) = \.(\w+):0x[0-9A-Fa-f]+;.*type:(\w+).*scope:(\w+)')
ENTRY = {'_prolog', '_epilog', '_unresolved'}


def main():
    defs = collections.defaultdict(set)
    for mod in sorted(os.listdir(BASE)):
        with open(os.path.join(BASE, mod, 'symbols.txt'), errors='ignore') as f:
            for line in f:
                m = LINE.match(line)
                if m and m.group(4) == 'global':
                    defs[m.group(1)].add(mod)
    dol = set()
    with open(os.path.join(ROOT, 'config', 'G4BE08', 'symbols.txt'), errors='ignore') as f:
        for line in f:
            m = re.match(r'(\S+) = ', line)
            if m:
                dol.add(m.group(1))
    dup = {k: v for k, v in defs.items() if len(v) > 1}
    templ = {k for k in dup if '__t' in k or k.startswith('_._') or '__dl__' in k or '__nw__' in k}
    both = sorted(k for k in defs if k in dol)
    print('modules: %d, global names defined by modules: %d' % (len(os.listdir(BASE)), len(defs)))
    print('  defined by more than one module: %d (entry points %d, template/ctor/dtor-like %d, others %d)' %
          (len(dup), len(ENTRY & set(dup)), len(templ - ENTRY), len(set(dup) - templ - ENTRY)))
    print('  also defined by the DOL: %d' % len(both))
    print('The entry points need the per-module renames (-D_prolog=<mod>_prolog ...); the rest needs per-module')
    print('isolation or weak linkage in the build.')
    others = sorted(set(dup) - templ - ENTRY)
    for k in (sorted(dup) if '--all' in sys.argv else others):
        print('  %-60s %3d modules  %s' % (k, len(dup[k]), ' '.join(sorted(dup[k])[:4])))


if __name__ == '__main__':
    main()
