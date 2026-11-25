#!/usr/bin/env python3
"""BLE-Eth tunnel throughput bench.

Echo round-trips through the tunnel at several payload sizes, reports
median throughput per size, and appends one JSONL record per run so
results can be compared across time slots (RF interference varies a lot
during the day: 19 KB/s @1460B in a clean morning, ~4.8 KB/s in a busy
afternoon on 2026-11-25, same firmware).

Usage: throughput_bench.py [label]
Env overrides: BLE_ADDR, TARGET, PORT, ROUNDS
"""
import asyncio
import json
import os
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

# Resolve the host library: repo layout first (tests/../tools), /tmp copy as fallback.
_HERE = Path(__file__).resolve().parent
for _p in (_HERE.parent / "tools", Path("/tmp/bletest/tools")):
    if (_p / "ble_eth_tunnel.py").exists():
        sys.path.insert(0, str(_p))
        break
else:
    sys.exit("ble_eth_tunnel.py not found (expected <repo>/tools/ or /tmp/bletest/tools)")
from ble_eth_tunnel import BleEthClient  # noqa: E402

BLE_ADDR = os.environ.get("BLE_ADDR", "1CC44426-51DA-E45A-73C8-2B01006A2D1E")
TARGET = os.environ.get("TARGET", "192.168.20.2")
PORT = int(os.environ.get("PORT", "9000"))
ROUNDS = int(os.environ.get("ROUNDS", "5"))
SIZES = (470, 1000, 1460, 2000, 4000, 6000)
RESULTS = _HERE / "results.jsonl"
# Reference baseline from the clean-RF morning run on 2026-11-25.
BASELINE_1460 = 19.0


def pattern(n, seed=0xAB):
    return bytes((seed + i) & 0xFF for i in range(n))


async def bench(label):
    c = BleEthClient(ble_address=BLE_ADDR)
    await c.connect()
    conn = await asyncio.wait_for(c.open_tunnel(TARGET, PORT), timeout=20)
    await asyncio.wait_for(c.recv_data(conn, timeout=8), timeout=10)  # banner

    print(f"\n===== {label} ({datetime.now().strftime('%Y-%m-%d %H:%M:%S')}) =====")
    results = {}
    for size in SIZES:
        msg = pattern(size)
        rtts, tputs = [], []
        for r in range(ROUNDS):
            t0 = time.time()
            await c.send_data(conn, msg)
            buf = bytearray()
            while len(buf) < size:
                part = await c.recv_data(conn, timeout=10)
                buf.extend(part)
            dt = time.time() - t0
            if bytes(buf[:size]) != msg:
                print(f"  {size}B round{r}: MISMATCH")
                continue
            rtts.append(dt)
            tputs.append(2 * size / dt / 1024)
        if not tputs:
            results[size] = None
            continue
        med = statistics.median(tputs)
        results[size] = {
            "median_kbps": round(med, 2),
            "min_kbps": round(min(tputs), 2),
            "max_kbps": round(max(tputs), 2),
            "rtt_ms": round(statistics.median(rtts) * 1000),
        }
        detail = " ".join(f"{t:.1f}" for t in tputs)
        print(f"  {size:>5}B  median {med:6.1f} KB/s  (min {min(tputs):5.1f} max {max(tputs):5.1f})  "
              f"rtt {results[size]['rtt_ms']:5.0f} ms   rounds: {detail}")
    st = c.get_reliability_stats()
    print(f"  ack_timeouts={st['tx_data_ack_timeout']} retransmits={st['tx_data_retransmit']} "
          f"window={st['ack_window_limit']}")
    await c.close_tunnel(conn)
    await c.disconnect()

    m1460 = (results.get(1460) or {}).get("median_kbps")
    verdict = ""
    if m1460:
        if m1460 >= BASELINE_1460 * 0.85:
            verdict = f"CLEAN-RF: {m1460} KB/s @1460B >= 85% of {BASELINE_1460} baseline"
        elif m1460 >= BASELINE_1460 * 0.5:
            verdict = f"MODERATE interference: {m1460} KB/s @1460B ({BASELINE_1460} baseline)"
        else:
            verdict = f"HEAVY interference: {m1460} KB/s @1460B ({BASELINE_1460} baseline)"
    print(f"  >> {verdict}")

    record = {
        "ts": datetime.now().isoformat(timespec="seconds"),
        "label": label,
        "sizes": results,
        "ack_timeouts": st["tx_data_ack_timeout"],
        "retransmits": st["tx_data_retransmit"],
        "verdict": verdict,
    }
    with open(RESULTS, "a") as f:
        f.write(json.dumps(record) + "\n")
    return record


if __name__ == "__main__":
    label = sys.argv[1] if len(sys.argv) > 1 else "bench"
    asyncio.run(bench(label))
