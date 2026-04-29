#!/usr/bin/env bash
# Rebuild website/assets/og-image.png from scripts/og/og.html.
#
# Pipeline: headless Chrome → 1200x630 PNG → ImageMagick PNG24 strip.
# The PNG24 re-encode flattens any alpha channel and drops metadata so the
# committed file matches the 8-bit non-alpha encoding contract from c1d1402e.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/scripts/og/og.html"
OUT="$ROOT/website/assets/og-image.png"

[ -f "$SRC" ] || { echo "missing: $SRC" >&2; exit 1; }

google-chrome \
  --headless=new \
  --hide-scrollbars \
  --window-size=1200,800 \
  --default-background-color=00000000 \
  --screenshot="$OUT" \
  "file://$SRC"

# Crop to exactly 1200x630 (Chrome's --window-size pads vertically)
# and re-encode to 8-bit non-alpha to match c1d1402e's encoding contract.
convert "$OUT" -crop 1200x630+0+0 +repage \
  -strip -define png:color-type=2 -define png:bit-depth=8 PNG24:"$OUT"

# Optional pass: pngquant if available (lossy palette quantization, big shrink).
if command -v pngquant >/dev/null 2>&1; then
  pngquant --quality 85-95 --speed 1 --force --output "$OUT" "$OUT"
fi

ls -la "$OUT"
identify "$OUT"
