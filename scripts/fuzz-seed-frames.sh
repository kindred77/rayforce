#!/usr/bin/env bash
#
# Populate the WORKING binary-frame corpus for the fuzz_de (and later
# fuzz_journal) targets by capturing real wire frames.
#
# The journal log is a concatenation of 16-byte-header IPC frames written
# verbatim — the exact bytes the live IPC decoder sees — so a journal
# produced by sending a spread of statements over IPC is a faithful frame
# corpus for both the decoder and journal replay.  The journal hook only
# records IPC traffic (not local file eval), so we spawn a journaling
# server and drive it from a client over the socket.
#
# Output: fuzz/corpus/de (gitignored).  A tiny curated set stays committed
# under fuzz/seeds/de.  Requires a built ./rayforce.
#
# Usage: scripts/fuzz-seed-frames.sh   (run from the repo root)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
bin="$root/rayforce"
out_dir="$root/fuzz/corpus/de"
port="${RF_FRAMESEED_PORT:-19970}"
base="${TMPDIR:-/tmp}/rf_frameseed_$$"
trap 'pkill -KILL -f "[r]ayforce -l ${base} -p ${port}" 2>/dev/null || true; rm -f "${base}"*' EXIT

[ -x "$bin" ] || { echo "build ./rayforce first (make debug)"; exit 1; }
mkdir -p "$out_dir"

pkill -KILL -f "[r]ayforce -l ${base} -p ${port}" 2>/dev/null || true
rm -f "${base}"*

RAYFORCE_CORES=0 "$bin" -l "$base" -p "$port" </dev/null >/dev/null 2>&1 &

# Wait for a completed IPC handshake, not a bare connect (matches the
# readiness probe in test/rfl/system/log_journal.rfl).
ready=0
for _ in $(seq 50); do
    if bash -c "exec 3<>/dev/tcp/127.0.0.1/${port} 2>/dev/null && printf '\003\000' >&3 && read -t2 -n1 -u3 _" 2>/dev/null; then
        ready=1; break
    fi
    sleep 0.1
done
[ "$ready" = 1 ] || { echo "server did not become ready on port ${port}"; exit 1; }

# A spread of value shapes: atoms, int/float/sym vectors, list, string,
# a table select.  Each send is journaled as one frame on the server.
#
# Each payload is driven by its OWN one-shot client process: a top-level
# multi-statement script file does not share `set` globals across forms
# the way the REPL does, so we send exactly one message per process
# (inline `.ipc.open` + `.ipc.send`, no stored handle).  The client's own
# exit status is ignored — the frame lands in the server journal
# regardless of how the short-lived client tears down its socket.
client="${base}_client.rfl"
send_payloads=(
    '1 2 3'
    '(set a 1 2 3)'
    '(set c 1.5 2.5 3.5)'
    '(set s "hello world")'
    '(set f (1;(2;3);4))'
    '(+ 2 3)'
    '(select from ([] id:1 2 3) where id > 1)'
    '`x`y`z'
)
for pl in "${send_payloads[@]}"; do
    printf '(.ipc.send (.ipc.open "127.0.0.1:%s") "%s")\n' "$port" "$pl" > "$client"
    RAYFORCE_CORES=0 "$bin" "$client" >/dev/null 2>&1 || true
done

pkill -TERM -f "[r]ayforce -l ${base} -p ${port}" 2>/dev/null || true
for _ in $(seq 30); do
    pgrep -f "[r]ayforce -l ${base} -p ${port}" >/dev/null || break
    sleep 0.1
done

log="${base}.log"
[ -s "$log" ] || { echo "no journal produced at $log"; exit 1; }

# Split the log into per-frame files using the header length field.
#   0..3 prefix  4 version  5 flags  6 endian  7 msgtype  8..15 size(i64)
python3 - "$log" "$out_dir" <<'PY'
import struct, sys, os
log_path, out_dir = sys.argv[1], sys.argv[2]
data = open(log_path, "rb").read()
off = i = 0
n = len(data)
while off + 16 <= n:
    prefix, ver, flags, endian, msgtype, size = struct.unpack_from("<IBBBBq", data, off)
    if size < 0 or off + 16 + size > n:
        break
    with open(os.path.join(out_dir, f"frame-{i:05d}"), "wb") as f:
        f.write(data[off:off + 16 + size])
    off += 16 + size
    i += 1
print(f"wrote {i} frames to {out_dir}")
PY

exit 0
