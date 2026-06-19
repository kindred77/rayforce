#!/usr/bin/env bash
# Generate a FEATURE-oriented changelog for a release, not a contributor ledger.
#
# Buckets conventional-commit subjects (feat/fix/perf/docs/...) since the
# previous release tag into readable sections. Author names and PR-merge noise
# are intentionally dropped — the "Full changelog" compare link at the bottom is
# the exhaustive, per-commit view for anyone who wants it.
#
# Usage: release-notes.sh <X.Y.Z> <target-sha>
# Writes the notes (Markdown) to stdout. Requires full git history + tags
# (checkout with fetch-depth: 0).
set -euo pipefail

VERSION="${1:?usage: release-notes.sh X.Y.Z <target-sha>}"
SHA="${2:?usage: release-notes.sh X.Y.Z <target-sha>}"
REPO="${GITHUB_REPOSITORY:-RayforceDB/rayforce}"

# Previous-release boundary: the highest semver tag strictly below this release.
# Falls back to the pre-v2 rollup marker, then to the repo root (no boundary).
prev="$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname | grep -vxF "v$VERSION" | head -1 || true)"
if [ -z "$prev" ] && git rev-parse -q --verify "pre-v2-rollup^{commit}" >/dev/null 2>&1; then
  prev="pre-v2-rollup"
fi
range="${prev:+$prev..}$SHA"

feats=""; fixes=""; perfs=""; docs=""; other=""
while IFS= read -r s; do
  [ -n "$s" ] || continue
  type="$(printf '%s' "$s" | sed -nE 's/^([a-zA-Z]+)(\([^)]*\))?!?:.*/\1/p')"
  desc="$(printf '%s' "$s" | sed -E 's/^[a-zA-Z]+(\([^)]*\))?!?:[[:space:]]*//')"
  case "$type" in
    feat) feats+="- ${desc}"$'\n';;
    fix)  fixes+="- ${desc}"$'\n';;
    perf) perfs+="- ${desc}"$'\n';;
    docs) docs+="- ${desc}"$'\n';;
    *)    other+="- ${s}"$'\n';;   # non-conventional / internal: keep full subject
  esac
done < <(git log --no-merges --reverse --pretty=tformat:'%s' "$range")

emit() { [ -n "$2" ] && printf '### %s\n\n%s\n' "$1" "$2"; return 0; }

emit "✨ Features"      "$feats"
emit "🐛 Bug fixes"    "$fixes"
emit "⚡ Performance"   "$perfs"
emit "📚 Documentation" "$docs"
if [ -n "$other" ]; then
  printf '<details>\n<summary>🔧 Maintenance &amp; internal</summary>\n\n%s\n</details>\n' "$other"
fi

if [ -n "$prev" ]; then
  printf '\n**Full changelog**: https://github.com/%s/compare/%s...v%s\n' "$REPO" "$prev" "$VERSION"
fi
