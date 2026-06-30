# Contributing to Rayforce

Thanks for helping improve Rayforce. This page covers the one thing that trips
people up most often — **which branch to target** — plus the basics of getting a
change merged.

## Branch model (read this first)

Rayforce uses a two-branch flow:

- **`dev`** — the integration branch and the repository default. **All
  contributions target `dev`.** Open your PR here.
- **`master`** — release-only. The single PR it ever receives is the
  `dev -> master` release PR (titled `vX.Y.Z`), which a maintainer cuts. Direct
  feature/fix PRs against `master` are blocked by the **Version Guard** check.

`dev` is the default branch, so a fresh `git clone` and the "New pull request"
button already point the right way. If you do open a PR against `master` by
mistake, a bot automatically retargets it to `dev` and leaves a note — nothing
for you to fix.

See [RELEASE.md](RELEASE.md) for how releases are cut.

## Opening a pull request

1. Branch off the latest `dev`.
2. Make your change. Keep commits focused; we use
   [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat:`, `fix:`, `perf:`, `docs:`, `refactor:`, `test:`, `chore:`) — the
   release notes are generated from these prefixes, so they matter.
3. Make sure the build is clean and tests pass locally:

   ```sh
   make            # builds (debug: ASan + UBSan)
   make test       # runs the test suite
   ```

4. Open the PR against **`dev`**. CI runs on every PR and push.

## Local build notes

- Zero external dependencies; C17 toolchain only.
- Debug builds enable AddressSanitizer + UndefinedBehaviorSanitizer; warnings are
  errors (`-Werror -Wall -Wextra`). A warning will fail CI, so fix them locally.
- The version reported by a local build is derived from git tags (`0.0.0` on an
  untagged checkout) — never hand-edit a version literal in source.

## Tests

Behavioural tests live under `test/rfl/<group>/<name>.rfl` and are
auto-discovered. Add or update a `.rfl` test alongside any behaviour change. Run
a focused subset with:

```sh
./rayforce.test -f <substring>
```
