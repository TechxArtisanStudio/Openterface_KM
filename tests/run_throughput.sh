#!/bin/bash
# Persistent runner for the BLE-Eth throughput bench (tests/throughput_bench.py).
# - ensures a venv with bleak exists (survives /tmp cleanup, reboots)
# - runs the bench, tees output to a dated log, results go to results.jsonl
# Designed to be invoked by a scheduled task; exits non-zero on failure.
# Runtime artifacts (venv/, logs/, results.jsonl) are gitignored.

set -u
BASE="$(cd "$(dirname "$0")" && pwd)"
VENV="$BASE/venv"
PY="$VENV/bin/python"
LOGDIR="$BASE/logs"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/$(date +%Y%m%d_%H%M)_throughput.log"
LABEL="${1:-scheduled}"

# Venv bootstrap: create if missing or broken, install bleak once.
if ! "$PY" -c "import bleak" >/dev/null 2>&1; then
    echo "[runner] (re)creating venv..." | tee -a "$LOG"
    rm -rf "$VENV.tmp"
    python3 -m venv "$VENV.tmp" || exit 2
    "$VENV.tmp/bin/pip" -q install bleak || exit 2
    rm -rf "$VENV"
    mv "$VENV.tmp" "$VENV"
fi

echo "[runner] $LABEL start $(date '+%F %T')" | tee -a "$LOG"
if ! "$PY" -u "$BASE/throughput_bench.py" "$LABEL" 2>"$BASE/logs/stderr_tail.txt" | tee -a "$LOG"; then
    echo "[runner] bench FAILED, stderr tail:" | tee -a "$LOG"
    tail -5 "$BASE/logs/stderr_tail.txt" | tee -a "$LOG"
    exit 1
fi
echo "[runner] done $(date '+%F %T')" | tee -a "$LOG"
