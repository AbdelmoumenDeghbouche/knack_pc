#!/usr/bin/env python3
"""Merge bot: merges every open pull request that passes the rules in CONTRIBUTING.md, so the
project moves without the owner. Run every hour by .github/workflows/merge-bot.yml.

  python3 .github/merge_bot.py            merge what passes, print why the rest wait
  python3 .github/merge_bot.py --dry-run  only print
Needs GH_REPO (owner/name) and GH_TOKEN; TRUST_REPOS (space-separated) are the repositories whose
merged pull requests count towards trust (default: GH_REPO)."""
import json
import os
import subprocess
import sys
from datetime import datetime, timedelta, timezone

REPO = os.environ['GH_REPO']
OWNER = REPO.split('/')[0]
TRUST_REPOS = os.environ.get('TRUST_REPOS', REPO).split()
TRUSTED_AFTER = 3                      # merged pull requests that make an author a trusted reviewer
WAIT = timedelta(hours=48)             # time anyone has to look at a pull request before it merges
OWNER_ONLY = ('.github/',)             # the rules, the bots and the release workflows
REQUIRED = ('Windows zip', 'Linux AppImage')  # workflows that must build and smoke-test the change
GOOD = {'pass', 'skipping'}


def gh(*args, ok=False):
    run = subprocess.run(['gh', *args], capture_output=True, text=True)
    if run.returncode and not ok:
        sys.exit(f'gh {" ".join(args)}: {run.stderr.strip()}')
    return run.stdout


def trusted_reviewers():
    merged = {}
    for repo in TRUST_REPOS:
        for pr in json.loads(gh('pr', 'list', '-R', repo, '--state', 'merged', '--limit', '2000', '--json', 'author')):
            login = pr['author']['login']
            merged[login] = merged.get(login, 0) + 1
    return {OWNER} | {login for login, n in merged.items() if n >= TRUSTED_AFTER and not pr_bot(login)}


def pr_bot(login):
    return login.endswith('[bot]') or login.startswith('app/')


def blockers(pr, trusted, now):
    """Why this pull request cannot merge yet; empty when it can. Also returns the approvers."""
    number, author, head = pr['number'], pr['author']['login'], pr['headRefOid']
    why = []
    if pr['isDraft']:
        why.append('it is a draft')
    if pr['baseRefName'] != 'main':
        why.append('it does not target main')
    if any(label['name'] == 'do-not-merge' for label in pr['labels']):
        why.append('it has the do-not-merge label')
    created = datetime.fromisoformat(pr['createdAt'].replace('Z', '+00:00'))
    if now - created < WAIT:
        why.append(f'it is open less than {WAIT.days * 24} hours')
    files = gh('api', '--paginate', f'repos/{REPO}/pulls/{number}/files', '--jq', '.[].filename').split()
    if any(path.startswith(OWNER_ONLY) for path in files):
        why.append('it changes .github/ (the owner merges those)')
    if pr['mergeable'] != 'MERGEABLE':
        why.append('it has conflicts with main (or GitHub is still checking)')
    checks = json.loads(gh('pr', 'checks', str(number), '-R', REPO, '--json', 'workflow,name,bucket', ok=True) or '[]')
    for workflow in REQUIRED:
        if not any(c['workflow'] == workflow for c in checks):
            why.append(f'the {workflow} check has not run')
    for c in checks:
        if c['bucket'] not in GOOD:
            why.append(f'check "{c["workflow"]} / {c["name"]}" is {c["bucket"]}')
    latest = {}  # each reviewer's last approval or change request
    for review in json.loads(gh('api', '--paginate', f'repos/{REPO}/pulls/{number}/reviews')):
        if review['user'] and review['state'] in ('APPROVED', 'CHANGES_REQUESTED', 'DISMISSED'):
            latest[review['user']['login']] = review
    approvers = sorted(login for login, r in latest.items()
                       if login in trusted and login != author and r['state'] == 'APPROVED' and r['commit_id'] == head)
    need = 1 if author in trusted else 2
    if len(approvers) < need:
        why.append(f'it needs {need} approval(s) of its latest commit from trusted reviewers other than the author, has {len(approvers)}')
    for login, r in latest.items():
        if login in trusted and r['state'] == 'CHANGES_REQUESTED':
            why.append(f'{login} requested changes')
    return why, approvers


def main():
    dry = '--dry-run' in sys.argv
    trusted = trusted_reviewers()
    print('Trusted reviewers:', ', '.join(sorted(trusted)))
    now = datetime.now(timezone.utc)
    prs = json.loads(gh('pr', 'list', '-R', REPO, '--state', 'open', '--limit', '200', '--json',
                        'number,title,author,isDraft,baseRefName,labels,createdAt,mergeable,headRefOid'))
    for pr in prs:
        why, approvers = blockers(pr, trusted, now)
        if why:
            print(f'#{pr["number"]} waits: ' + '; '.join(why))
            continue
        print(f'#{pr["number"]} merges (approved by {", ".join(approvers)})')
        if dry:
            continue
        gh('pr', 'merge', str(pr['number']), '-R', REPO, '--merge', '--match-head-commit', pr['headRefOid'])
        gh('pr', 'comment', str(pr['number']), '-R', REPO, '--body',
           f'Merged by the merge bot: approved by {", ".join("@" + a for a in approvers)}, '
           f'Windows and Linux builds pass. It goes out in the next weekly release. Thanks!')


if __name__ == '__main__':
    main()
