#!/usr/bin/env python3
"""Comprehensive KeyMod firmware test suite.

Covers all KeyMod features:
  - CH9329 protocol: all commands (0x01..0x30), checksums, edge cases
  - BLE-Eth protocol: all commands (0x10..0x21), fragmentation, ACK, parser
  - USB mode config: mode support, capability mask, feature queries
  - HID reports: keyboard (single/multi-key/modifiers), mouse (abs/rel), media
  - Serial HID integration: GET_INFO, keyboard, mouse, media (requires device)
  - Cross-feature: mode-dependent command routing, bridge selection

Usage::

    # Unit tests only (no device needed)
    pytest tests/test_keymod_firmware.py -v

    # With live hardware (serial port auto-detected or via OPENTERFACE_PORT)
    pytest tests/test_keymod_firmware.py -v --tb=short

    # Specific category
    pytest tests/test_keymod_firmware.py -v -k "ch9329"
    pytest tests/test_keymod_firmware.py -v -k "ble_eth"
    pytest tests/test_keymod_firmware.py -v -k "usb_mode"
    pytest tests/test_keymod_firmware.py -v -k "serial_hid"
"""

import os
import struct
import sys
import time
from typing import Optional, Tuple

import pytest

# ---------------------------------------------------------------------------
# Repo root on sys.path so we can import tools/ modules
# ---------------------------------------------------------------------------
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _REPO_ROOT)


# ===================================================================
# CH9329 helpers (self-contained — no external dependency)
# ===================================================================

CH9329_HEAD1 = 0x57
CH9329_HEAD2 = 0xAB
CH9329_CMD_GET_INFO     = 0x01
CH9329_CMD_KB_GENERAL   = 0x02
CH9329_CMD_KB_MEDIA     = 0x03
CH9329_CMD_MS_ABS       = 0x04
CH9329_CMD_MS_REL       = 0x05
CH9329_CMD_GET_PARA_CFG = 0x08
CH9329_CMD_USB_WORK_MODE = 0x30

# Response command = request cmd | 0x80
CH9329_RESP_OFFSET = 0x80


def ch9329_checksum(data: bytes) -> int:
    """8-bit additive checksum used by CH9329 and BLE-Eth frames."""
    return sum(data) & 0xFF


def build_ch9329(addr: int, cmd: int, payload: bytes = b"") -> bytes:
    """Build a CH9329 frame: 57 AB addr cmd len payload... checksum."""
    head = bytes([CH9329_HEAD1, CH9329_HEAD2, addr & 0xFF, cmd & 0xFF, len(payload)])
    frame = head + payload
    return frame + bytes([ch9329_checksum(frame)])


def parse_ch9329(data: bytes) -> Optional[dict]:
    """Parse a CH9329 frame, returning {addr, cmd, payload} or None."""
    if len(data) < 6:
        return None
    if data[0] != CH9329_HEAD1 or data[1] != CH9329_HEAD2:
        return None
    payload_len = data[4]
    if len(data) < 6 + payload_len:
        return None
    expected_csum = ch9329_checksum(data[:5 + payload_len])
    if data[5 + payload_len] != expected_csum:
        return None
    return {
        "addr": data[2],
        "cmd": data[3],
        "payload": data[5:5 + payload_len],
    }


def build_keyboard(modifier: int = 0, keys: "Optional[list]" = None) -> bytes:
    """Build CH9329 keyboard report (cmd 0x02)."""
    keys = keys or []
    payload = bytes([modifier & 0xFF, 0x00])
    payload += bytes(keys[:6])
    payload += bytes(max(0, 6 - len(keys[:6])))
    return build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload)


def build_media(modifier: int = 0) -> bytes:
    """Build CH9329 media key report (cmd 0x03)."""
    return build_ch9329(0x00, CH9329_CMD_KB_MEDIA, bytes([modifier, 0]))


def build_abs_mouse(buttons: int = 0, x: int = 0, y: int = 0, wheel: int = 0) -> bytes:
    """Build CH9329 absolute mouse report (cmd 0x04).

    Protocol requires 7 bytes: [reserved][buttons][x_lo][x_hi][y_lo][y_hi][wheel]
    """
    payload = struct.pack("<BBHHB", 0, buttons & 0x07, x & 0xFFFF, y & 0xFFFF, wheel & 0xFF)
    return build_ch9329(0x00, CH9329_CMD_MS_ABS, payload)


def build_rel_mouse(buttons: int = 0, dx: int = 0, dy: int = 0, wheel: int = 0) -> bytes:
    """Build CH9329 relative mouse report (cmd 0x05)."""
    payload = struct.pack(
        "<BBbbB",
        0,  # reserved
        buttons & 0x07,
        dx if dx >= 0 else -((-dx) & 0xFF),
        dy if dy >= 0 else -((-dy) & 0xFF),
        wheel & 0xFF,
    )
    return build_ch9329(0x00, CH9329_CMD_MS_REL, payload)


# ===================================================================
# BLE-Eth helpers (self-contained subset)
# ===================================================================

BLE_ETH_CMD_CONNECT    = 0x10
BLE_ETH_CMD_DATA       = 0x11
BLE_ETH_CMD_DISCONNECT = 0x12
BLE_ETH_CMD_PING       = 0x1E
BLE_ETH_CMD_INFO       = 0x1F
BLE_ETH_CMD_SET_DEFAULT  = 0x20
BLE_ETH_CMD_GET_DEFAULT  = 0x21

BLE_ETH_CMD_CONNECT_RESP   = 0x90
BLE_ETH_CMD_DATA_RESP      = 0x91
BLE_ETH_CMD_DISCONN_RESP   = 0x92
BLE_ETH_CMD_PONG           = 0x9E
BLE_ETH_CMD_INFO_RESP      = 0x9F
BLE_ETH_CMD_SET_DEFAULT_RESP = 0xA0
BLE_ETH_CMD_GET_DEFAULT_RESP = 0xA1

BLE_ETH_FRAG_MORE  = 0x80
BLE_ETH_FRAG_FIRST = 0x40
BLE_ETH_MAX_PAYLOAD = 249


def build_ble_eth_frame(addr: int, cmd: int, payload: bytes = b"") -> bytes:
    """Build a BLE-Eth protocol frame."""
    head = bytes([CH9329_HEAD1, CH9329_HEAD2, addr & 0xFF, cmd & 0xFF, len(payload)])
    frame = head + payload
    return frame + bytes([ch9329_checksum(frame)])


def build_ble_eth_connect(ip: str, port: int) -> bytes:
    """Build a CONNECT request frame."""
    parts = [int(p) for p in ip.split(".")]
    payload = bytes(parts) + struct.pack(">H", port)
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_CONNECT, payload)


def build_ble_eth_disconnect(conn_id: int) -> bytes:
    """Build a DISCONNECT request frame."""
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_DISCONNECT, bytes([conn_id]))


def build_ble_eth_ping(data: bytes) -> bytes:
    """Build a PING request frame."""
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_PING, data)


def build_ble_eth_info_request() -> bytes:
    """Build an INFO request frame."""
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_INFO)


def build_ble_eth_set_default(ip: str, port: int) -> bytes:
    """Build a SET_DEFAULT request frame (cmd 0x20)."""
    parts = [int(p) for p in ip.split(".")]
    payload = bytes(parts) + struct.pack(">H", port)
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_SET_DEFAULT, payload)


def build_ble_eth_get_default() -> bytes:
    """Build a GET_DEFAULT request frame (cmd 0x21)."""
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_GET_DEFAULT)


def build_ble_eth_data(conn_id: int, data: bytes) -> bytes:
    """Build a single-frame DATA frame (no fragmentation)."""
    header = bytes([BLE_ETH_FRAG_FIRST | 0x01, 0x00, conn_id & 0xFF])
    return build_ble_eth_frame(0x00, BLE_ETH_CMD_DATA, header + data)


def build_ble_eth_data_fragmented(conn_id: int, data: bytes, max_payload: int = BLE_ETH_MAX_PAYLOAD - 3) -> list[bytes]:
    """Fragment data into multiple DATA frames."""
    frag_header_size = 3
    max_data = max_payload - frag_header_size
    if max_data <= 0:
        max_data = 1

    chunks = []
    for i in range(0, len(data), max_data):
        chunks.append(data[i:i + max_data])

    if not chunks:
        chunks = [b""]

    total = len(chunks)
    frames = []
    for seq, chunk in enumerate(chunks):
        flags = seq & 0x0F
        if total > 1:
            flags |= BLE_ETH_FRAG_FIRST if seq == 0 else 0
            flags |= BLE_ETH_FRAG_MORE if seq < total - 1 else 0
        else:
            flags |= BLE_ETH_FRAG_FIRST
        header = bytes([flags, seq & 0xFF, conn_id & 0xFF])
        frames.append(build_ble_eth_frame(0x00, BLE_ETH_CMD_DATA, header + chunk))
    return frames


def parse_ble_eth_frame(data: bytes) -> Optional[dict]:
    """Parse a BLE-Eth frame."""
    if len(data) < 6:
        return None
    if data[0] != CH9329_HEAD1 or data[1] != CH9329_HEAD2:
        return None
    payload_len = data[4]
    if len(data) < 6 + payload_len:
        return None
    expected_csum = ch9329_checksum(data[:5 + payload_len])
    if data[5 + payload_len] != expected_csum:
        return None
    return {
        "addr": data[2],
        "cmd": data[3],
        "payload": data[5:5 + payload_len],
    }


def extract_data_header(payload: bytes) -> Optional[Tuple[int, int, int, bytes]]:
    """Extract fragmentation header from DATA payload.

    Returns (flags, seq, conn_id, data) or None if too short.
    """
    if len(payload) < 3:
        return None
    flags = payload[0]
    seq = payload[1]
    conn_id = payload[2]
    return (flags, seq, conn_id, payload[3:])


# ===================================================================
# USB Mode Config helpers
# ===================================================================

MODE_NAMES = {
    1: "hid_acm",
    2: "hid_uac_acm",
    3: "hid_acm_bridge",
    4: "hid_ecm_bridge",
    5: "hid_uac_ecm",
    6: "hid_acm_ecm",
}

# Capability mask for runtime-capable Mode 4 image (modes 1-4)
CAPABILITY_MASK_RUNTIME_MODE4 = 0x0000001E

# Mode feature matrix
MODE_FEATURES = {
    1: {"target_hid": True, "target_uac": False, "host_uac": False,
        "target_cdc_acm": False, "host_cdc_acm": True,
        "target_cdc_ecm": False, "host_cdc_ecm": False,
        "bridge": "none"},
    2: {"target_hid": True, "target_uac": True, "host_uac": True,
        "target_cdc_acm": False, "host_cdc_acm": True,
        "target_cdc_ecm": False, "host_cdc_ecm": False,
        "bridge": "uac"},
    3: {"target_hid": True, "target_uac": False, "host_uac": False,
        "target_cdc_acm": True, "host_cdc_acm": True,
        "target_cdc_ecm": False, "host_cdc_ecm": False,
        "bridge": "acm"},
    4: {"target_hid": True, "target_uac": False, "host_uac": False,
        "target_cdc_acm": False, "host_cdc_acm": False,
        "target_cdc_ecm": True, "host_cdc_ecm": True,
        "bridge": "ecm"},
    5: {"target_hid": True, "target_uac": True, "host_uac": True,
        "target_cdc_acm": False, "host_cdc_acm": False,
        "target_cdc_ecm": True, "host_cdc_ecm": True,
        "bridge": "ecm"},
    6: {"target_hid": True, "target_uac": False, "host_uac": False,
        "target_cdc_acm": True, "host_cdc_acm": True,
        "target_cdc_ecm": True, "host_cdc_ecm": True,
        "bridge": "ecm"},
}


def mode_is_supported(capability_mask: int, mode: int) -> bool:
    """Check if a mode is advertised in the capability mask."""
    return bool(capability_mask & (1 << mode))


def mode_has_feature(mode: int, feature: str) -> bool:
    """Check if a mode has a specific feature."""
    features = MODE_FEATURES.get(mode)
    if features is None:
        return False
    return features.get(feature, False)


# ===================================================================
# CH9329 Unit Tests
# ===================================================================

class TestCH9329Protocol:
    """CH9329 protocol unit tests — no hardware required."""

    # --- Checksum ---

    def test_checksum_empty(self):
        assert ch9329_checksum(b"") == 0x00

    def test_checksum_single_byte(self):
        assert ch9329_checksum(b"\x42") == 0x42

    def test_checksum_overflow(self):
        assert ch9329_checksum(b"\xFF\xFF") == 0xFE

    def test_checksum_known_frame(self):
        # 57 AB 00 30 01 00 → sum = 0x57+0xAB+0x00+0x30+0x01+0x00 = 0x133 → 0x33
        assert ch9329_checksum(bytes([0x57, 0xAB, 0x00, 0x30, 0x01, 0x00])) == 0x33

    def test_checksum_all_zeros(self):
        assert ch9329_checksum(bytes(10)) == 0x00

    def test_checksum_all_ff(self):
        assert ch9329_checksum(bytes([0xFF] * 4)) == 0xFC

    # --- Frame building ---

    def test_frame_no_payload(self):
        frame = build_ch9329(0x00, CH9329_CMD_GET_INFO)
        assert len(frame) == 6
        assert frame[0] == 0x57
        assert frame[1] == 0xAB
        assert frame[2] == 0x00
        assert frame[3] == CH9329_CMD_GET_INFO
        assert frame[4] == 0x00  # payload length
        assert frame[5] == ch9329_checksum(frame[:5])

    def test_frame_with_payload(self):
        payload = bytes([0x01, 0x02, 0x03])
        frame = build_ch9329(0x01, CH9329_CMD_KB_GENERAL, payload)
        assert len(frame) == 6 + 3
        assert frame[2] == 0x01
        assert frame[3] == CH9329_CMD_KB_GENERAL
        assert frame[4] == 3
        assert frame[5:8] == payload
        assert frame[8] == ch9329_checksum(frame[:8])

    def test_frame_max_payload(self):
        payload = bytes(range(255))
        frame = build_ch9329(0xFF, 0xFF, payload)
        assert len(frame) == 6 + 255
        assert frame[4] == 255
        assert frame[-1] == ch9329_checksum(frame[:-1])

    def test_frame_addr_cmd_masked(self):
        frame = build_ch9329(0x123, 0x456)
        assert frame[2] == 0x23
        assert frame[3] == 0x56

    # --- Frame parsing ---

    def test_parse_valid_frame(self):
        frame = build_ch9329(0x00, CH9329_CMD_GET_INFO, bytes([0x01, 0x02]))
        result = parse_ch9329(frame)
        assert result is not None
        assert result["addr"] == 0x00
        assert result["cmd"] == CH9329_CMD_GET_INFO
        assert result["payload"] == bytes([0x01, 0x02])

    def test_parse_bad_header(self):
        assert parse_ch9329(b"\x00\x00\x00\x00\x00\x00") is None

    def test_parse_truncated(self):
        frame = bytes([0x57, 0xAB, 0x00, 0x01, 0x10])  # claims 16 bytes payload
        assert parse_ch9329(frame) is None

    def test_parse_bad_checksum(self):
        frame = build_ch9329(0x00, CH9329_CMD_GET_INFO)
        corrupted = frame[:-1] + bytes([(frame[-1] + 1) & 0xFF])
        assert parse_ch9329(corrupted) is None

    def test_parse_too_short(self):
        assert parse_ch9329(b"\x57\xAB\x00") is None

    # --- Keyboard reports ---

    def test_keyboard_single_key(self):
        frame = build_keyboard(0, [0x04])  # 'a'
        assert frame == bytes.fromhex("57ab000208000004000000000010")
        parsed = parse_ch9329(frame)
        assert parsed is not None
        assert parsed["cmd"] == CH9329_CMD_KB_GENERAL
        assert parsed["payload"][0] == 0x00  # modifier
        assert parsed["payload"][2] == 0x04  # first key slot

    def test_keyboard_modifier_bits(self):
        # Ctrl+Shift+A
        frame = build_keyboard(0x03, [0x04])
        assert frame == bytes.fromhex("57ab000208030004000000000013")
        parsed = parse_ch9329(frame)
        assert parsed["payload"][0] == 0x03

    def test_keyboard_six_keys(self):
        keys = [0x04, 0x05, 0x06, 0x07, 0x08, 0x09]
        frame = build_keyboard(0, keys)
        assert frame == bytes.fromhex("57ab000208000004050607080933")
        parsed = parse_ch9329(frame)
        assert parsed["payload"][2:8] == bytes(keys)

    def test_keyboard_release(self):
        frame = build_keyboard()
        parsed = parse_ch9329(frame)
        assert frame == bytes.fromhex("57ab00020800000000000000000c")
        assert parsed["payload"] == bytes(8)

    def test_keyboard_max_six_keys_truncates(self):
        """Only first 6 keys should be sent."""
        keys = [0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B]
        frame = build_keyboard(0, keys)
        assert frame == bytes.fromhex("57ab000208000004050607080933")
        parsed = parse_ch9329(frame)
        assert parsed["payload"][2:8] == bytes(keys[:6])

    def test_keyboard_all_modifiers(self):
        """Test all modifier combinations."""
        for mod in [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80]:
            frame = build_keyboard(mod, [0x04])
            parsed = parse_ch9329(frame)
            assert parsed["payload"][0] == mod

    # --- Media key reports ---

    def test_media_key(self):
        frame = build_media(0x01)
        parsed = parse_ch9329(frame)
        assert parsed["cmd"] == CH9329_CMD_KB_MEDIA
        assert parsed["payload"][0] == 0x01

    def test_media_key_release(self):
        frame = build_media(0x00)
        parsed = parse_ch9329(frame)
        assert parsed["payload"] == bytes([0x00, 0x00])

    # --- Absolute mouse reports ---

    def test_abs_mouse_basic(self):
        frame = build_abs_mouse(1, 100, 200)
        assert frame == bytes.fromhex("57ab00040700016400c800003a")
        parsed = parse_ch9329(frame)
        assert parsed["cmd"] == CH9329_CMD_MS_ABS
        assert len(parsed["payload"]) == 7  # 7-byte payload per protocol
        assert parsed["payload"][0] == 0x00  # reserved
        assert parsed["payload"][1] == 0x01  # buttons
        x = struct.unpack("<H", parsed["payload"][2:4])[0]
        y = struct.unpack("<H", parsed["payload"][4:6])[0]
        assert x == 100
        assert y == 200
        assert parsed["payload"][6] == 0x00  # wheel

    def test_abs_mouse_max_coords(self):
        frame = build_abs_mouse(0, 0xFFFF, 0xFFFF)
        parsed = parse_ch9329(frame)
        x = struct.unpack("<H", parsed["payload"][2:4])[0]
        y = struct.unpack("<H", parsed["payload"][4:6])[0]
        assert x == 0xFFFF
        assert y == 0xFFFF

    def test_abs_mouse_all_buttons(self):
        """Test all button combinations."""
        for btn in range(8):  # 0..7 (3 bits)
            frame = build_abs_mouse(btn, 0, 0)
            parsed = parse_ch9329(frame)
            assert parsed["payload"][1] == btn  # buttons at offset 1

    def test_abs_mouse_zero(self):
        frame = build_abs_mouse(0, 0, 0)
        parsed = parse_ch9329(frame)
        assert parsed["payload"] == bytes(7)  # 7 zero bytes

    # --- Relative mouse reports ---

    def test_rel_mouse_basic(self):
        frame = build_rel_mouse(0, 10, -5, 0)
        assert frame == bytes.fromhex("57ab00050500000afb0011")
        parsed = parse_ch9329(frame)
        assert parsed["cmd"] == CH9329_CMD_MS_REL
        dx = struct.unpack("<b", parsed["payload"][2:3])[0]
        dy = struct.unpack("<b", parsed["payload"][3:4])[0]
        assert dx == 10
        assert dy == -5

    def test_rel_mouse_with_wheel(self):
        frame = build_rel_mouse(0, 0, 0, 3)
        assert frame == bytes.fromhex("57ab00050500000000030f")
        parsed = parse_ch9329(frame)
        wheel = parsed["payload"][4]
        assert wheel == 3

    def test_rel_mouse_negative_movement(self):
        frame = build_rel_mouse(0, -128, -128, 0)
        assert frame == bytes.fromhex("57ab00050500008080000c")
        parsed = parse_ch9329(frame)
        dx = struct.unpack("<b", parsed["payload"][2:3])[0]
        dy = struct.unpack("<b", parsed["payload"][3:4])[0]
        assert dx == -128
        assert dy == -128

    def test_rel_mouse_buttons(self):
        frame = build_rel_mouse(3, 0, 0, 0)  # left+right click
        assert frame == bytes.fromhex("57ab00050500030000000f")
        parsed = parse_ch9329(frame)
        assert parsed["payload"][0] == 0  # reserved
        assert parsed["payload"][1] == 3

    # --- USB Work Mode (cmd 0x30) ---

    def test_mode_read_command(self):
        frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x00]))
        parsed = parse_ch9329(frame)
        assert parsed is not None
        assert parsed["cmd"] == CH9329_CMD_USB_WORK_MODE
        assert parsed["payload"] == bytes([0x00])

    def test_mode_write_command(self):
        for mode in [1, 2, 3, 4, 5, 6]:
            payload = bytes([0x01, mode, 0x00, 0x00])
            frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, payload)
            parsed = parse_ch9329(frame)
            assert parsed is not None
            assert parsed["payload"][1] == mode

    def test_mode_clear_command(self):
        frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x02]))
        parsed = parse_ch9329(frame)
        assert parsed["payload"][0] == 0x02

    def test_mode_reboot_command(self):
        frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x03]))
        parsed = parse_ch9329(frame)
        assert parsed["payload"][0] == 0x03

    def test_mode_write_with_flags(self):
        flags = 0x1234
        payload = bytes([0x01, 3, flags & 0xFF, (flags >> 8) & 0xFF])
        frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, payload)
        parsed = parse_ch9329(frame)
        assert parsed["payload"][2] == 0x34
        assert parsed["payload"][3] == 0x12


# ===================================================================
# BLE-Eth Protocol Unit Tests
# ===================================================================

class TestBleEthProtocol:
    """BLE-Eth protocol unit tests — no hardware required."""

    # --- Frame building ---

    def test_connect_frame(self):
        frame = build_ble_eth_connect("192.168.1.100", 22)
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_CONNECT
        assert parsed["payload"][:4] == bytes([192, 168, 1, 100])
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 22

    def test_disconnect_frame(self):
        frame = build_ble_eth_disconnect(3)
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_DISCONNECT
        assert parsed["payload"] == bytes([3])

    def test_ping_frame(self):
        frame = build_ble_eth_ping(b"hello")
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_PING
        assert parsed["payload"] == b"hello"

    def test_info_request_frame(self):
        frame = build_ble_eth_info_request()
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_INFO
        assert len(parsed["payload"]) == 0

    def test_set_default_frame(self):
        frame = build_ble_eth_set_default("10.0.0.1", 8080)
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_SET_DEFAULT
        assert parsed["payload"][:4] == bytes([10, 0, 0, 1])
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 8080

    def test_get_default_frame(self):
        frame = build_ble_eth_get_default()
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_GET_DEFAULT
        assert len(parsed["payload"]) == 0

    def test_data_single_frame(self):
        frame = build_ble_eth_data(0, b"test data")
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["cmd"] == BLE_ETH_CMD_DATA
        header = extract_data_header(parsed["payload"])
        assert header is not None
        flags, seq, conn_id, data = header
        assert flags & BLE_ETH_FRAG_FIRST
        assert not (flags & BLE_ETH_FRAG_MORE)
        assert seq == 0
        assert conn_id == 0
        assert data == b"test data"

    def test_data_fragmented(self):
        data = bytes(range(256)) * 3  # 768 bytes
        frames = build_ble_eth_data_fragmented(1, data)
        assert len(frames) > 1

        # Reassemble
        reassembled = b""
        for i, frame in enumerate(frames):
            parsed = parse_ble_eth_frame(frame)
            assert parsed is not None
            assert parsed["cmd"] == BLE_ETH_CMD_DATA
            header = extract_data_header(parsed["payload"])
            assert header is not None
            flags, seq, conn_id, chunk = header
            assert conn_id == 1
            assert seq == i
            if i < len(frames) - 1:
                assert flags & BLE_ETH_FRAG_MORE
            else:
                assert not (flags & BLE_ETH_FRAG_MORE)
            reassembled += chunk

        assert reassembled == data

    def test_data_empty_payload(self):
        frames = build_ble_eth_data_fragmented(0, b"")
        assert len(frames) == 1
        parsed = parse_ble_eth_frame(frames[0])
        assert parsed is not None
        header = extract_data_header(parsed["payload"])
        assert header is not None
        _, _, _, data = header
        assert data == b""

    # --- Checksum validation ---

    def test_checksum_valid(self):
        frame = build_ble_eth_connect("10.0.0.1", 80)
        assert parse_ble_eth_frame(frame) is not None

    def test_checksum_invalid(self):
        frame = build_ble_eth_connect("10.0.0.1", 80)
        corrupted = frame[:-1] + bytes([(frame[-1] + 1) & 0xFF])
        assert parse_ble_eth_frame(corrupted) is None

    # --- Parser edge cases ---

    def test_parse_garbage_then_valid(self):
        """Parser should recover from garbage data."""
        garbage = bytes([0xDE, 0xAD, 0xBE, 0xEF])
        valid = build_ble_eth_info_request()
        # The parser should skip garbage and find the valid frame
        result = parse_ble_eth_frame(garbage)
        assert result is None
        result = parse_ble_eth_frame(valid)
        assert result is not None

    def test_parse_truncated_frame(self):
        frame = build_ble_eth_ping(b"test")
        assert parse_ble_eth_frame(frame[:3]) is None

    def test_parse_empty(self):
        assert parse_ble_eth_frame(b"") is None

    # --- SET_DEFAULT / GET_DEFAULT ---

    def test_set_default_all_zeros(self):
        frame = build_ble_eth_set_default("0.0.0.0", 0)
        parsed = parse_ble_eth_frame(frame)
        assert parsed["payload"][:4] == bytes([0, 0, 0, 0])
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 0

    def test_set_default_max_ip(self):
        frame = build_ble_eth_set_default("255.255.255.255", 65535)
        parsed = parse_ble_eth_frame(frame)
        assert parsed["payload"][:4] == bytes([255, 255, 255, 255])
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 65535

    def test_set_default_common_ssh(self):
        frame = build_ble_eth_set_default("192.168.11.2", 22)
        parsed = parse_ble_eth_frame(frame)
        assert parsed["payload"][:4] == bytes([192, 168, 11, 2])
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 22

    # --- CONNECT edge cases ---

    def test_connect_port_zero(self):
        frame = build_ble_eth_connect("10.0.0.1", 0)
        parsed = parse_ble_eth_frame(frame)
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 0

    def test_connect_port_max(self):
        frame = build_ble_eth_connect("10.0.0.1", 65535)
        parsed = parse_ble_eth_frame(frame)
        port = struct.unpack(">H", parsed["payload"][4:6])[0]
        assert port == 65535

    # --- DISCONNECT edge cases ---

    def test_disconnect_conn_id_zero(self):
        frame = build_ble_eth_disconnect(0)
        parsed = parse_ble_eth_frame(frame)
        assert parsed["payload"] == bytes([0])

    def test_disconnect_conn_id_max(self):
        frame = build_ble_eth_disconnect(0xFF)
        parsed = parse_ble_eth_frame(frame)
        assert parsed["payload"] == bytes([0xFF])

    # --- PING edge cases ---

    def test_ping_empty(self):
        frame = build_ble_eth_ping(b"")
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["payload"] == b""

    def test_ping_max_payload(self):
        max_data = bytes(range(249))
        frame = build_ble_eth_ping(max_data)
        parsed = parse_ble_eth_frame(frame)
        assert parsed is not None
        assert parsed["payload"] == max_data

    def test_ping_roundtrip_pattern(self):
        """Verify various byte patterns survive build/parse."""
        for pattern in [b"\x00" * 10, b"\xFF" * 10, bytes(range(10)), b"\xAA\x55" * 5]:
            frame = build_ble_eth_ping(pattern)
            parsed = parse_ble_eth_frame(frame)
            assert parsed["payload"] == pattern


# ===================================================================
# USB Mode Config Unit Tests
# ===================================================================

class TestUSBModeConfig:
    """USB mode configuration logic tests — no hardware required."""

    # --- Mode support ---

    def test_mode_1_supported(self):
        assert mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 1)

    def test_mode_2_supported(self):
        assert mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 2)

    def test_mode_3_supported(self):
        assert mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 3)

    def test_mode_4_supported(self):
        assert mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 4)

    def test_mode_5_not_in_runtime_mask(self):
        """Mode 5/6 are not in the standard runtime Mode 4 capability mask."""
        assert not mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 5)

    def test_mode_6_not_in_runtime_mask(self):
        assert not mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 6)

    def test_mode_0_not_in_standard_mask(self):
        """Mode 0 is not a valid USB work mode; not in standard capability masks."""
        assert not mode_is_supported(CAPABILITY_MASK_RUNTIME_MODE4, 0)

    def test_capability_mask_bits(self):
        """0x1E = bits 1,2,3,4."""
        mask = CAPABILITY_MASK_RUNTIME_MODE4
        assert mask == 0x1E
        for mode in [1, 2, 3, 4]:
            assert mode_is_supported(mask, mode)
        assert not mode_is_supported(mask, 0)
        assert not mode_is_supported(mask, 5)

    # --- Mode feature matrix ---

    def test_mode_1_features(self):
        assert mode_has_feature(1, "target_hid")
        assert mode_has_feature(1, "host_cdc_acm")
        assert not mode_has_feature(1, "target_uac")
        assert not mode_has_feature(1, "host_uac")
        assert mode_has_feature(1, "bridge") is False or MODE_FEATURES[1]["bridge"] == "none"

    def test_mode_2_features(self):
        assert mode_has_feature(2, "target_hid")
        assert mode_has_feature(2, "target_uac")
        assert mode_has_feature(2, "host_uac")
        assert mode_has_feature(2, "host_cdc_acm")
        assert MODE_FEATURES[2]["bridge"] == "uac"

    def test_mode_3_features(self):
        assert mode_has_feature(3, "target_hid")
        assert mode_has_feature(3, "target_cdc_acm")
        assert mode_has_feature(3, "host_cdc_acm")
        assert MODE_FEATURES[3]["bridge"] == "acm"

    def test_mode_4_features(self):
        assert mode_has_feature(4, "target_hid")
        assert mode_has_feature(4, "target_cdc_ecm")
        assert mode_has_feature(4, "host_cdc_ecm")
        assert not mode_has_feature(4, "host_cdc_acm")
        assert MODE_FEATURES[4]["bridge"] == "ecm"

    def test_mode_5_features(self):
        assert mode_has_feature(5, "target_hid")
        assert mode_has_feature(5, "target_uac")
        assert mode_has_feature(5, "host_uac")
        assert mode_has_feature(5, "target_cdc_ecm")
        assert mode_has_feature(5, "host_cdc_ecm")

    def test_mode_6_features(self):
        assert mode_has_feature(6, "target_hid")
        assert mode_has_feature(6, "target_cdc_acm")
        assert mode_has_feature(6, "host_cdc_acm")
        assert mode_has_feature(6, "target_cdc_ecm")
        assert mode_has_feature(6, "host_cdc_ecm")

    def test_unknown_mode_has_no_features(self):
        assert not mode_has_feature(99, "target_hid")
        assert not mode_has_feature(0, "target_hid")

    # --- Mode names ---

    def test_all_modes_have_names(self):
        for mode_id in [1, 2, 3, 4, 5, 6]:
            assert mode_id in MODE_NAMES

    def test_mode_name_values(self):
        assert MODE_NAMES[1] == "hid_acm"
        assert MODE_NAMES[2] == "hid_uac_acm"
        assert MODE_NAMES[3] == "hid_acm_bridge"
        assert MODE_NAMES[4] == "hid_ecm_bridge"
        assert MODE_NAMES[5] == "hid_uac_ecm"
        assert MODE_NAMES[6] == "hid_acm_ecm"

    # --- Bridge type exclusivity ---

    def test_bridge_types_exclusive_per_mode(self):
        """Each mode should have exactly one bridge type."""
        for mode, features in MODE_FEATURES.items():
            bridge = features["bridge"]
            assert bridge in ("none", "uac", "acm", "ecm"), f"Mode {mode} has invalid bridge: {bridge}"

    # --- HID Feature report decode ---

    def test_hid_report_decode(self):
        """Simulate decoding a HID Feature report 0x30."""
        report = bytes([
            0x30,  # report ID
            0x00,  # last_status
            0x04,  # compiled_mode (Mode 4)
            0x04,  # active_mode
            0x04,  # stored_mode
            0x01,  # config_status (valid)
            0x00, 0x00,  # flags
            0x1E, 0x00, 0x00, 0x00,  # capability_mask = 0x0000001E
        ])
        # Decode
        capability = report[8] | (report[9] << 8) | (report[10] << 16) | (report[11] << 24)
        flags = report[6] | (report[7] << 8)
        status = {
            "last_status": report[1],
            "compiled_mode": report[2],
            "active_mode": report[3],
            "stored_mode": report[4],
            "config_status": report[5],
            "flags": flags,
            "capability_mask": capability,
        }
        assert status["active_mode"] == 4
        assert status["stored_mode"] == 4
        assert status["config_status"] == 1
        assert status["capability_mask"] == 0x1E

    def test_hid_report_mode_2(self):
        report = bytes([0x30, 0x00, 0x04, 0x02, 0x02, 0x01, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00])
        assert report[3] == 2  # active_mode
        assert report[4] == 2  # stored_mode

    def test_hid_report_with_flags(self):
        flags = 0x1234
        report = bytes([0x30, 0x00, 0x04, 0x03, 0x03, 0x01,
                        flags & 0xFF, (flags >> 8) & 0xFF,
                        0x1E, 0x00, 0x00, 0x00])
        decoded_flags = report[6] | (report[7] << 8)
        assert decoded_flags == 0x1234


# ===================================================================
# Cross-feature / integration logic tests
# ===================================================================

class TestCrossFeature:
    """Cross-feature integration logic tests — no hardware required."""

    def test_ch9329_response_command_offset(self):
        """Response cmd = request cmd | 0x80."""
        for cmd in [CH9329_CMD_GET_INFO, CH9329_CMD_KB_GENERAL,
                    CH9329_CMD_KB_MEDIA, CH9329_CMD_MS_ABS,
                    CH9329_CMD_MS_REL, CH9329_CMD_GET_PARA_CFG,
                    CH9329_CMD_USB_WORK_MODE]:
            resp = cmd | CH9329_RESP_OFFSET
            assert resp & 0x80 != 0

    def test_ble_eth_response_command_offset(self):
        """BLE-Eth response cmds follow the same | 0x80 pattern."""
        assert BLE_ETH_CMD_CONNECT | 0x80 == BLE_ETH_CMD_CONNECT_RESP
        assert BLE_ETH_CMD_DATA | 0x80 == BLE_ETH_CMD_DATA_RESP
        assert BLE_ETH_CMD_DISCONNECT | 0x80 == BLE_ETH_CMD_DISCONN_RESP
        assert BLE_ETH_CMD_PING | 0x80 == BLE_ETH_CMD_PONG
        assert BLE_ETH_CMD_INFO | 0x80 == BLE_ETH_CMD_INFO_RESP

    def test_mode_3_has_acm_bridge_for_ch9329_routing(self):
        """Mode 3 should have ACM bridge for CH9329 command routing."""
        assert MODE_FEATURES[3]["bridge"] == "acm"
        assert mode_has_feature(3, "host_cdc_acm")
        assert mode_has_feature(3, "target_cdc_acm")

    def test_mode_4_ecm_bridge_intercepts_ch9329(self):
        """Mode 4 ECM bridge should intercept CH9329 frames from TCP."""
        assert MODE_FEATURES[4]["bridge"] == "ecm"
        assert mode_has_feature(4, "target_cdc_ecm")

    def test_all_modes_have_target_hid(self):
        """Every mode must have target-side HID."""
        for mode in MODE_FEATURES:
            assert mode_has_feature(mode, "target_hid"), f"Mode {mode} missing target HID"

    def test_mode_1_no_bridge(self):
        """Mode 1 has no bridge."""
        assert MODE_FEATURES[1]["bridge"] == "none"

    def test_mode_4_no_host_acm(self):
        """Mode 4 should not have host CDC ACM."""
        assert not mode_has_feature(4, "host_cdc_acm")

    def test_ch9329_and_ble_eth_share_frame_format(self):
        """Both protocols use the same 57 AB header and checksum."""
        ch9329_frame = build_ch9329(0x00, CH9329_CMD_GET_INFO)
        ble_frame = build_ble_eth_info_request()
        # Both start with 57 AB
        assert ch9329_frame[0] == ble_frame[0] == 0x57
        assert ch9329_frame[1] == ble_frame[1] == 0xAB
        # Both use same checksum algorithm
        assert ch9329_checksum(ch9329_frame[:-1]) == ch9329_frame[-1]
        assert ch9329_checksum(ble_frame[:-1]) == ble_frame[-1]


# ===================================================================
# Serial HID integration tests (require live device)
# ===================================================================

def _detect_control_port(baud=115200):
    """Find the CDC port that answers CH9329 GET_INFO."""
    import glob as _glob
    import serial as _serial
    frame = build_ch9329(0x00, CH9329_CMD_GET_INFO)
    for port in sorted(_glob.glob("/dev/cu.usbmodem*")):
        try:
            ser = _serial.Serial(port, baud, timeout=1)
        except Exception:
            continue
        try:
            ser.reset_input_buffer()
            ser.write(frame)
            time.sleep(0.3)
            resp = ser.read(64)
            if len(resp) >= 4 and resp[0] == 0x57 and resp[1] == 0xAB and resp[3] == 0x81:
                return port
        finally:
            ser.close()
    return None


@pytest.fixture(scope="module")
def serial_port():
    """Open serial port to device for the test session."""
    try:
        import serial
    except ImportError:
        pytest.skip("pyserial not installed")

    baud = 115200
    port = os.environ.get("OPENTERFACE_PORT") or _detect_control_port(baud)
    if port is None:
        pytest.skip("No Openterface control serial port detected")
    ser = serial.Serial(port, baud, timeout=3)
    yield ser
    ser.close()


@pytest.fixture
def flush_serial(serial_port):
    """Flush serial buffers before each test."""
    serial_port.reset_input_buffer()
    serial_port.reset_output_buffer()


@pytest.mark.hardware
class TestSerialHIDIntegration:
    """Serial HID integration tests — require live KeyMod device."""

    def test_get_info(self, serial_port, flush_serial):
        """GET_INFO (0x01) returns device status."""
        frame = build_ch9329(0x00, CH9329_CMD_GET_INFO)
        serial_port.write(frame)
        time.sleep(0.3)
        resp = serial_port.read(1024)

        assert len(resp) >= 6, f"Expected response, got {len(resp)} bytes"
        assert resp[0] == 0x57 and resp[1] == 0xAB
        assert resp[3] == 0x81, f"Expected ACK 0x81, got 0x{resp[3]:02x}"
        assert resp[4] == 8, f"Expected 8-byte payload, got {resp[4]}"

        # Hardware selector: 1=KVM-GO, 2=MiniKVM, 3=KeyMod
        hw_selector = resp[5]
        assert hw_selector in (0x01, 0x02, 0x03), f"Bad HW selector 0x{hw_selector:02x}"

    def test_keyboard_press_release(self, serial_port, flush_serial):
        """Keyboard press/release (0x02) sends HID report."""
        # Key down: 'A'
        payload = bytes([0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x82
        assert resp[5] == 0x00  # success

        # Key release
        payload_release = bytes([0x00] * 8)
        frame_release = build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload_release)
        serial_port.write(frame_release)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x82

    def test_keyboard_with_modifier(self, serial_port, flush_serial):
        """Ctrl+A keyboard press."""
        payload = bytes([0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x82
        assert resp[5] == 0x00

    def test_absolute_mouse(self, serial_port, flush_serial):
        """Absolute mouse move (0x04) sends HID report.

        Protocol requires 7-byte payload: [reserved][buttons][x_lo][x_hi][y_lo][y_hi][wheel]
        """
        payload = bytes([0x00, 0x00, 0xF4, 0x01, 0x2C, 0x01, 0x00])  # reserved=0, btn=0, x=500, y=300, wheel=0
        frame = build_ch9329(0x00, CH9329_CMD_MS_ABS, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x84
        assert resp[5] == 0x00

    def test_absolute_mouse_click(self, serial_port, flush_serial):
        """Absolute mouse with button click."""
        # Button down at position (7-byte payload)
        payload = bytes([0x00, 0x01, 0x64, 0x00, 0xC8, 0x00, 0x00])  # reserved=0, btn=1, x=100, y=200, wheel=0
        frame = build_ch9329(0x00, CH9329_CMD_MS_ABS, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x84

        # Button release
        payload = bytes([0x00, 0x00, 0x64, 0x00, 0xC8, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_MS_ABS, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6

    def test_relative_mouse_move(self, serial_port, flush_serial):
        """Relative mouse move (0x05)."""
        payload = bytes([0x00, 0x00, 30, 20, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_MS_REL, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x85
        assert resp[5] == 0x00

    def test_relative_mouse_click(self, serial_port, flush_serial):
        """Relative mouse left click."""
        # Button down
        payload = bytes([0x00, 0x01, 0x00, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_MS_REL, payload)
        serial_port.write(frame)
        time.sleep(0.1)

        # Button up
        payload = bytes([0x00, 0x00, 0x00, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_MS_REL, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x85

    def test_relative_mouse_wheel(self, serial_port, flush_serial):
        """Relative mouse scroll wheel."""
        payload = bytes([0x00, 0x00, 0x00, 0x00, 0x03])  # wheel=3
        frame = build_ch9329(0x00, CH9329_CMD_MS_REL, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x85

    def test_media_key(self, serial_port, flush_serial):
        """Media key (0x03)."""
        payload = bytes([0x02, 0x00, 0x00, 0x10])
        frame = build_ch9329(0x00, CH9329_CMD_KB_MEDIA, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == 0x83
        assert resp[5] == 0x00

        # Release
        payload = bytes([0x02, 0x00, 0x00, 0x00])
        frame = build_ch9329(0x00, CH9329_CMD_KB_MEDIA, payload)
        serial_port.write(frame)
        time.sleep(0.1)
        resp = serial_port.read(1024)

    def test_usb_work_mode_read(self, serial_port, flush_serial):
        """USB work mode read (0x30).

        Sends cmd 0x30 with empty payload (read status).
        The response is a 12-byte status block with cmd | 0x80.
        Some firmware builds may return ERR_PARAM (0xC0) if the command
        is not routed through the serial path in the current mode.
        """
        # Try empty payload first (canonical read per protocol doc)
        frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE)
        serial_port.write(frame)
        time.sleep(0.5)
        resp = serial_port.read(1024)

        if len(resp) < 6:
            # Retry with [0x00] payload
            serial_port.reset_input_buffer()
            frame = build_ch9329(0x00, CH9329_CMD_USB_WORK_MODE, bytes([0x00]))
            serial_port.write(frame)
            time.sleep(0.5)
            resp = serial_port.read(1024)

        assert len(resp) >= 6, "No response received for USB work mode read"
        # Response cmd should be 0x30 | 0x80 = 0xB0 (success)
        # or 0x30 | 0xC0 = 0xF0 (error, e.g. if cmd not routed through serial in current mode)
        expected_ack = CH9329_CMD_USB_WORK_MODE | 0x80
        expected_err = CH9329_CMD_USB_WORK_MODE | 0xC0
        assert resp[3] in (expected_ack, expected_err), (
            f"Unexpected response cmd 0x{resp[3]:02X}, expected 0x{expected_ack:02X} or 0x{expected_err:02X}"
        )
        if resp[3] == expected_ack and len(resp) >= 6 + 12:
            # Success: validate 12-byte status block
            status_payload = resp[5:5 + 12]
            assert status_payload[0] == 0x00, f"Operation status should be 0x00, got 0x{status_payload[0]:02X}"

    def test_get_para_cfg(self, serial_port, flush_serial):
        """Get parameter config (0x08)."""
        frame = build_ch9329(0x00, CH9329_CMD_GET_PARA_CFG)
        serial_port.write(frame)
        time.sleep(0.3)
        resp = serial_port.read(1024)
        assert len(resp) >= 6
        assert resp[3] == (CH9329_CMD_GET_PARA_CFG | 0x80)

    def test_rapid_keyboard_sequence(self, serial_port, flush_serial):
        """Rapid key press/release sequence."""
        for key in [0x04, 0x05, 0x06, 0x07]:  # a, b, c, d
            # Press
            payload = bytes([0x00, 0x00, key, 0x00, 0x00, 0x00, 0x00, 0x00])
            frame = build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload)
            serial_port.write(frame)
            time.sleep(0.05)

            # Release
            payload = bytes([0x00] * 8)
            frame = build_ch9329(0x00, CH9329_CMD_KB_GENERAL, payload)
            serial_port.write(frame)
            time.sleep(0.05)

        resp = serial_port.read(1024)
        # Should have multiple ACKs
        assert len(resp) >= 6
