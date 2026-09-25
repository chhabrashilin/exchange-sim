#!/usr/bin/env bash
# End-to-end test of the order-entry gateway: correctness, latency, and crash recovery.
#
#   scripts/e2e_gateway.sh [build-dir]
#
# A. Correctness: a client sends N orders over TCP and checks that the reports it got back are
#    byte-identical (via an order-sensitive digest) to a local engine run on the same input.
# B. Latency: open-loop paced load; latency is measured from each command's SCHEDULED send time.
# C. Crash recovery: kill -9 the server mid-stream, restart with --recover, and check
#      1. every command the client saw acknowledged is in the recovered journal (none lost),
#      2. the recovered state's digest equals an independent offline replay of the same journal,
#      3. a torn journal tail (simulated by truncating mid-record) is detected and discarded.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
W=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
SRV_PID=""
trap '[ -n "$SRV_PID" ] && kill -9 "$SRV_PID" 2>/dev/null || true; rm -rf "$W"' EXIT

start_server() {  # start_server <log> [extra args...]
  local log=$1; shift
  "$BUILD/exsim_server" --port "$PORT" --journal "$W/j.bin" "$@" > "$log" 2>&1 &
  SRV_PID=$!
  for _ in $(seq 100); do grep -q READY "$log" && return 0; sleep 0.05; done
  echo "server failed to start"; cat "$log"; exit 1
}
stop_server() { kill -TERM "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; SRV_PID=""; }

echo "=== A. correctness over TCP (closed loop, 500k orders) ==="
start_server "$W/a.log"
"$BUILD/exsim_client" --port "$PORT" --messages 500000 --window 256 | tee "$W/a.out"
stop_server
grep -q "IDENTICAL" "$W/a.out" || { echo "FAIL: server output differs from local engine"; exit 1; }

echo; echo "=== B. latency at a fixed offered load (open loop, 100k msg/s) ==="
start_server "$W/b.log" --sync os
"$BUILD/exsim_client" --port "$PORT" --messages 500000 --rate 100000 | grep -E "mode|throughput|round-trip|verification"
stop_server

echo; echo "=== C. crash recovery ==="
start_server "$W/c1.log"
( "$BUILD/exsim_client" --port "$PORT" --messages 3000000 --window 1024 --no-verify > "$W/c.out" 2>&1 || true ) &
CLI_PID=$!
# Crash only once real traffic is in the journal (a fixed sleep is racy: slow builds take longer to start sending).
for _ in $(seq 600); do
  [ "$(stat -c %s "$W/j.bin" 2>/dev/null || echo 0)" -gt 2000000 ] && break
  sleep 0.05
done
[ "$(stat -c %s "$W/j.bin")" -gt 2000000 ] || { echo "FAIL: no traffic reached the server"; exit 1; }
kill -9 "$SRV_PID"; wait "$SRV_PID" 2>/dev/null || true; SRV_PID=""      # the crash
wait "$CLI_PID" || true
ACKED=$(awk '/^ACKED/ {print $2}' "$W/c.out")
echo "client saw $ACKED commands acknowledged before the crash"

"$BUILD/exsim_journal" verify "$W/j.bin" | sed 's/^/journal after crash: /'
cp "$W/j.bin" "$W/j_crash.bin"
start_server "$W/c2.log" --recover
sleep 0.2
grep RECOVERED "$W/c2.log"
REC=$(sed -n 's/.*RECOVERED records=\([0-9]*\).*/\1/p' "$W/c2.log")
SRV_DIGEST=$(sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p' "$W/c2.log" | head -1)
stop_server

OFFLINE=$("$BUILD/exsim_journal" replay "$W/j_crash.bin")
echo "offline replay: $OFFLINE"
OFF_DIGEST=$(echo "$OFFLINE" | sed -n 's/.*digest=\([0-9a-f]*\).*/\1/p')
[ "$SRV_DIGEST" = "$OFF_DIGEST" ] && echo "recovered-state digest == offline replay digest: OK" \
  || { echo "FAIL: digests differ ($SRV_DIGEST vs $OFF_DIGEST)"; exit 1; }
[ "$REC" -ge "$ACKED" ] && echo "no acknowledged command lost: recovered $REC >= acked $ACKED: OK" \
  || { echo "FAIL: lost acknowledged commands (recovered $REC < acked $ACKED)"; exit 1; }

echo; echo "=== C2. torn tail: truncate the journal mid-record and recover ==="
SIZE=$(stat -c %s "$W/j_crash.bin")
truncate -s $((SIZE - 13)) "$W/j_crash.bin"
cp "$W/j_crash.bin" "$W/j.bin"
"$BUILD/exsim_journal" verify "$W/j.bin" | sed 's/^/before recovery: /'
start_server "$W/c3.log" --recover
sleep 0.2
grep RECOVERED "$W/c3.log"
grep -q "torn-tail-discarded" "$W/c3.log" || { echo "FAIL: torn tail not reported"; exit 1; }
stop_server
"$BUILD/exsim_journal" verify "$W/j.bin" | sed 's/^/after recovery: /'
echo; echo "ALL GATEWAY CHECKS PASSED"
