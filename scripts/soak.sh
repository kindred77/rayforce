#!/usr/bin/env bash
#
# Long-running server soak: spawn a journaling rayforce, hammer it from
# several concurrent IPC clients, kill and restart it on a cycle, and watch
# resident memory for a leak.  Generalises the spawned-server pattern proven
# in test/rfl/system/log_journal.rfl (handshake readiness probe, external
# SIGTERM/SIGKILL, /tmp namespace cleanup).
#
# Kill/restart cycles defeat LeakSanitizer (it only reports at a clean
# exit), so the RSS slope IS the leak detector here: a healthy server holds
# steady-state RSS within a single generation, and each fresh generation
# after journal replay returns to roughly the same floor.
#
# Env:
#   SOAK_SECONDS   total run time            (default 300)
#   SOAK_CLIENTS   concurrent client loops   (default 4)
#   SOAK_PORT      IPC port                  (default 19980)
#   SOAK_BIN       server binary             (default ./rayforce)
#   SOAK_RSS_GROWTH_PCT  fail threshold, per-generation RSS growth (default 20)
#
# Usage: scripts/soak.sh   (run from the repo root; build the binary first)
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
bin="${SOAK_BIN:-$root/rayforce}"
port="${SOAK_PORT:-19980}"
seconds="${SOAK_SECONDS:-300}"
clients="${SOAK_CLIENTS:-4}"
growth_pct="${SOAK_RSS_GROWTH_PCT:-20}"
base="${TMPDIR:-/tmp}/rf_soak_$$"
rss_csv="${base}_rss.csv"

pat="[r]ayforce -l ${base} -p ${port}"
cleanup() { pkill -KILL -f "$pat" 2>/dev/null; rm -f "${base}"*; }
trap cleanup EXIT

[ -x "$bin" ] || { echo "soak: build $bin first"; exit 1; }

# ── spawn the server and wait for a completed IPC handshake ──────────
start_server() {
    pkill -KILL -f "$pat" 2>/dev/null
    RAYFORCE_CORES="${RAYFORCE_CORES:-2}" "$bin" -l "$base" -p "$port" </dev/null >>"${base}.stdout" 2>&1 &
    for _ in $(seq 100); do
        if bash -c "exec 3<>/dev/tcp/127.0.0.1/${port} 2>/dev/null && printf '\003\000' >&3 && read -t2 -n1 -u3 _" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

server_pid() { pgrep -f "$pat" | head -1; }

# ── one client: a run-and-exit script doing a mixed workload ────────
client_once() {
    local id="$1" epoch="$2"
    local f="${base}_client_${id}.rfl"
    cat > "$f" <<RFL
(set h (.ipc.open "127.0.0.1:${port}"))
(.ipc.send h "(set c${id} ${epoch})")
(.ipc.send h "(set v${id} (+ (til 1000) ${epoch}))")
(.ipc.send h "(select from ([] id:(til 100)) where id > 50)")
(.ipc.send h "(set soak_epoch ${epoch})")
(.ipc.close h)
RFL
    # Bound each client so a stuck connection can never wedge the soak.
    timeout 15 env RAYFORCE_CORES=1 "$bin" "$f" >/dev/null 2>&1 || true
}

rss_of() {
    local pid="$1"
    [ -n "$pid" ] && awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo 0
}

echo "soak: $seconds s, $clients clients, port $port, base $base"
start_server || { echo "soak: server never became ready"; exit 1; }

: > "$rss_csv"
gen=1
epoch=0
deadline=$(( $(cut -d. -f1 /proc/uptime) + seconds ))
next_restart=$(( $(cut -d. -f1 /proc/uptime) + ${SOAK_RESTART_SECS:-60} ))
fail=0

while [ "$(cut -d. -f1 /proc/uptime)" -lt "$deadline" ]; do
    # Burst of concurrent clients.  Wait only on the client PIDs — a bare
    # `wait` would also block on the persistent server started with `&`.
    cpids=""
    for id in $(seq 1 "$clients"); do
        epoch=$((epoch + 1))
        client_once "$id" "$epoch" &
        cpids="$cpids $!"
    done
    wait $cpids 2>/dev/null

    # Sample RSS for this generation.
    pid="$(server_pid)"
    rss="$(rss_of "$pid")"
    echo "$gen,$epoch,$rss" >> "$rss_csv"

    now="$(cut -d. -f1 /proc/uptime)"
    if [ "$now" -ge "$next_restart" ]; then
        # Alternate a clean SIGTERM and a hard SIGKILL to exercise both
        # graceful shutdown and dirty-death journal recovery.
        if [ $((gen % 2)) -eq 1 ]; then
            pkill -TERM -f "$pat" 2>/dev/null
        else
            pkill -KILL -f "$pat" 2>/dev/null
        fi
        for _ in $(seq 30); do server_pid >/dev/null || break; sleep 0.1; done
        gen=$((gen + 1))
        start_server || { echo "soak: restart (gen $gen) failed to come up"; fail=1; break; }
        # Journal replay must have restored the last epoch we set.
        vf="${base}_verify.rfl"
        printf '(set h (.ipc.open "127.0.0.1:%s"))\n(.ipc.send h "soak_epoch")\n(.ipc.close h)\n' "$port" > "$vf"
        timeout 15 env RAYFORCE_CORES=1 "$bin" "$vf" >/dev/null 2>&1 || true
        next_restart=$(( now + ${SOAK_RESTART_SECS:-60} ))
    fi
done

# ── leak verdict: per-generation RSS growth ─────────────────────────
# For each generation compare its first vs last sample; a steadily climbing
# resident set within a single server generation is the leak signal.
echo "soak: RSS samples -> $rss_csv"
python3 - "$rss_csv" "$growth_pct" <<'PY'
import sys, collections
path, thr = sys.argv[1], float(sys.argv[2])
gens = collections.OrderedDict()
for line in open(path):
    g, e, rss = line.strip().split(",")
    gens.setdefault(g, []).append(int(rss))
bad = False
for g, rss in gens.items():
    if len(rss) < 3: continue
    first, last = rss[0], rss[-1]
    if first <= 0: continue
    grow = 100.0 * (last - first) / first
    flag = "  <-- LEAK?" if grow > thr else ""
    print(f"  gen {g}: {first} -> {last} KiB  ({grow:+.1f}%){flag}")
    if grow > thr: bad = True
sys.exit(1 if bad else 0)
PY
rc=$?
[ "$fail" = 1 ] && rc=1
echo "soak: $([ $rc -eq 0 ] && echo PASS || echo FAIL)"
exit $rc
