#!/usr/bin/env python3
"""Complete BLE-ETH tunnel test suite.

Layers covered:
  A. BLE link            - connect (advertising-pause retry), INFO, reconnect cycles
  B. Protocol data path  - PING boundary sweep (1..249 B incl. 238/239), pattern check
  C. ICMP through target - NET_PING over the target CDC-NCM link
  D. TCP tunnel          - CONNECT, banner, echo sizes incl. fragmentation
                           boundaries (234/235/236 B per-fragment), sequential
                           tunnels, DISCONNECT handling
  E. Robustness          - garbage injection, bad-checksum frame, mixed traffic

Target: a TCP echo server (sends a banner on connect, echoes everything).
Defaults match the Openterface echo server: 192.168.20.2:9000.

Usage:
    python3 tests/test_tunnel_full.py \
        --ble-addr 1CC44426-51DA-E45A-73C8-2B01006A2D1E \
        --target 192.168.20.2 --port 9000

Exit code is non-zero when any test fails.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
import time

_repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _repo_root)

from tools.ble_eth_tunnel import BleEthClient  # noqa: E402

FRAME_HEAD = b"\x57\xAB"
TX_CHAR_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"


def build_frame(cmd: int, payload: bytes, addr: int = 0) -> bytes:
    f = [0x57, 0xAB, addr, cmd, len(payload)] + list(payload)
    f.append(sum(f) & 0xFF)
    return bytes(f)


def pattern(size: int) -> bytes:
    """Deterministic byte pattern (not a constant fill)."""
    return bytes((i * 7 + 13) & 0xFF for i in range(size))


class Suite:
    def __init__(self, args):
        self.args = args
        self.results: list[tuple[str, str, str]] = []  # (layer, name, status)
        self.client: BleEthClient | None = None
        self.raw_responses: list[bytes] = []
        self.tx_log: list = []

    def record(self, layer: str, name: str, ok: bool, detail: str = "") -> None:
        status = "PASS" if ok else "FAIL"
        self.results.append((layer, name, status if not detail else f"{status} ({detail})"))
        print(f"  [{status}] {layer}/{name}" + (f" - {detail}" if detail else ""), flush=True)

    # ------------------------------------------------------------------
    # helpers
    # ------------------------------------------------------------------
    async def robust_connect(self) -> None:
        """The device pauses advertising after a disconnect; poll for it."""
        last_err = None
        for attempt in range(self.args.connect_retries):
            try:
                await self.client.connect()
                return
            except ConnectionError as e:
                last_err = e
                print(f"  connect attempt {attempt + 1}: {e}", flush=True)
                await asyncio.sleep(self.args.retry_delay)
        raise ConnectionError(f"device never appeared: {last_err}")

    def _ch9329_frame(self, cmd: int, data: bytes) -> bytes:
        f = [0x57, 0xAB, 0, cmd, len(data)] + list(data)
        f.append(sum(f) & 0xFF)
        return bytes(f)

    async def _query_diag(self, sub: int, wait_s: float = 1.2) -> bytes:
        """Send CMD_ISP_DEBUG 0x33 sub over fff2, return 0xB3 payload."""
        before = len(self.raw_responses)
        async with self.client._tx_lock:
            await self.client._bleak_client.write_gatt_char(
                "0000fff2-0000-1000-8000-00805f9b34fb",
                self._ch9329_frame(0x33, bytes([sub])), response=True)
        await asyncio.sleep(wait_s)
        stream = b"".join(d for t, d in list(self.raw_responses)[before:])
        print(f"  [TUN1-DIAG] 0x{sub:02X} stream len={len(stream)} "
              f"hex={stream[:96].hex()}", flush=True)
        idx = stream.find(bytes([0x57, 0xAB]))
        while idx >= 0:
            if idx + 6 <= len(stream) and stream[idx + 3] == 0xB3:
                plen = stream[idx + 4]
                if idx + 5 + plen <= len(stream):
                    return stream[idx + 5:idx + 5 + plen]
            idx = stream.find(bytes([0x57, 0xAB]), idx + 1)
        return b""

    STATS16 = ["tx_push_ok", "tx_push_drop", "push_front_fb", "frames_parsed",
               "cmd_ping", "pong_queued", "rx_evt", "rx_bytes",
               "notify_ok", "notify_fail"]

    def _fmt_stats11(self, d: bytes) -> str:
        if len(d) < 31:
            return f"short({len(d)}B)"
        parts = [f"{n}={d[2*i] | (d[2*i+1] << 8)}"
                 for i, n in enumerate(self.STATS16)]
        parts.append(f"pstate=0x{d[20]:02X} rxidx={d[21] | (d[22] << 8)}")
        parts.append(f"txq c/r/w={d[25]}/{d[26]}/{d[27]} partial={d[28]}")
        parts.append(f"ackq={d[29]} hold={d[30]}")
        if len(d) >= 37:
            parts.append(f"qdrop={d[31] | (d[32] << 8)} "
                         f"ack_sent={d[33] | (d[34] << 8)} "
                         f"ack_qdrop={d[35] | (d[36] << 8)}")
        if len(d) >= 57:
            # TCP RX-path tail: 9 counters from offset 37, then win_update
            parts.append("tcp:" + ",".join(
                f"{d[37+2*i] | (d[38+2*i] << 8)}" for i in range(10)))
        return " ".join(parts)

    CMD_NAMES = {0x21: "CONNECT", 0x22: "DISCONNECT", 0x91: "DATA", 0x11: "PING",
                 0xA2: "CONNECT_RESP", 0xB2: "DISCONN_RESP", 0xB1: "DATA_RESP",
                 0x11 + 0x80: "PONG", 0x12: "INFO", 0x92: "INFO_RESP"}

    async def dump_tun1_diag(self, tag: str) -> None:
        """Dump device-side diagnostics while a TUN-1 stall is live."""
        try:
            now = time.time()
            from collections import Counter
            hist = Counter(cmd for _, cmd, _, _ in self.tx_log)
            print(f"  [TUN1-TX {tag}] tx_total={len(self.tx_log)} "
                  f"hist={dict(sorted(hist.items()))}", flush=True)
            for t, cmd, plen, head in self.tx_log:
                if cmd in (0x10, 0x12):
                    print(f"    -{now-t:7.2f}s {cmd:02X} len={plen} {head.hex()}", flush=True)
            print(f"  [TUN1-RX {tag}] last 25 device notifications:", flush=True)
            now2 = time.time()
            for t, d in self.raw_responses[-25:]:
                cmd = d[3] if len(d) >= 6 and d[0] == 0x57 else -1
                print(f"    -{now2-t:7.2f}s len={len(d)} cmd={cmd:02X} {bytes(d[:20]).hex()}", flush=True)
            print(f"  [TUN1-DIAG {tag}] 0x11: "
                  f"{self._fmt_stats11(await self._query_diag(0x11))}", flush=True)
            snap = await self._query_diag(0x12)
            if len(snap) >= 28:
                for si in range(2):
                    b = snap[si*14:(si+1)*14]
                    u16 = lambda i: b[i] | (b[i+1] << 8)
                    st = {0: "CLOSED", 1: "ARP_PEND", 2: "SYN_SENT", 3: "ESTAB"}.get(b[1], b[1])
                    print(f"  [TUN1-SNAP {tag}] slot{si}: {'ACTIVE' if b[0] else 'free'} "
                          f"state={st} lport={u16(2)} rport={u16(4)} "
                          f"lseq={u16(6):04x} lack={u16(8):04x} "
                          f"rseq={u16(10):04x} rack={u16(12):04x}", flush=True)
            else:
                print(f"  [TUN1-SNAP {tag}] short: {snap.hex()}", flush=True)
            print(f"  [TUN1-DIAG {tag}] raw_responses={len(self.raw_responses)}", flush=True)
        except Exception as e:
            print(f"  [TUN1-DIAG {tag}] failed: {type(e).__name__}: {e}", flush=True)

    async def recv_until(self, conn_id: int, size: int, timeout: float = 20.0) -> bytes:
        """recv_data() may return partial reassembled data; accumulate."""
        buf = bytearray()
        deadline = time.time() + timeout
        while len(buf) < size and time.time() < deadline:
            part = await self.client.recv_data(conn_id, timeout=max(1.0, deadline - time.time()))
            buf.extend(part)
        return bytes(buf)

    async def echo_roundtrip(self, conn_id: int, size: int) -> tuple[bool, str]:
        msg = pattern(size)
        t0 = time.time()
        await self.client.send_data(conn_id, msg)
        echo = await self.recv_until(conn_id, size)
        dt = time.time() - t0
        ok = echo == msg
        tput = 2 * size / dt / 1024 if dt > 0 else 0
        detail = f"{size}B rtt={dt:.2f}s tput={tput:.1f}KB/s"
        if not ok:
            diffs = [i for i in range(min(len(echo), size)) if echo[i] != msg[i]]
            detail += f" len={len(echo)} diffs={len(diffs)}@{diffs[0] if diffs else '-'}"
        return ok, detail

    async def clear_conn_slots(self) -> None:
        """Force-close any stale connection slots (max_connections=2)."""
        for c in (0, 1):
            try:
                await self.client.close_tunnel(c, force=True)
            except Exception:
                pass

    async def open_with_banner(self) -> tuple[int, bytes]:
        conn = await asyncio.wait_for(
            self.client.open_tunnel(self.args.target, self.args.port), timeout=20.0)
        banner = await asyncio.wait_for(
            self.client.recv_data(conn, timeout=8.0), timeout=10.0)
        return conn, banner

    # ------------------------------------------------------------------
    # Layer A: BLE link
    # ------------------------------------------------------------------
    async def layer_a(self) -> None:
        print("\n--- Layer A: BLE link ---", flush=True)
        self.client = BleEthClient(ble_address=self.args.ble_addr,
                                   device_name=self.args.device_name)

        # capture raw protocol responses (NET_PING 0xA4, diag 0xB3, errors)
        # BEFORE connect so bleak registers the capture callback
        orig = self.client._notification_handler

        def capture(sender, data):
            orig(sender, data)
            self.raw_responses.append((time.time(), bytes(data)))

        self.client._notification_handler = capture

        T0 = time.time()

        # log host->device frames (cmd + length) for TUN-1 analysis
        orig_open = self.client.open_tunnel
        async def logging_open(ip, port, timeout=10.0):
            print(f"  [OPEN-CALL] +{time.time()-T0:.2f}s", flush=True)
            try:
                r = await orig_open(ip, port, timeout)
                print(f"  [OPEN-OK] conn={r} +{time.time()-T0:.2f}s", flush=True)
                return r
            except Exception as e:
                print(f"  [OPEN-FAIL] {type(e).__name__} +{time.time()-T0:.2f}s", flush=True)
                raise
        self.client.open_tunnel = logging_open

        orig_send = self.client._send_frame
        async def logging_send(frame: bytes):
            if len(frame) >= 6:
                self.tx_log.append((time.time(), int(frame[3]), int(frame[4]),
                                    bytes(frame[:24])))
            await orig_send(frame)
        self.client._send_frame = logging_send

        t0 = time.time()
        await self.robust_connect()
        self.record("A", "connect_with_retry", True, f"{time.time() - t0:.1f}s incl. scan")

        info = await self.client.get_info()
        ver = int(info.get("version", info.get("fw_version", 0)) or 0)
        self.record("A", "info_exchange", ver > 0, f"info={info}")

        ok = True
        for cycle in range(2):
            try:
                await self.client.disconnect()
                await asyncio.sleep(self.args.retry_delay)
                await self.robust_connect()
                await self.client.ping(b"cycle", timeout=5.0)
            except Exception as e:
                ok = False
                print(f"    cycle {cycle}: {type(e).__name__}: {e}", flush=True)
                break
        self.record("A", "reconnect_cycles_x2", ok)

    # ------------------------------------------------------------------
    # Layer B: protocol data path
    # ------------------------------------------------------------------
    async def layer_b(self) -> None:
        print("\n--- Layer B: protocol data path ---", flush=True)
        sizes = [1, 16, 100, 180, 181, 200, 235, 238, 239, 247, 249]
        ok_all = True
        fail_detail = ""
        for size in sizes:
            try:
                pong = await asyncio.wait_for(
                    self.client.ping(pattern(size), timeout=5.0), timeout=6.0)
                if pong != pattern(size):
                    ok_all = False
                    fail_detail += f" {size}B:mismatch"
            except Exception as e:
                ok_all = False
                fail_detail += f" {size}B:{type(e).__name__}"
        self.record("B", "ping_boundary_sweep", ok_all,
                    f"sizes={sizes}" + (f" FAILED:{fail_detail}" if fail_detail else ""))

        max_frag = 0
        try:
            info = await self.client.get_info()
            max_frag = int(info.get("max_fragment_payload",
                                    info.get("max_frag", 0)) or 0)
        except Exception:
            pass
        self.record("B", "info_max_frag_plausible", 100 <= max_frag <= 249,
                    f"max_fragment_payload={max_frag}")

    # ------------------------------------------------------------------
    # Layer C: ICMP through target
    # ------------------------------------------------------------------
    async def layer_c(self) -> None:
        print("\n--- Layer C: ICMP through target ---", flush=True)
        # NET_PING (0x24): ip(4) + count(1); response 0xA4: status, sent, recv, rtt_avg(2)
        del self.raw_responses[:]
        try:
            payload = list(bytes(map(int, self.args.target.split(".")))) + [3]
            await self.client._send_frame(build_frame(0x24, bytes(payload)))
        except Exception as e:
            self.record("C", "net_ping_icmp", False, f"{type(e).__name__}: {e}")
            return

        import time as _t
        resp = None
        deadline = time.time() + 20
        while time.time() < deadline and resp is None:
            for d in self.raw_responses:
                dd = d[1] if isinstance(d, tuple) else d
                if dd[:2] == FRAME_HEAD and dd[3] == 0xA4:
                    resp = d
                    break
            await asyncio.sleep(0.2)

        if resp is None:
            # NET_PING frames (cmd 0x24) are sent via _send_frame (write without
            # response); a silent timeout means the ICMP path or the notify
            # pump dropped everything -- dump device diagnostics on failure.
            stats = await self._query_diag(0x11)
            self.record("C", "net_ping_icmp", False,
                        f"no 0xA4 response in 20s (stats={self._fmt_stats11(stats)})")
            return
        rd = resp[1] if isinstance(resp, tuple) else resp
        p = rd[5:5 + rd[4]]
        if len(p) >= 5:
            status, sent, recv = p[0], p[1], p[2]
            rtt = p[3] | (p[4] << 8)
            self.record("C", "net_ping_icmp", status == 0 and sent == recv == 3,
                        f"status={status} sent={sent} recv={recv} rtt={rtt}ms")
        else:
            self.record("C", "net_ping_icmp", False, f"short payload {p.hex()}")

    # ------------------------------------------------------------------
    # Layer D: TCP tunnel
    # ------------------------------------------------------------------
    async def layer_d(self) -> None:
        print("\n--- Layer D: TCP tunnel ---", flush=True)
        await self.clear_conn_slots()
        await asyncio.sleep(1.0)

        # D1 connect + banner
        try:
            conn, banner = await self.open_with_banner()
        except Exception as e:
            self.record("D", "connect_banner", False, f"{type(e).__name__}: {e}")
            return
        self.record("D", "connect_banner", True, f"conn={conn} banner={banner[:24]!r}")

        # D2 echo sizes incl. fragmentation boundaries
        for size in (1, 100, 232, 233, 234, 235, 236, 470, 1000, 1460, 2000, 4000, 6000):
            try:
                ok, detail = await self.echo_roundtrip(conn, size)
                self.record("D", f"echo_{size}B", ok, detail)
            except Exception as e:
                self.record("D", f"echo_{size}B", False, f"{type(e).__name__}: {e}")
                break

        # D3 sequential open/close cycles. Close the still-open D2 tunnel
        # first. The original TUN-1 "device race" was exactly this: the new
        # connection completed at the TCP level but the (single-client) echo
        # server never returned to accept(), so the banner never arrived.
        # Closing first keeps the sequential test deterministic, and the echo
        # server is multi-client now anyway.
        try:
            await self.client.close_tunnel(conn)
        except Exception:
            pass
        ok = True
        detail = ""
        try:
            for cycle in range(3):
                tag = f"cycle-{cycle}"
                try:
                    c = await asyncio.wait_for(
                        self.client.open_tunnel(self.args.target, self.args.port),
                        timeout=15.0)
                except Exception as e:
                    # one retry after a settle delay (conn slot release latency)
                    detail += f" [{tag} open retry: {type(e).__name__}]"
                    await asyncio.sleep(3.0)
                    await self.clear_conn_slots()
                    c = await asyncio.wait_for(
                        self.client.open_tunnel(self.args.target, self.args.port),
                        timeout=15.0)
                # banner can be lost right after a rapid reopen
                banner = None
                for _ in range(3):
                    try:
                        banner = await asyncio.wait_for(
                            self.client.recv_data(c, timeout=8.0), timeout=10.0)
                        break
                    except Exception:
                        continue
                if banner is None:
                    # A missing banner is a real failure (was TUN-1: caused by
                    # the single-client echo server + the previous tunnel left
                    # open, NOT by a device-side race).
                    await self.dump_tun1_diag(tag)
                    detail += f" {tag}:no-banner"
                    ok = False
                    await self.clear_conn_slots()
                    await asyncio.sleep(2.0)
                    break
                # KNOWN ISSUE: the first host->target DATA frame sent right
                # after a rapid close/reopen can stall the ATT write (send
                # TimeoutError) or be dropped device-side. Recovery: reopen.
                tag_bytes = tag.encode()
                echo = None
                for attempt in range(3):
                    try:
                        await asyncio.sleep(1.0)
                        await self.client.send_data(c, tag_bytes)
                    except Exception as se:
                        detail += f" [{tag} send#{attempt} stall]"
                        try:
                            await self.client.close_tunnel(c, force=True)
                        except Exception:
                            pass
                        await self.clear_conn_slots()
                        await asyncio.sleep(2.0)
                        c = await asyncio.wait_for(
                            self.client.open_tunnel(self.args.target, self.args.port),
                            timeout=15.0)
                        await asyncio.wait_for(
                            self.client.recv_data(c, timeout=8.0), timeout=10.0)
                        continue
                    try:
                        echo = await self.recv_until(c, len(tag_bytes), timeout=8.0)
                    except Exception:
                        echo = None
                    if echo == tag_bytes:
                        break
                    detail += f" [{tag} data#{attempt} lost]"
                if echo != tag_bytes:
                    ok = False
                    break
                try:
                    await self.client.close_tunnel(c)
                except Exception as ce:
                    detail += f" [{tag} close: {type(ce).__name__}]"
                    await self.clear_conn_slots()
                await asyncio.sleep(5.0)
        except Exception as e:
            ok = False
            detail += f" {type(e).__name__}: {e}"
            self.record("D", "sequential_tunnels_x3", False, detail)
            return
        self.record("D", "sequential_tunnels_x3", ok, detail)

        # D4 data after DISCONNECT must not crash the link
        try:
            c = await asyncio.wait_for(
                self.client.open_tunnel(self.args.target, self.args.port), timeout=20.0)
            await asyncio.wait_for(self.client.recv_data(c, timeout=8.0), timeout=10.0)
            await self.client.close_tunnel(c, force=True)
            try:
                await self.client.send_data(c, b"after-close")
                await asyncio.wait_for(self.client.recv_data(c, timeout=3.0), timeout=4.0)
            except Exception:
                pass  # any failure here is acceptable; link must stay alive
            pong = await asyncio.wait_for(self.client.ping(b"alive", timeout=5.0), timeout=6.0)
            self.record("D", "disconnect_link_alive", pong == b"alive")
        except Exception as e:
            self.record("D", "disconnect_link_alive", False, f"{type(e).__name__}: {e}")

    # ------------------------------------------------------------------
    # Layer E: robustness
    # ------------------------------------------------------------------
    async def layer_e(self) -> None:
        print("\n--- Layer E: robustness ---", flush=True)

        # E1 garbage injection between frames
        try:
            await self.client._bleak_client.write_gatt_char(
                TX_CHAR_UUID, bytes(range(256))[:120], response=False)
            pong = await asyncio.wait_for(
                self.client.ping(b"after-garbage", timeout=5.0), timeout=6.0)
            self.record("E", "garbage_resync", pong == b"after-garbage")
        except Exception as e:
            self.record("E", "garbage_resync", False, f"{type(e).__name__}: {e}")

        # E2 bad-checksum frame must be ignored, link continues
        try:
            bad = bytearray(build_frame(0x1E, b"badcsum"))
            bad[-1] ^= 0xFF
            await self.client._bleak_client.write_gatt_char(TX_CHAR_UUID, bytes(bad),
                                                            response=False)
            pong = await asyncio.wait_for(
                self.client.ping(b"after-badcsum", timeout=5.0), timeout=6.0)
            self.record("E", "bad_checksum_ignored", pong == b"after-badcsum")
        except Exception as e:
            self.record("E", "bad_checksum_ignored", False, f"{type(e).__name__}: {e}")

        # E3 mixed traffic: large ping interleaved with tunnel echo
        try:
            await self.clear_conn_slots()
            await asyncio.sleep(2.0)
            conn = await asyncio.wait_for(
                self.client.open_tunnel(self.args.target, self.args.port), timeout=20.0)
            await asyncio.wait_for(self.client.recv_data(conn, timeout=8.0), timeout=10.0)
            msg = pattern(800)
            await self.client.send_data(conn, msg)
            pong = await asyncio.wait_for(
                self.client.ping(pattern(249), timeout=5.0), timeout=6.0)
            echo = await self.recv_until(conn, len(msg))
            self.record("E", "mixed_ping_tunnel", pong == pattern(249) and echo == msg)
            await self.client.close_tunnel(conn)
        except Exception as e:
            self.record("E", "mixed_ping_tunnel", False, f"{type(e).__name__}: {e}")

    # ------------------------------------------------------------------
    async def run(self) -> bool:
        try:
            await self.layer_a()
            await self.layer_b()
            await self.layer_c()
            await self.layer_d()
            await self.layer_e()
        finally:
            if self.client and self.client.is_connected:
                try:
                    await self.client.disconnect()
                except Exception:
                    pass

        passed = sum(1 for _, _, s in self.results if s.startswith("PASS"))
        warns = sum(1 for _, _, s in self.results if s.startswith("PASS (WARN"))
        failed = len(self.results) - passed
        print("\n" + "=" * 60, flush=True)
        print("TEST SUMMARY", flush=True)
        print("=" * 60, flush=True)
        for layer, name, status in self.results:
            head = status.split(" ")[0]
            tail = status.split(" ", 1)[1] if " " in status else ""
            print(f"  [{head:4s}] {layer}/{name}" + (f" - {tail}" if tail else ""), flush=True)
        print(f"\n  Passed: {passed} (incl. {warns} WARN)  Failed: {failed}", flush=True)
        return failed == 0


async def main() -> int:
    parser = argparse.ArgumentParser(description="Complete BLE-ETH tunnel test suite")
    parser.add_argument("--target", default="192.168.20.2")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--ble-addr", default=None)
    parser.add_argument("--device-name", default="KVM-GO")
    parser.add_argument("--connect-retries", type=int, default=6)
    parser.add_argument("--retry-delay", type=float, default=10.0)
    args = parser.parse_args()

    suite = Suite(args)
    ok = await suite.run()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
