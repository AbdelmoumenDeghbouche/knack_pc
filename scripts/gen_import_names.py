"""Add a game's imports to src/import_names.inc (NID#library -> symbol name).

runtime_lookup() binds the runtime's name tables through this list, so a function the runtime
implements is only reachable when its NID is listed here. Names come from shadPS4's aerolib
(src/core/aerolib/aerolib.inl, generated from public PS4 symbol lists).

Usage: python scripts/gen_import_names.py <game dir> <aerolib.inl> [module.prx ...]
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from link_modules import module  # noqa: E402

INC = Path(__file__).resolve().parent.parent / 'src' / 'import_names.inc'


def main():
    game, aerolib = Path(sys.argv[1]), Path(sys.argv[2])
    extra = sys.argv[3:] or ['libc.prx', 'libSceFios2.prx']
    names = dict(re.findall(r'STUB\(\s*"([^"]+)"\s*,\s*([^)\s]+)\s*\)', aerolib.read_text()))
    text = INC.read_text()
    known = set(re.findall(r'\{"([^"]+)"', text))
    added = []
    for path in [game / 'eboot.bin'] + [game / 'sce_module' / m for m in extra]:
        for s in module(path)['symbols']:
            if s['section'] or not s['identity']:
                continue
            nid, lib = s['identity'][0], s['identity'][1][0]
            if lib == 'libScePosix':
                lib = 'libkernel'  # runtime.c canonical_name()
            key = f'{nid}#{lib}'
            if key in known or nid not in names:
                continue
            known.add(key)
            added.append(f'{{"{key}","{names[nid]}"}},')
    if added:
        INC.write_text(text.rstrip('\n') + '\n/* Added by gen_import_names.py */\n' + '\n'.join(added) + '\n')
    print(f'{len(added)} names added to {INC}')


if __name__ == '__main__':
    main()
