"""Emit synthetic packed-math fixtures through the production translator."""
import argparse
from pathlib import Path
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import translate as T


class Image:
    base = end = size = 0
    md = None

    def insn_end(self, addr, mnemonic):
        return None

    def rd8(self, addr):
        return None

    def rd32(self, addr):
        return None

    def is_exec(self, addr):
        return False

    def relocated_pointers(self):
        return set()


def emit_fixture(name, instructions):
    image = Image()
    addr = 0x0d020000
    listing = '\n'.join('%08x  %s' % (addr + 4 * i, ins)
                        for i, ins in enumerate(instructions + ['RET']))
    fn = T.Function(addr, name, 4 * (len(instructions) + 1), T.parse_listing_text(listing))
    fn.measure(image)
    tr = T.Translator(image, {addr}, SimpleNamespace(eager_flags=False))
    tr.prepare(fn, strict=True)
    body = ['void %s(X86 *c) {' % name]
    for i in range(len(instructions)):
        body.extend(tr.emit(fn, i, set()))
    body.append('}')
    return '\n'.join(body)


def fixtures():
    for op in ('ADDPS', 'SUBPS', 'MULPS', 'DIVPS', 'ADDPD', 'SUBPD', 'MULPD', 'DIVPD'):
        for mode, src in (('reg', 'XMM1'), ('mem', 'xmmword ptr [ESI + 0x10]'), ('alias', 'XMM0')):
            yield 'packed_%s_%s' % (op.lower(), mode), ['%s XMM0,%s' % (op, src)]
    yield 'packed_rounding', ['MULPS XMM0,XMM1', 'SUBPS XMM0,XMM2']
    yield 'packed_matrix', [
        'MOVAPS XMM0,xmmword ptr [ESI]',
        'MOVAPS XMM1,xmmword ptr [EDI]',
        'MOVAPS XMM2,XMM0', 'SHUFPS XMM2,XMM0,0x0', 'MULPS XMM2,XMM1',
        'MOVAPS XMM1,xmmword ptr [EDI + 0x10]',
        'MOVAPS XMM3,XMM0', 'SHUFPS XMM3,XMM0,0x55', 'MULPS XMM3,XMM1', 'ADDPS XMM3,XMM2',
        'MOVAPS XMM1,xmmword ptr [EDI + 0x20]',
        'MOVAPS XMM2,XMM0', 'SHUFPS XMM2,XMM0,0xaa', 'MULPS XMM2,XMM1', 'ADDPS XMM2,XMM3',
        'MOVAPS XMM1,xmmword ptr [EDI + 0x30]',
        'MOVAPS XMM3,XMM0', 'SHUFPS XMM3,XMM0,0xff', 'MULPS XMM3,XMM1', 'ADDPS XMM3,XMM2',
        'MOVUPS xmmword ptr [EBX],XMM3',
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    source, header = ['#include "x86.h"'], ['#pragma once', '#include "x86.h"']
    for name, instructions in fixtures():
        source.append(emit_fixture(name, instructions))
        header.append('void %s(X86 *c);' % name)
    args.out.with_suffix('.c').write_text('\n\n'.join(source) + '\n')
    args.out.with_suffix('.h').write_text('\n'.join(header) + '\n')


if __name__ == '__main__':
    main()
