# Release process

Rayforce uses a tag-driven release flow. **The git tag is the single source of
truth for the version** — no version literal is ever hand-edited in source.

## Branch model

- **`dev`** — integration upstream. Feature/fix branches merge here. CI runs on
  every push and PR.
- **`master`** — protected, stable releases only. The only way in is a merged
  release PR.

## Cutting a release

1. Make sure `dev` is green and ready.
2. Open a pull request **`dev` → `master`** with the title set to the new
   version: **`vX.Y.Z`** (e.g. `v2.1.3`).
   - The **Version Guard** check validates the title: it must be a clean
     `vX.Y.Z`, must not already be tagged, and must be strictly greater than the
     latest release.
3. Once CI and Version Guard are green, **merge** the PR (or enable
   auto-merge so it merges itself when checks pass).
4. On merge, the **Release** workflow automatically:
   - creates the tag `vX.Y.Z` at the merge commit,
   - builds the optimized binary for Linux and macOS with the version injected
     at compile time (`make dist RAY_VERSION=X.Y.Z`),
   - packages `rayforce-X.Y.Z-<os>-<arch>.tar.gz` + a `.sha256` checksum,
   - publishes a GitHub Release with auto-generated notes and the artifacts.

That's the whole ritual. **Never edit the version in source to make a release** —
the tag is authoritative.

### Manual / re-run path

The Release workflow also has a `workflow_dispatch` trigger. Run it from the
Actions tab (or `gh workflow run release.yml -f version=X.Y.Z`) to re-drive a
release that half-finished, or to seed the very first release before the
automatic `pull_request: closed` path can fire. It is idempotent: if a draft
`vX.Y.Z` already exists it is reused and built from its own target commit; a
manual dispatch with no prior draft tags the current `master` HEAD.

> **Why the build checks out a commit, not the tag:** a *draft* GitHub release
> does **not** create its git tag — the tag ref only appears when the release is
> published (`--draft=false`). So `build` checks out the release's target
> **commit**; the `publish` job is what creates the tag `vX.Y.Z`. Checking out
> `ref: vX.Y.Z` during build would fail (the tag does not exist yet).

## How the version reaches the binary

The Makefile resolves the version (CI override `RAY_VERSION=` > latest git tag >
`0.0.0` dev default) and injects it at compile time via `-D`, exactly like it
already does for the git commit and build date. `ray_version_string()`, the REPL
banner, and `.sys.build` all report that value. The literals in
`include/rayforce.h` are only `#ifndef`-guarded `0.0.0` fallbacks for untagged
builds and for downstream code that includes the header without our `-D` flags.

A plain local `make` on an untagged commit reports `0.0.0`; with tags present it
reports the latest via `git describe`. To build a specific version locally:

```sh
make dist RAY_VERSION=2.1.3
```

## One-time repository setup (GitHub)

- **Branch protection / ruleset on `master`**: require a pull request before
  merging; require these two status checks to pass, and disallow direct pushes:
  - **`ci-success`** — a single aggregator job in `ci.yml` that goes green only
    when every CI matrix leg (ubuntu/macOS × debug/release) passed. Require this
    one instead of the four matrix names so renaming the matrix can't silently
    drop the gate.
  - **`check-version`** — the Version Guard job that validates the release
    version in the PR title.
- No release secret or bypass token is needed — the default `GITHUB_TOKEN`
  (`contents: write`) creates the tag and the release. The release workflow never
  pushes a commit to `master`, only a tag ref.

## Platform support

Linux and macOS binaries are published today. Windows is not build-ready yet
(IOCP backend is a stub, `main.c`/`heap.c` have unguarded POSIX calls, and the
Makefile has no Windows toolchain path); once ported, add a `windows-latest` row
to the `build` matrix in `.github/workflows/release.yml`.
