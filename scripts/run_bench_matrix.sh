#!/usr/bin/env bash
# Reproducible bench for ONE codec (must match running server).
# Terminal A: bin/server -i bin/bench_local_proto.conf OR bench_local_msgpack.conf
# Terminal B: ./scripts/run_bench_matrix.sh protobuf|msgpack
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CODEC="${1:?usage: $0 protobuf|msgpack}"
BENCH="${BENCH:-${ROOT}/bin/bench_demo}"
SERVER_IP="${SERVER_IP:-127.0.0.1}"
SERVER_PORT="${SERVER_PORT:-8000}"
MAX_READY_WAIT_S="${MAX_READY_WAIT_S:-10}"

if [[ "$CODEC" != "protobuf" && "$CODEC" != "msgpack" ]]; then
  echo "codec must be protobuf or msgpack" >&2
  exit 1
fi

CONF="${ROOT}/bin/bench_local_${CODEC}.conf"
OUT="${ROOT}/docs/bench_results_${CODEC}.txt"

if [[ ! -x "$BENCH" ]]; then
  echo "bench_demo not found: $BENCH" >&2
  exit 1
fi

: "${BENCH_CONCURRENCY:=8}"
: "${BENCH_REQUESTS:=400}"

{
  echo "# Krpc bench matrix codec=${CODEC}"
  echo "# date: $(date -Iseconds 2>/dev/null || date)"
  echo "# host: $(uname -a)"
  echo "# cpu: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null || echo unknown)"
  echo "# BENCH_CONCURRENCY=$BENCH_CONCURRENCY BENCH_REQUESTS=$BENCH_REQUESTS"
  echo "# LB_STATIC_ENDPOINTS=${SERVER_IP}:${SERVER_PORT}"
  echo "# Server must use: bin/server -i bin/bench_local_${CODEC}.conf"
  echo ""
} >"$OUT"

export LB_STATIC_ENDPOINTS="${SERVER_IP}:${SERVER_PORT}"

wait_server_ready() {
  local elapsed=0
  while (( elapsed < MAX_READY_WAIT_S )); do
    if nc -z "${SERVER_IP}" "${SERVER_PORT}" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
    ((elapsed += 1))
  done
  return 1
}

if ! wait_server_ready; then
  {
    echo "ERROR: server ${SERVER_IP}:${SERVER_PORT} not ready within ${MAX_READY_WAIT_S}s"
    echo "       expected server config: bin/bench_local_${CODEC}.conf"
  } | tee -a "$OUT" >&2
  exit 2
fi

invalid_cases=0

for mode in sync async; do
  for conn in keepalive short; do
    for payload in 0 1 64 256 1024; do
      echo "=== codec=$CODEC mode=$mode conn=$conn payload_kb=$payload ===" | tee -a "$OUT"
      export BENCH_MODE="$mode"
      export BENCH_CONN="$conn"
      export BENCH_PAYLOAD_KB="$payload"

      CASE_LOG="$("$BENCH" -i "$CONF" 2>&1 || true)"
      printf "%s\n" "$CASE_LOG" | tee -a "$OUT"

      succ="$(printf "%s\n" "$CASE_LOG" | awk -F'[ =]+' '/^success=/{print $2}' | tail -n1)"
      fail="$(printf "%s\n" "$CASE_LOG" | awk -F'[ =]+' '/^success=/{print $4}' | tail -n1)"
      if [[ -z "${succ}" || -z "${fail}" ]]; then
        echo "WARN: invalid case (summary parse failed) codec=$CODEC mode=$mode conn=$conn payload_kb=$payload" | tee -a "$OUT" >&2
        ((invalid_cases += 1))
      elif [[ "$fail" != "0" ]]; then
        echo "WARN: invalid case (has failures) codec=$CODEC mode=$mode conn=$conn payload_kb=$payload success=$succ fail=$fail" | tee -a "$OUT" >&2
        ((invalid_cases += 1))
      fi

      echo "" | tee -a "$OUT"
    done
  done
done

if (( invalid_cases > 0 )); then
  {
    echo "ERROR: detected ${invalid_cases} invalid cases (failures or parse error)."
    echo "       benchmark results are not reliable for codec comparison."
    echo "       please verify server process, rpc_codec match, and connectivity."
  } | tee -a "$OUT" >&2
  exit 3
fi

echo "Wrote $OUT"
