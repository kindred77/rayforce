#!/usr/bin/env bash
#
# Populate the WORKING parse/eval fuzz corpus from the behavioural test
# suite.  Every line of every test/rfl/**/*.rfl is a real Rayfall
# expression; stripping the assertion tail ("-- expected" / "!- errsub")
# leaves a clean source string.  One expression per corpus file lets
# libFuzzer merge/minimize cleanly.
#
# Output goes to fuzz/corpus/parse (gitignored, grown across runs) — NOT
# fuzz/seeds/parse, which holds a small hand-curated committed bootstrap
# set.  Run this locally and in CI before a fuzzing session; it is
# deterministic and fast, so the derived 50k inputs never need to live in
# version control.
#
# Usage: scripts/fuzz-seed-parse.sh   (run from the repo root)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
rfl_dir="$root/test/rfl"
out_dir="$root/fuzz/corpus/parse"

mkdir -p "$out_dir"

# awk: strip the assertion tail, trim, drop blanks/comments, dedupe, and
# write each unique expression to its own seed file with no trailing NL.
find "$rfl_dir" -name '*.rfl' -exec cat {} + | awk -v out="$out_dir" '
{
    line = $0
    p = index(line, "--"); if (p) line = substr(line, 1, p - 1)
    p = index(line, "!-"); if (p) line = substr(line, 1, p - 1)
    gsub(/^[ \t]+|[ \t]+$/, "", line)
    if (line == "" || line ~ /^#/) next
    if (seen[line]++) next
    n++
    f = sprintf("%s/seed-%05d", out, n)
    printf "%s", line > f
    close(f)
}
END { print n }
' | { read -r n; echo "wrote $n unique parse inputs to $out_dir"; }
