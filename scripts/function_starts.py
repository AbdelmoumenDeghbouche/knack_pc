#!/usr/bin/env python3
"""bbport-windows: the start addresses of the game's functions, for red-zone protection.

Windows writes exception records right below the stack pointer, over the 128-byte red zone that
the game's leaf functions keep data in. The loader protects them by rerouting faultable memory
accesses (windows/redzone.cpp), which needs function boundaries: the binary search tables of the
.eh_frame_hdr sections (PT_GNU_EH_FRAME) of the eboot and of the modules linked into the image.

Writes <out>/functions.bin: b'BBFUNC01', u64 count, count u64 image offsets (sorted, unique).

Usage: python scripts/function_starts.py <game dir> --out <dir with boot-linked.bin> [--modules ...]

From Phroster's bloodborne_pc_windows (windows/function_starts.py).
"""
import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from prepare import parse_self  # noqa: E402
from link_modules import DEFAULT_MODULES  # noqa: E402

PT_LOAD, PT_GNU_EH_FRAME = 1, 0x6474e550


def read_encoded(data, pos, encoding, hdr_address):
    """A DWARF EH pointer of the encodings found in .eh_frame_hdr; (value, next position)."""
    form = encoding & 0x0f
    if form in (0x03, 0x0b):  # udata4, sdata4
        value = struct.unpack_from('<i' if form == 0x0b else '<I', data, pos)[0]
        pos += 4
    elif form in (0x04, 0x0c):  # udata8, sdata8
        value = struct.unpack_from('<q' if form == 0x0c else '<Q', data, pos)[0]
        pos += 8
    else:
        raise ValueError(f'unsupported .eh_frame_hdr encoding {encoding:#x}')
    application = encoding & 0x70
    if application == 0x10:  # pcrel
        value += hdr_address + pos - (4 if form in (0x03, 0x0b) else 8)
    elif application == 0x30:  # datarel: relative to .eh_frame_hdr
        value += hdr_address
    elif application != 0:
        raise ValueError(f'unsupported .eh_frame_hdr application {encoding:#x}')
    return value, pos


def function_starts(elf, ph, base):
    """Image offsets of the functions in one module's .eh_frame_hdr table."""
    hdr = next((p for p in ph if p['type'] == PT_GNU_EH_FRAME), None)
    if hdr is None:
        return []
    data = elf[hdr['offset']:hdr['offset'] + hdr['filesz']]
    version, frame_encoding, count_encoding, table_encoding = data[:4]
    if version != 1 or table_encoding == 0xff:
        raise ValueError('no .eh_frame_hdr search table')
    address = hdr['vaddr']
    pos = 4
    _, pos = read_encoded(data, pos, frame_encoding, address)
    count, pos = read_encoded(data, pos, count_encoding, address)
    executable = [(p['vaddr'], p['vaddr'] + p['memsz']) for p in ph if p['type'] == PT_LOAD and p['flags'] & 1]
    starts = []
    for _ in range(count):
        start, pos = read_encoded(data, pos, table_encoding, address)
        _, pos = read_encoded(data, pos, table_encoding, address)
        if any(lo <= start < hi for lo, hi in executable):
            starts.append(base + start)
    return starts


def module_bases(boot_linked):
    """Image bases of the linked modules, from the BBPROBE5 header (link_modules.py)."""
    raw = boot_linked.read_bytes()
    if raw[:8] != b'G3PROBE5':
        raise ValueError(f'{boot_linked}: expected G3PROBE5 (run link_modules.py first)')
    pos = 8 + 6 * 8 + 8 + 4 * 8  # size..capabilities, procparam, eboot TLS
    count = struct.unpack_from('<Q', raw, pos)[0]
    pos += 8
    return [struct.unpack_from('<Q', raw, pos + i * 56)[0] for i in range(count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))  # as link_modules.py
    args = parser.parse_args()
    elf, _, ph, _, _ = parse_self((args.game / 'eboot.bin').read_bytes())
    starts = function_starts(elf, ph, 0)
    bases = module_bases(args.out / 'boot-linked.bin')
    if len(bases) != len(args.modules):
        raise ValueError(f'{len(bases)} linked modules, expected {len(args.modules)}')
    for name, base in zip(args.modules, bases):
        elf, _, ph, _, _ = parse_self((args.game / 'sce_module' / name).read_bytes())
        starts += function_starts(elf, ph, base)
    starts = sorted(set(starts))
    with (args.out / 'functions.bin').open('wb') as f:
        f.write(b'BBFUNC01' + struct.pack('<Q', len(starts)))
        f.write(struct.pack(f'<{len(starts)}Q', *starts))
    print(f'Functions: {len(starts)} starts for red-zone protection')


if __name__ == '__main__':
    main()
