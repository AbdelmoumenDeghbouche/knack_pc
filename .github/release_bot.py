#!/usr/bin/env python3
"""Weekly release: when pull requests were merged into main since the last release, bump the
launcher version, tag it, build the Windows zip and the Linux AppImage on that tag and publish.
Run by .github/workflows/weekly-release.yml (Sundays, or by hand).

Needs GH_REPO, GH_TOKEN and TAG_PREFIX (the part of a release tag before the version)."""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = os.environ['GH_REPO']
PREFIX = os.environ['TAG_PREFIX']
WORKFLOWS = ('windows-zip.yml', 'linux-appimage.yml')
LAUNCHER = Path('launcher/gow3_launcher_win.py')
VERSION_TEST = Path('tests/test_launcher_controls.py')


def run(*args):
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode:
        sys.exit(f'{" ".join(args)}: {out.stderr.strip()}')
    return out.stdout


def numbers(version):
    return tuple(int(part) for part in version.split('.'))


def bump(version):
    parts = list(numbers(version))
    parts[-1] += 1
    return '.'.join(map(str, parts))


def replace_once(path, pattern, new):
    text, n = re.subn(pattern, new, path.read_text(encoding='utf-8'), flags=re.M)
    if n != 1:
        sys.exit(f'{path}: expected one match for {pattern}, found {n}')
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def section(body, title):
    """The '## title' section of release notes, up to the next '## ' heading ('' if missing)."""
    match = re.search(rf'^## {re.escape(title)}\n.*?(?=^## |\Z)', body, flags=re.M | re.S)
    return match.group(0).rstrip() + '\n\n' if match else ''


def main():
    run('git', 'fetch', '-q', '--tags', 'origin', 'main')
    latest = json.loads(run('gh', 'release', 'view', '-R', REPO, '--json', 'tagName,body'))
    merged = re.findall(r'^Merge pull request #(\d+) ',
                        run('git', 'log', f'{latest["tagName"]}..origin/main', '--merges', '--format=%s'), flags=re.M)
    if not merged:
        print(f'Nothing merged since {latest["tagName"]}: no release this week.')
        return
    # Above every tag so far, also a pre-release left by a failed build.
    tags = [t[len(PREFIX):] for t in run('git', 'tag', '-l', PREFIX + '*').split()]
    version = bump(max((t for t in tags if re.fullmatch(r'\d+(\.\d+)+', t)), key=numbers))
    tag = PREFIX + version

    run('git', 'checkout', '-q', '--detach', 'origin/main')
    replace_once(LAUNCHER, r"^VERSION = '[^']*'", f"VERSION = '{version}'")
    replace_once(VERSION_TEST, r"launcher\.VERSION, '[^']*'\)", f"launcher.VERSION, '{version}')")
    replace_once(VERSION_TEST, rf"version_tuple\('{re.escape(PREFIX)}[0-9.]+'\), launcher\.version_tuple\(launcher\.VERSION\)\)",
                 f"version_tuple('{PREFIX}{bump(version)}'), launcher.version_tuple(launcher.VERSION))")
    run('git', '-c', 'user.name=github-actions[bot]', '-c', 'user.email=41898282+github-actions[bot]@users.noreply.github.com',
        'commit', '-q', '-am', f'Launcher VERSION {version}')
    run('git', 'tag', tag)
    run('git', 'push', '-q', 'origin', tag)

    lines = []
    for number in sorted(set(merged), key=int):
        pr = json.loads(run('gh', 'pr', 'view', number, '-R', REPO, '--json', 'title,author'))
        lines.append(f'- {pr["title"]} (#{number}, by @{pr["author"]["login"]})')
    notes = (section(latest['body'], 'Project status') +
             '## What\'s new\n\nCommunity update: these pull requests were reviewed and approved by trusted '
             'contributors and merged by the merge bot (see CONTRIBUTING.md).\n\n' + '\n'.join(lines) + '\n\n' +
             section(latest['body'], 'All features') + section(latest['body'], 'Download'))
    Path('notes.md').write_text(notes, encoding='utf-8')
    run('gh', 'release', 'create', tag, '-R', REPO, '--prerelease', '--verify-tag',
        '--title', f'v{version} - Community update', '--notes-file', 'notes.md')

    started = time.time()
    for workflow in WORKFLOWS:
        run('gh', 'workflow', 'run', workflow, '-R', REPO, '--ref', tag, '-f', f'tag={tag}')
    time.sleep(30)
    results = {}
    while len(results) < len(WORKFLOWS):
        if time.time() - started > 3 * 3600:
            sys.exit(f'{tag}: builds did not finish in 3 hours; the release stays a pre-release')
        for workflow in WORKFLOWS:
            runs = json.loads(run('gh', 'run', 'list', '-R', REPO, '--workflow', workflow, '--branch', tag,
                                  '--limit', '1', '--json', 'status,conclusion'))
            if runs and runs[0]['status'] == 'completed':
                results[workflow] = runs[0]['conclusion']
        time.sleep(60)
    if any(result != 'success' for result in results.values()):
        sys.exit(f'{tag}: {results}; the release stays a pre-release')
    run('gh', 'release', 'edit', tag, '-R', REPO, '--prerelease=false', '--latest')
    print(f'Released {tag}')


if __name__ == '__main__':
    main()
