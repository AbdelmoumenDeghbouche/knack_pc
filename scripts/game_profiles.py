"""Per-game settings, by title id in sce_sys/param.sfo (after bc160201983's MediEvil port).

modules: bundled sce_module PRX files linked into the image (link_modules.py), in order.
env: default environment for the run (run.py); a value already set wins.
"""
from pathlib import Path

PROFILES = {
    # WipEout Omega Collection. NpToolkit2 is the game's own PSN toolkit. Red-zone protection
    # always: the startup probe sees no overwrite, but 2048 races run at 0.3 FPS without it (a
    # loop at 0x40ff50 keeps its count below the stack pointer, under the GPU write faults).
    'CUSA07671': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1'}},
    'CUSA05670': {'name': 'WipEout Omega Collection',
                  'modules': ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx'),
                  'env': {'GOW3_RED_ZONE': '1'}},
    'CUSA01623': {'name': 'God of War III', 'modules': ('libc.prx', 'libSceFios2.prx')},
}
DEFAULT = {'name': None, 'modules': ('libc.prx', 'libSceFios2.prx')}


def title_id(game):
    """TITLE_ID of a game folder, or None."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys' / 'param.sfo').read_bytes()).get('TITLE_ID')
    except (OSError, ValueError, ImportError):
        return None


def profile(game):
    """The profile of a game folder (DEFAULT for unknown games)."""
    found = PROFILES.get(title_id(game) or '', DEFAULT)
    # Only modules the dump actually ships are linked.
    modules = tuple(m for m in found['modules'] if (Path(game) / 'sce_module' / m).is_file())
    return {**found, 'modules': modules}
