# Contributing

Everyone can contribute, and the project does not wait for the owner. Pull requests are reviewed by
trusted contributors and merged by a bot when they pass the rules below. Every Sunday the merged
changes go out as a new release.

## How a pull request gets merged

The merge bot (`.github/merge_bot.py`) checks every open pull request each hour. It merges one only
when **all** of these are true:

1. **It is ready.** Not a draft, it targets `main`, and it has no conflicts with `main`.
2. **It builds.** The **Windows zip** and **Linux AppImage** checks pass: both packages build and open
   on clean machines. They run on every push to the pull request.
3. **People had time to look.** It has been open for at least **48 hours**.
4. **Trusted reviewers approved it.** A trusted reviewer is anyone with **3 or more merged pull
   requests** in this project or in [wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc).
   - Author who is a trusted reviewer: **1 approval** from another trusted reviewer.
   - Any other author: **2 approvals** from trusted reviewers.
   - The approvals must be on the **latest commit**. A new push needs new approvals.
5. **Nobody objects.** No trusted reviewer has an open "Request changes", and it has no
   `do-not-merge` label.
6. **It does not change `.github/`.** The rules, the bots and the release workflows are merged by the
   owner only.

The bot's log (Actions > Merge bot) lists every pull request that waits and why.

## What reviewers check

Approve only what you understood and believe is right:

- **Why:** the description says what it fixes or adds, with the issue number.
- **Tested in game:** the author or the reviewer ran it on the screens it changes, and says so (GPU,
  frame rate, game mode). The log is not proof: look at the game.
- **No side effects:** default settings do not change for people who did not ask for it. A new
  feature or a risky fix is behind a setting or off by default.
- **Clean:** only the files the change needs; no game files, logs, captures or personal paths.
- **Tests:** Python tests pass (`python -B -m unittest discover -s tests`), and new logic has a test
  when that is possible.

When something is wrong, use "Request changes" and say what. That blocks the merge until you approve.

## Releases

Every Sunday at 12:00 UTC, when something was merged that week, `.github/release_bot.py` bumps the
version, builds the Windows zip and the Linux AppImage on the new tag and publishes the release
with the list of merged pull requests. If a build fails, the release stays a pre-release.

## The owner

The owner can still merge, close or revert anything, and can add the `do-not-merge` label to stop a
pull request. Bad merges are fixed by a revert pull request, which follows the same rules.

## AI tools

AI-assisted pull requests are welcome. Say so in the description, and test them like any other.
