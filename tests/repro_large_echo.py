#!/usr/bin/env python3
"""Large-echo reliability reproduction (the 4000 B corruption hunt).

Sends a large echo repeatedly with tracing; on any payload mismatch it
dumps the sent-vs-received bytes around the first differing offset and
stops. This is the script that exposed the phantom-reassembly bug fixed
by the FRAG_RETRANS protocol bit (2026-11-25): before the fix, one BLE
write loss per session caused a retransmitted FIRST fragment to be
treated as a fresh message, forwarding the payload twice from offset
1476.

Usage: repro_large_echo.py [size]
Env overrides: BLE_ADDR, TARGET, PORT
"""
import asyncio
import logging
import os
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve().parent
for _p in (_HERE.parent / "tools", Path("/tmp/bletest/tools")):
    if (_p / "ble_eth_tunnel.py").exists():
        sys.path.insert(0, str(_p))
        break
else:
    sys.exit("ble_eth_tunnel.py not found (expected <repo>/tools/ or /tmp/bletest/tools)")

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s.%(msecs)03d %(levelname)s %(message)s",
                    datefmt="%H:%M:%S")
for _n in ("asyncio", "bleak"):
    logging.getLogger(_n).setLevel(logging.WARNING)

from ble_eth_tunnel import BleEthClient  # noqa: E402

SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
BLE_ADDR = os.environ.get("BLE_ADDR", "1CC44426-51DA-E45A-73C8-2B01006A2D1E")
TARGET = os.environ.get("TARGET", "192.168.20.2")
PORT = int(os.environ.get("PORT", "9000"))


def pattern(n, seed=0xAB):
    return bytes((seed + i) & 0xFF for i in range(n))


async def main():
    c = BleEthClient(ble_address=BLE_ADDR, trace_frames=True)
    await c.connect()
    conn = await asyncio.wait_for(c.open_tunnel(TARGET, PORT), timeout=20)
    banner = await asyncio.wait_for(c.recv_data(conn, timeout=8), timeout=10)
    print("banner", banner[:24])
    ok_runs = 0
    for it in range(6):
        msg = pattern(SIZE, seed=0x20 + it)
        t0 = time.time()
        await c.send_data(conn, msg)
        buf = bytearray()
        try:
            while len(buf) < SIZE:
                part = await c.recv_data(conn, timeout=8)
                buf.extend(part)
        except Exception as e:
            print(f"iter{it}: recv err {e}")
        dt = time.time() - t0
        echo = bytes(buf[:SIZE])
        diffs = [i for i in range(min(len(echo), SIZE)) if echo[i] != msg[i]]
        st = c.get_reliability_stats()
        print(f"iter{it}: dt={dt:.2f} got={len(buf)} diffs={len(diffs)}@{diffs[0] if diffs else '-'} "
              f"ack_to={st['tx_data_ack_timeout']} retr={st['tx_data_retransmit']} w={st['ack_window_limit']}")
        if diffs:
            i = diffs[0]
            print("sent :", msg[max(0, i - 8):i + 24].hex())
            print("recv :", echo[max(0, i - 8):i + 24].hex())
            break
        ok_runs += 1
    await c.close_tunnel(conn)
    await c.disconnect()
    print("ok runs:", ok_runs)


asyncio.run(main())
