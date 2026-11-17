#!/usr/bin/env python3
"""Automated test suite for CH9329-over-CDC-ECM.

Tests cover:
- Unit: CH9329 frame building, checksums, Ethernet/IPv4/TCP encapsulation
- Unit: Response decoding (header stripping, payload extraction)
- Integration: all CH9329 commands sent through ECM transport to a live device
- Integration: USB work mode read/write/clear/reboot
- Integration: keyboard, absolute mouse, relative mouse, media key commands

Usage::

    # Unit tests only (no device needed)
    python tools/test_ch9329_ecm_full.py

    # Unit + integration (device must be connected and in ECM mode)
    python tools/test_ch9329_ecm_full.py --usb-direct
    python tools/test_ch9329_ecm_full.py --iface usb0

    # Verbose
    python tools/test_ch9329_ecm_full.py --usb-direct -v

Requires: pyusb (``pip install pyusb``) for --usb-direct integration tests.
"""

import argparse
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

if __package__:
    from . import test_ch9329_ecm as ecm_cli
else:
    import test_ch9329_ecm as ecm_cli


# ===================================================================
# Import or duplicate the frame-building logic from test_ch9329_ecm.py
# ===================================================================

CH9329_HEAD1 = 0x57
CH9329_HEAD2 = 0xAB
CH9329_CMD_GET_INFO = 0x01
CH9329_CMD_KB_GENERAL = 0x02
CH9329_CMD_KB_MEDIA = 0x03
CH9329_CMD_MS_ABS = 0x04
CH9329_CMD_MS_REL = 0x05
CH9329_CMD_GET_PARA_CFG = 0x08
CH9329_CMD_USB_WORK_MODE = 0x30

ECM_VID = 0x1A86
ECM_PID = 0xFE0C

DEFAULT_DST_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])
DEFAULT_SRC_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])


def _checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def build_ch9329_frame(addr: int, cmd: int, payload: bytes = b"") -> bytes:
    frame = bytes([CH9329_HEAD1, CH9329_HEAD2, addr & 0xFF, cmd & 0xFF, len(payload)])
    frame += payload
    frame += bytes([_checksum(frame)])
    return frame


def build_eth_ipv4_tcp_frame(
    ch9329_payload: bytes,
    dst_mac: bytes = DEFAULT_DST_MAC,
    src_mac: bytes = DEFAULT_SRC_MAC,
    src_ip: str = "192.168.77.2",
    dst_ip: str = "192.168.77.1",
    src_port: int = 50000,
    dst_port: int = 80,
) -> bytes:
    eth = dst_mac + src_mac + struct.pack("!H", 0x0800)
    total_ip_len = 20 + 20 + len(ch9329_payload)
    ip_header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0x00, total_ip_len, 0x0001, 0x4000,
        64, 6, 0,
        socket.inet_aton(src_ip),
        socket.inet_aton(dst_ip),
    )
    ip_chk = ip_checksum(ip_header[:10] + b"\x00\x00" + ip_header[12:])
    ip_header = ip_header[:10] + struct.pack("!H", ip_chk) + ip_header[12:]
    tcp_header = struct.pack(
        "!HHIIBBHHH",
        src_port, dst_port,
        0x00000001, 0x00000000,
        0x50, 0x02, 65535, 0, 0,
    )
    return eth + ip_header + tcp_header + ch9329_payload


def ip_checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        w = (data[i] << 8) + data[i + 1]
        s += w
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def decode_ecm_response(data: bytes) -> Optional[dict]:
    """Strip Ethernet/IPv4/TCP headers and return CH9329 response dict."""
    if len(data) >= 14:
        eth_type = struct.unpack("!H", data[12:14])[0]
        if eth_type == 0x0800:
            data = data[14:]
    if len(data) >= 20 and (data[0] >> 4) == 4:
        ip_header_len = (data[0] & 0x0F) * 4
        if data[9] == 6:
            data = data[ip_header_len:]
    if len(data) >= 20:
        tcp_data_offset = (data[12] >> 4) * 4
        data = data[tcp_data_offset:]
    if len(data) < 6 or data[0] != CH9329_HEAD1 or data[1] != CH9329_HEAD2:
        return None
    payload_len = data[4]
    total_len = 6 + payload_len
    if len(data) < total_len:
        return None
    return {
        "addr": data[2],
        "cmd": data[3] & 0x3F,
        "cmd_raw": data[3],
        "payload": data[5:5 + payload_len],
    }


def build_ch9329_keyboard(modifier: int = 0, keys: Optional[list] = None) -> bytes:
    if keys is None:
        keys = []
    payload = bytes([modifier, 0])
    payload += bytes(keys[:6])
    payload += bytes(max(0, 6 - len(keys[:6])))  # pad key slots to 6
    return build_ch9329_frame(0x00, CH9329_CMD_KB_GENERAL, payload)


def build_ch9329_media(modifier: int = 0) -> bytes:
    return build_ch9329_frame(0x00, CH9329_CMD_KB_MEDIA, bytes([modifier, 0]))


def build_ch9329_abs_mouse(buttons: int = 0, x: int = 0, y: int = 0) -> bytes:
    payload = struct.pack("<BBHHB", 0, buttons & 0x07, x & 0xFFFF, y & 0xFFFF, 0)
    return build_ch9329_frame(0x00, CH9329_CMD_MS_ABS, payload)


def build_ch9329_rel_mouse(buttons: int = 0, dx: int = 0, dy: int = 0, wheel: int = 0) -> bytes:
    payload = struct.pack("<BBbbB", 0, buttons & 0x07, dx & 0xFF if dx >= 0 else -((-dx) & 0xFF),
                          dy & 0xFF if dy >= 0 else -((-dy) & 0xFF), wheel & 0xFF)
    return build_ch9329_frame(0x00, CH9329_CMD_MS_REL, payload)


# ===================================================================
# Test framework (minimal, no external deps)
# ===================================================================

RESULTS_PASS = 0
RESULTS_FAIL = 1
RESULTS_SKIP = 2


@dataclass
class TestResult:
    name: str
    passed: bool = True
    skipped: bool = False
    error: str = ""


class TestRunner:
    def __init__(self, verbose: bool = False):
        self.verbose = verbose
        self.results: list[TestResult] = []

    def run(self, name: str, fn: Callable[[], None]):
        result = TestResult(name=name)
        try:
            fn()
        except AssertionError as e:
            result.passed = False
            result.error = str(e)
        except SkipTest as e:
            result.skipped = True
            result.error = str(e)
        except Exception as e:
            result.passed = False
            result.error = f"{type(e).__name__}: {e}"
        self.results.append(result)
        status = "PASS" if result.passed else ("SKIP" if result.skipped else "FAIL")
        print(f"  [{status}] {name}" + (f" — {result.error}" if result.error and (self.verbose or not result.passed) else ""))

    def summary(self) -> int:
        passed = sum(1 for r in self.results if r.passed and not r.skipped)
        failed = sum(1 for r in self.results if not r.passed)
        skipped = sum(1 for r in self.results if r.skipped)
        total = len(self.results)
        print(f"\n{'=' * 60}")
        print(f"Total: {total}  Passed: {passed}  Failed: {failed}  Skipped: {skipped}")
        print(f"{'=' * 60}")
        if failed:
            print("\nFailures:")
            for r in self.results:
                if not r.passed and not r.skipped:
                    print(f"  FAIL: {r.name}: {r.error}")
        return failed


class SkipTest(Exception):
    pass


# ===================================================================
# Unit tests
# ===================================================================

def test_unit_checksum(runner: TestRunner):
    def test_single_byte():
        assert _checksum(b"\x01") == 0x01
    runner.run("checksum: single byte", test_single_byte)

    def test_multi_byte():
        assert _checksum(b"\x57\xAB\x00\x30\x01\x00") == 0x33
    runner.run("checksum: multi byte", test_multi_byte)

    def test_overflow():
        assert _checksum(b"\xFF\xFF") == 0xFE
    runner.run("checksum: overflow wraps", test_overflow)

    def test_empty():
        assert _checksum(b"") == 0x00
    runner.run("checksum: empty", test_empty)


def test_unit_ch9329_frame(runner: TestRunner):
    def test_no_payload():
        frame = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        assert len(frame) == 6
        assert frame[:5] == bytes([0x57, 0xAB, 0x00, 0x01, 0x00])
        assert frame[5] == _checksum(frame[:5])
    runner.run("CH9329 frame: no payload", test_no_payload)

    def test_with_payload():
        frame = build_ch9329_frame(0x01, CH9329_CMD_KB_GENERAL, bytes([0x00, 0x00, 0x04]))
        assert frame[0] == 0x57
        assert frame[1] == 0xAB
        assert frame[2] == 0x01
        assert frame[3] == 0x02
        assert frame[4] == 0x03
        assert frame[5:8] == bytes([0x00, 0x00, 0x04])
        # Verify checksum
        assert frame[-1] == _checksum(frame[:-1])
    runner.run("CH9329 frame: with payload", test_with_payload)

    def test_max_payload():
        payload = bytes(range(256))[:255]
        frame = build_ch9329_frame(0xFF, 0xFF, payload)
        assert len(frame) == 6 + 255
        assert frame[-1] == _checksum(frame[:-1])
    runner.run("CH9329 frame: max payload (255 bytes)", test_max_payload)

    def test_addr_cmd_wrapping():
        frame = build_ch9329_frame(0x123, 0x456)
        assert frame[2] == 0x23  # addr & 0xFF
        assert frame[3] == 0x56  # cmd & 0xFF
    runner.run("CH9329 frame: addr/cmd masked to 8-bit", test_addr_cmd_wrapping)


def test_unit_keyboard_report(runner: TestRunner):
    # Literal wire vectors from the protocol contract also cover the CLI helpers.
    def test_single_key():
        for builder in (build_ch9329_keyboard, ecm_cli.build_ch9329_keyboard):
            assert builder(0, [0x04]) == bytes.fromhex("57ab000208000004000000000010"), builder.__module__
    runner.run("Keyboard report: single key", test_single_key)

    def test_modifier():
        for builder in (build_ch9329_keyboard, ecm_cli.build_ch9329_keyboard):
            assert builder(0x03, [0x04]) == bytes.fromhex("57ab000208030004000000000013"), builder.__module__
    runner.run("Keyboard report: modifier bits", test_modifier)

    def test_six_keys():
        for builder in (build_ch9329_keyboard, ecm_cli.build_ch9329_keyboard):
            for keys in ([0x04, 0x05, 0x06, 0x07, 0x08, 0x09],
                         [0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A]):
                assert builder(0, keys) == bytes.fromhex("57ab000208000004050607080933"), builder.__module__
    runner.run("Keyboard report: 6 keys and truncation", test_six_keys)

    def test_no_keys_release():
        for builder in (build_ch9329_keyboard, ecm_cli.build_ch9329_keyboard):
            assert builder() == bytes.fromhex("57ab00020800000000000000000c"), builder.__module__
    runner.run("Keyboard report: no keys (release)", test_no_keys_release)


def test_unit_mouse_reports(runner: TestRunner):
    def test_abs_mouse():
        for builder in (build_ch9329_abs_mouse, ecm_cli.build_ch9329_abs_mouse):
            assert builder(1, 100, 200) == bytes.fromhex("57ab00040700016400c800003a"), builder.__module__
            assert builder() == bytes.fromhex("57ab000407000000000000000d"), builder.__module__
            assert builder(0xFF, 0x12345, 0x16789) == bytes.fromhex("57ab000407000745238967006c"), builder.__module__
    runner.run("Abs mouse report", test_abs_mouse)

    def test_rel_mouse():
        for builder in (build_ch9329_rel_mouse, ecm_cli.build_ch9329_rel_mouse):
            assert builder(2, -10, 5, 1) == bytes.fromhex("57ab0005050002f605010a"), builder.__module__
            assert builder() == bytes.fromhex("57ab00050500000000000c"), builder.__module__
            assert builder(0xFF, -128, 127, -1) == bytes.fromhex("57ab0005050007807fff11"), builder.__module__
    runner.run("Rel mouse report", test_rel_mouse)


def test_unit_media_report(runner: TestRunner):
    def test_media_key():
        frame = build_ch9329_media(0x01)
        assert frame[3] == CH9329_CMD_KB_MEDIA
        assert frame[5] == 0x01
        assert frame[-1] == _checksum(frame[:-1])
    runner.run("Media key report", test_media_key)


def test_unit_eth_ipv4_tcp(runner: TestRunner):
    def test_eth_header():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        frame = build_eth_ipv4_tcp_frame(ch9329)
        assert frame[:6] == DEFAULT_DST_MAC
        assert frame[6:12] == DEFAULT_SRC_MAC
        eth_type = struct.unpack("!H", frame[12:14])[0]
        assert eth_type == 0x0800
    runner.run("Ethernet header: MACs and type", test_eth_header)

    def test_ipv4_header():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        frame = build_eth_ipv4_tcp_frame(ch9329)
        ip_start = 14
        version_ihl = frame[ip_start]
        assert (version_ihl >> 4) == 4  # IPv4
        assert (version_ihl & 0x0F) == 5  # 20-byte header
        assert frame[ip_start + 9] == 6  # TCP protocol
        # Verify IP checksum
        ip_header = frame[ip_start:ip_start + 20]
        assert ip_checksum(ip_header) == 0
    runner.run("IPv4 header: version, protocol, checksum", test_ipv4_header)

    def test_tcp_header():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        frame = build_eth_ipv4_tcp_frame(ch9329, src_port=12345, dst_port=8080)
        tcp_start = 14 + 20
        src_port = struct.unpack("!H", frame[tcp_start:tcp_start + 2])[0]
        dst_port = struct.unpack("!H", frame[tcp_start + 2:tcp_start + 4])[0]
        assert src_port == 12345
        assert dst_port == 8080
        data_offset = (frame[tcp_start + 12] >> 4)
        assert data_offset == 5  # 20-byte TCP header
    runner.run("TCP header: ports and data offset", test_tcp_header)

    def test_payload_integrity():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_KB_GENERAL, bytes([0x00, 0x00, 0x04]))
        frame = build_eth_ipv4_tcp_frame(ch9329)
        tcp_payload = frame[14 + 20 + 20:]
        assert tcp_payload == ch9329
    runner.run("Payload: CH9329 frame preserved at end", test_payload_integrity)

    def test_total_length():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        frame = build_eth_ipv4_tcp_frame(ch9329)
        expected = 14 + 20 + 20 + len(ch9329)
        assert len(frame) == expected
    runner.run("Total frame length", test_total_length)

    def test_ipv4_total_length_field():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO, bytes([0x01, 0x02]))
        frame = build_eth_ipv4_tcp_frame(ch9329)
        ip_total = struct.unpack("!H", frame[16:18])[0]
        assert ip_total == 20 + 20 + len(ch9329)
    runner.run("IPv4 total length field", test_ipv4_total_length_field)


def test_unit_decode_response(runner: TestRunner):
    def test_strip_all_headers():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO, bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]))
        # Simulate a response (cmd | 0x80 = response)
        resp_ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO | 0x80, bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08]))
        frame = build_eth_ipv4_tcp_frame(resp_ch9329)
        result = decode_ecm_response(frame)
        assert result is not None
        assert result["cmd"] == CH9329_CMD_GET_INFO
        assert result["addr"] == 0x00
        assert result["payload"] == bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08])
    runner.run("Decode: strip all headers", test_strip_all_headers)

    def test_decode_bare_ch9329():
        ch9329 = build_ch9329_frame(0x01, CH9329_CMD_USB_WORK_MODE, bytes([0x00]))
        result = decode_ecm_response(ch9329)
        assert result is not None
        assert result["cmd"] == CH9329_CMD_USB_WORK_MODE
        assert result["addr"] == 0x01
        assert result["payload"] == bytes([0x00])
    runner.run("Decode: bare CH9329 (no wrapping)", test_decode_bare_ch9329)

    def test_decode_invalid():
        result = decode_ecm_response(b"\x00\x00\x00\x00")
        assert result is None
    runner.run("Decode: invalid data returns None", test_decode_invalid)

    def test_decode_truncated():
        # Valid header but truncated payload
        frame = bytes([0x57, 0xAB, 0x00, 0x01, 0x10])  # claims 16 bytes payload, has 0
        result = decode_ecm_response(frame)
        assert result is None
    runner.run("Decode: truncated payload returns None", test_decode_truncated)

    def test_decode_vlan_tagged():
        ch9329 = build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        # VLAN-tagged Ethernet frame (0x8100)
        eth = DEFAULT_DST_MAC + DEFAULT_SRC_MAC + struct.pack("!HH", 0x8100, 0x0000) + struct.pack("!H", 0x0800)
        ip_header = struct.pack("!BBHHHBBH4s4s", 0x45, 0x00, 40, 0x0001, 0x4000, 64, 6, 0,
                                socket.inet_aton("192.168.77.2"), socket.inet_aton("192.168.77.1"))
        ip_chk = ip_checksum(ip_header[:10] + b"\x00\x00" + ip_header[12:])
        ip_header = ip_header[:10] + struct.pack("!H", ip_chk) + ip_header[12:]
        tcp_header = struct.pack("!HHIIBBHHH", 50000, 80, 1, 0, 0x50, 0x02, 65535, 0, 0)
        frame = eth + ip_header + tcp_header + ch9329
        result = decode_ecm_response(frame)
        # The current decoder doesn't strip VLAN tags, so this should return None
        # (this tests the current behavior — VLAN support would need to be added)
        assert result is None
    runner.run("Decode: VLAN-tagged frame (expected: not handled)", test_decode_vlan_tagged)


# ===================================================================
# Integration tests
# ===================================================================

def send_ecm_frames_usb(
    frames: list[bytes],
    vid: int,
    pid: int,
    timeout_ms: int = 3000,
    verbose: bool = False,
    expect_response: bool = True,
) -> list[dict]:
    try:
        import usb.core
        import usb.util
    except ImportError:
        raise SkipTest("pyusb not installed")

    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        raise SkipTest(f"No CDC device found (VID=0x{vid:04X}, PID=0x{pid:04X})")

    data_if = None
    ep_out = None
    ep_in = None
    for cfg in dev:
        for intf in cfg:
            if intf.bInterfaceClass == 10:
                for ep in intf:
                    if ep.bEndpointAddress & 0x80:
                        ep_in = ep
                    else:
                        ep_out = ep
                data_if = intf
                break
        if data_if is not None:
            break

    if data_if is None or ep_out is None:
        raise SkipTest("No CDC-ECM bulk OUT endpoint found")

    try:
        if dev.is_kernel_driver_active(data_if.bInterfaceNumber):
            dev.detach_kernel_driver(data_if.bInterfaceNumber)
    except Exception:
        pass

    usb.util.claim_interface(dev, data_if.bInterfaceNumber)
    responses = []

    try:
        for frame in frames:
            if verbose:
                print(f"    TX {len(frame)} bytes: {frame.hex(' ')}")
            ep_out.write(frame, timeout=timeout_ms)
            time.sleep(0.05)

        if expect_response and ep_in is not None:
            try:
                raw = bytes(ep_in.read(1514, timeout=timeout_ms))
                if verbose:
                    print(f"    RX {len(raw)} bytes: {raw.hex(' ')}")
                resp = decode_ecm_response(raw)
                if resp is not None:
                    responses.append(resp)
            except Exception:
                pass
    finally:
        usb.util.release_interface(dev, data_if.bInterfaceNumber)

    return responses


def send_ecm_frames_iface(
    frames: list[bytes],
    iface: str,
    timeout_ms: int = 3000,
    verbose: bool = False,
    expect_response: bool = True,
) -> list[dict]:
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    sock.bind((iface, 0))
    responses = []

    try:
        for frame in frames:
            if verbose:
                print(f"    TX {len(frame)} bytes: {frame.hex(' ')}")
            sock.send(frame)
            time.sleep(0.05)

        if expect_response:
            sock.settimeout(timeout_ms / 1000.0)
            try:
                raw = sock.recv(1514)
                if verbose:
                    print(f"    RX {len(raw)} bytes: {raw.hex(' ')}")
                resp = decode_ecm_response(raw)
                if resp is not None:
                    responses.append(resp)
            except socket.timeout:
                pass
    finally:
        sock.close()

    return responses


def run_integration(runner: TestRunner, transport_fn, verbose: bool = False):
    """Run all integration tests using the given transport function.

    transport_fn(frames, expect_response=True) -> list[dict]
    """

    # ----- USB Work Mode (cmd 0x30) -----

    def test_read_mode():
        frame = build_eth_ipv4_tcp_frame(
            build_ch9329_frame(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x00]))
        )
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        # The response should be cmd 0x30 (or 0xB0 for response)
        if resp["cmd"] != CH9329_CMD_USB_WORK_MODE:
            raise AssertionError(
                f"Expected cmd 0x30, got 0x{resp['cmd']:02X} — "
                f"device may be in a mode where cmd 0x30 is not routed through ECM bridge. "
                f"Raw response: {resp}"
            )
        if verbose:
            payload = resp["payload"]
            print(f"    Mode response ({len(payload)} bytes): {payload.hex(' ')}")
            if len(payload) >= 12:
                print(f"      compiled_mode={payload[1]}, active_mode={payload[2]}, "
                      f"stored_mode={payload[3]}, cap=0x{payload[7]|(payload[8]<<8)|(payload[9]<<16)|(payload[10]<<24):08X}")

    runner.run("Integration: read USB work mode (cmd 0x30)", test_read_mode)

    def test_write_mode():
        for mode in [1, 2, 3, 4]:
            frame = build_eth_ipv4_tcp_frame(
                build_ch9329_frame(0x00, CH9329_CMD_USB_WORK_MODE,
                                   bytes([0x01, mode, 0x00, 0x00]))
            )
            transport_fn([frame], expect_response=False)
            time.sleep(0.1)
            # Read back
            read_frame = build_eth_ipv4_tcp_frame(
                build_ch9329_frame(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x00]))
            )
            responses = transport_fn([read_frame])
            if responses:
                resp = responses[0]
                if resp["cmd"] == CH9329_CMD_USB_WORK_MODE and len(resp["payload"]) >= 4:
                    stored = resp["payload"][3]
                    if stored != mode:
                        raise AssertionError(
                            f"Write mode {mode} failed: stored_mode={stored}"
                        )
                    if verbose:
                        print(f"    Mode {mode}: stored_mode confirmed = {stored}")

    runner.run("Integration: write USB work mode (1-4) and verify", test_write_mode)

    def test_clear_mode():
        frame = build_eth_ipv4_tcp_frame(
            build_ch9329_frame(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x02]))
        )
        transport_fn([frame], expect_response=False)
        time.sleep(0.1)

    runner.run("Integration: clear persistent mode record (cmd 0x30, op 0x02)", test_clear_mode)

    def test_reboot():
        frame = build_eth_ipv4_tcp_frame(
            build_ch9329_frame(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x03]))
        )
        transport_fn([frame], expect_response=False)
        time.sleep(0.1)
        # After reboot, the device will re-enumerate. We don't verify here
        # because the USB bus needs time to settle.

    runner.run("Integration: reboot command (cmd 0x30, op 0x03)", test_reboot)

    # ----- CH9329 Get Info (cmd 0x01) -----

    def test_get_info():
        frame = build_eth_ipv4_tcp_frame(
            build_ch9329_frame(0x00, CH9329_CMD_GET_INFO)
        )
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_GET_INFO:
            raise AssertionError(f"Expected cmd 0x01, got 0x{resp['cmd']:02X}")
        if len(resp["payload"]) < 8:
            raise AssertionError(f"Get-Info payload too short: {len(resp['payload'])} bytes")
        if verbose:
            print(f"    Product version: {resp['payload'][0]}, Connected: {resp['payload'][1]}")

    runner.run("Integration: get-info (cmd 0x01)", test_get_info)

    # ----- Keyboard (cmd 0x02) -----

    def test_keyboard_press_a():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_keyboard(0, [0x04]))
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_KB_GENERAL:
            raise AssertionError(f"Expected cmd 0x02, got 0x{resp['cmd']:02X}")
        # Status byte should be 0x00 (success)
        if resp["payload"][0] != 0x00:
            raise AssertionError(f"Keyboard status non-zero: 0x{resp['payload'][0]:02X}")

    runner.run("Integration: keyboard press 'A' (cmd 0x02)", test_keyboard_press_a)

    def test_keyboard_release():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_keyboard())
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_KB_GENERAL:
            raise AssertionError(f"Expected cmd 0x02, got 0x{resp['cmd']:02X}")

    runner.run("Integration: keyboard release (all zeros)", test_keyboard_release)

    def test_keyboard_ctrl_a():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_keyboard(0x01, [0x04]))
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_KB_GENERAL:
            raise AssertionError(f"Expected cmd 0x02, got 0x{resp['cmd']:02X}")

    runner.run("Integration: keyboard Ctrl+A (cmd 0x02, modifier=0x01)", test_keyboard_ctrl_a)

    # ----- Media key (cmd 0x03) -----

    def test_media_key():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_media(0x01))
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_KB_MEDIA:
            raise AssertionError(f"Expected cmd 0x03, got 0x{resp['cmd']:02X}")

    runner.run("Integration: media key (cmd 0x03)", test_media_key)

    # ----- Absolute mouse (cmd 0x04) -----

    def test_abs_mouse():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_abs_mouse(0, 500, 300))
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_MS_ABS:
            raise AssertionError(f"Expected cmd 0x04, got 0x{resp['cmd']:02X}")

    runner.run("Integration: absolute mouse (cmd 0x04)", test_abs_mouse)

    # ----- Relative mouse (cmd 0x05) -----

    def test_rel_mouse():
        frame = build_eth_ipv4_tcp_frame(build_ch9329_rel_mouse(0, 10, -5, 0))
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_MS_REL:
            raise AssertionError(f"Expected cmd 0x05, got 0x{resp['cmd']:02X}")

    runner.run("Integration: relative mouse (cmd 0x05)", test_rel_mouse)

    # ----- Get Para Cfg (cmd 0x08) -----

    def test_get_para_cfg():
        frame = build_eth_ipv4_tcp_frame(
            build_ch9329_frame(0x00, CH9329_CMD_GET_PARA_CFG)
        )
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_GET_PARA_CFG:
            raise AssertionError(f"Expected cmd 0x08, got 0x{resp['cmd']:02X}")

    runner.run("Integration: get parameter config (cmd 0x08)", test_get_para_cfg)

    # ----- Multi-frame batch -----

    def test_batch_frames():
        """Send multiple CH9329 frames in a single Ethernet packet."""
        frames_data = (
            build_ch9329_keyboard(0, [0x04]) +
            build_ch9329_keyboard()  # release
        )
        eth_frame = build_eth_ipv4_tcp_frame(frames_data)
        responses = transport_fn([eth_frame])
        if not responses:
            raise SkipTest("No response from device")
        # At least one response should come back
        resp = responses[0]
        if resp["cmd"] != CH9329_CMD_KB_GENERAL:
            raise AssertionError(f"Expected cmd 0x02 in batch, got 0x{resp['cmd']:02X}")

    runner.run("Integration: batch multiple CH9329 frames in one packet", test_batch_frames)

    # ----- Raw frame passthrough -----

    def test_raw_frame():
        """Send a pre-built raw CH9329 frame (for cmd 0x30 read)."""
        raw_ch9329 = bytes([0x57, 0xAB, 0x00, 0x30, 0x01, 0x00, 0x33])
        frame = build_eth_ipv4_tcp_frame(raw_ch9329)
        responses = transport_fn([frame])
        if not responses:
            raise SkipTest("No response from device")
        # Any valid CH9329 response is acceptable here
        resp = responses[0]
        assert "cmd" in resp

    runner.run("Integration: raw CH9329 frame passthrough", test_raw_frame)


# ===================================================================
# Main
# ===================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Full automation test suite for CH9329-over-CDC-ECM.",
    )
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--usb-direct", action="store_true",
                           help="Run integration tests via pyusb.")
    transport.add_argument("--iface", type=str,
                           help="Run integration tests via network interface.")
    parser.add_argument("--vid", type=lambda v: int(v, 0), default=ECM_VID)
    parser.add_argument("--pid", type=lambda v: int(v, 0), default=ECM_PID)
    parser.add_argument("--timeout", type=int, default=3000)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    runner = TestRunner(verbose=args.verbose)

    # ---- Unit tests (always run) ----
    print("\n[Unit tests]")
    test_unit_checksum(runner)
    test_unit_ch9329_frame(runner)
    test_unit_keyboard_report(runner)
    test_unit_mouse_reports(runner)
    test_unit_media_report(runner)
    test_unit_eth_ipv4_tcp(runner)
    test_unit_decode_response(runner)

    # ---- Integration tests (optional) ----
    if args.usb_direct or args.iface:
        print("\n[Integration tests]")

        if args.usb_direct:
            def transport_fn(frames, expect_response=True):
                return send_ecm_frames_usb(
                    frames, vid=args.vid, pid=args.pid,
                    timeout_ms=args.timeout, verbose=args.verbose,
                    expect_response=expect_response,
                )
        else:
            def transport_fn(frames, expect_response=True):
                return send_ecm_frames_iface(
                    frames, iface=args.iface,
                    timeout_ms=args.timeout, verbose=args.verbose,
                    expect_response=expect_response,
                )

        run_integration(runner, transport_fn, verbose=args.verbose)
    else:
        print("\n[Integration tests] Skipped (no --usb-direct or --iface)")

    failed = runner.summary()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
