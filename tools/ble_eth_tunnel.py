"""BLE-to-Ethernet tunneling protocol — Python implementation.

Matches the firmware implementation in APP/ble_eth_protocol.c and
APP/include/ble_eth_protocol.h.  Uses the Bleak library for BLE
communication.

Protocol frame format (CH9329-compatible):

    +------+------+-------+------+--------+----------+------+
    | 57   | AB   | Addr  | Cmd  | PayLen | Payload  | CSum |
    +------+------+-------+------+--------+----------+------+

Commands:
    0x10  CONNECT       – establish TCP tunnel (ip:port)
    0x11  DATA          – fragmented TCP payload
    0x12  DISCONNECT    – close tunnel
    0x1F  INFO          – device capabilities

Responses use cmd | 0x80 (success) or | 0xC0 (error).
"""

from __future__ import annotations

import asyncio
import logging
import struct
import time
from collections import deque
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable, Optional

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants (must match ble_eth_protocol.h)
# ---------------------------------------------------------------------------
FRAME_HEAD1 = 0x57
FRAME_HEAD2 = 0xAB
FRAME_HEAD_LEN = 5
MAX_PAYLOAD_LEN = 249

CMD_CONNECT = 0x10
CMD_DATA = 0x11
CMD_DISCONNECT = 0x12
CMD_INFO = 0x1F
CMD_PING = 0x1E
CMD_SET_DEFAULT = 0x20
CMD_GET_DEFAULT = 0x21
CMD_SET_ECM_IP = 0x22
CMD_GET_ECM_IP = 0x23

CMD_CONNECT_RESP = 0x90
CMD_DATA_RESP = 0x91
CMD_DATA_PUSH = 0x91
CMD_DISCONN_RESP = 0x92
CMD_CONN_CLOSED = 0xD2
CMD_INFO_RESP = 0x9F
CMD_PONG = 0x9E
CMD_ERROR = 0xD0
CMD_SET_DEFAULT_RESP = 0xA0
CMD_GET_DEFAULT_RESP = 0xA1
CMD_SET_ECM_IP_RESP = 0xA2
CMD_GET_ECM_IP_RESP = 0xA3

CMD_NAMES = {
    CMD_CONNECT: "CONNECT",
    CMD_DATA: "DATA",
    CMD_DISCONNECT: "DISCONNECT",
    CMD_INFO: "INFO",
    CMD_PING: "PING",
    CMD_SET_DEFAULT: "SET_DEFAULT",
    CMD_GET_DEFAULT: "GET_DEFAULT",
    CMD_SET_ECM_IP: "SET_ECM_IP",
    CMD_GET_ECM_IP: "GET_ECM_IP",
    CMD_CONNECT_RESP: "CONNECT_RESP",
    CMD_DATA_PUSH: "DATA_PUSH",
    CMD_DISCONN_RESP: "DISCONNECT_RESP",
    CMD_CONN_CLOSED: "CONN_CLOSED",
    CMD_INFO_RESP: "INFO_RESP",
    CMD_PONG: "PONG",
    CMD_SET_DEFAULT_RESP: "SET_DEFAULT_RESP",
    CMD_GET_DEFAULT_RESP: "GET_DEFAULT_RESP",
    CMD_SET_ECM_IP_RESP: "SET_ECM_IP_RESP",
    CMD_GET_ECM_IP_RESP: "GET_ECM_IP_RESP",
    CMD_ERROR: "ERROR",
}

STATUS_SUCCESS = 0x00
STATUS_TIMEOUT = 0xE1
STATUS_PARAM_ERR = 0xE5
STATUS_FRAME_ERR = 0xE6
STATUS_CONN_FULL = 0xE7

FEAT_TCP_TUNNEL = 0x01
FEAT_SSH_OPT = 0x02
FEAT_DATA_ACK_META = 0x04
FEAT_DATA_ACK_WINDOW = 0x08

FRAG_MORE = 0x80
FRAG_FIRST = 0x40
FRAG_COUNT_MASK = 0x0F
FRAG_HEADER_LEN = 3
SAFE_BLE_STREAM_CHUNK = 180

MAX_FRAG_DATA = MAX_PAYLOAD_LEN - FRAG_HEADER_LEN  # 246
MAX_REASSEMBLED = MAX_FRAG_DATA * 6  # fits one full-size TCP segment in firmware
# Per-message cap used when splitting outbound data. Must stay below the
# firmware TCP MSS (1460): the firmware forwards one reassembled message as
# one TCP segment, and a segment beyond MTU 1500 gets dropped on the wire.
MAX_MESSAGE_LEN = 240 * 6  # 1440 bytes, still exactly 6 fragments

# BLE GATT UUIDs (16-bit, used with Nordic UUID base)
SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
TX_CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"   # notify (device → host)
RX_CHAR_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"   # write  (host → device)


def _device_matches(discovered, name_hint: str) -> bool:
    """Return True when a discovered BLE device likely is an Openterface target."""
    aliases = {"kvm-go", "keymod", "openterface"}
    hint = (name_hint or "").strip().lower()
    if hint:
        aliases.add(hint)

    name = (getattr(discovered, "name", "") or "").lower()
    if any(alias in name for alias in aliases):
        return True

    metadata = getattr(discovered, "metadata", {}) or {}
    service_uuids = metadata.get("uuids") or metadata.get("service_uuids") or []
    normalized = {str(uuid).lower() for uuid in service_uuids}
    if SERVICE_UUID.lower() in normalized or "0000fff0" in " ".join(normalized):
        return True

    return False


# ---------------------------------------------------------------------------
# Frame builder
# ---------------------------------------------------------------------------

def _checksum(data: bytes) -> int:
    return sum(data) & 0xFF


def _short_hex(data: bytes, limit: int = 24) -> str:
    if not data:
        return "-"
    head = data[:limit].hex(" ")
    if len(data) > limit:
        return f"{head} ...(+{len(data) - limit})"
    return head


def _cmd_name(cmd: int) -> str:
    return CMD_NAMES.get(cmd, f"0x{cmd:02X}")


def _describe_payload(cmd: int, payload: bytes) -> str:
    if cmd in (CMD_DATA, CMD_DATA_PUSH) and len(payload) >= FRAG_HEADER_LEN:
        flags = payload[0]
        total = flags & FRAG_COUNT_MASK
        if total == 0:
            total = 1
        return (
            f"conn={payload[2]} seq={payload[1]}/{total} "
            f"first={1 if flags & FRAG_FIRST else 0} "
            f"more={1 if flags & FRAG_MORE else 0} data_len={len(payload) - FRAG_HEADER_LEN}"
        )
    if cmd == CMD_CONNECT_RESP and len(payload) >= 2:
        return f"conn={payload[0]} status=0x{payload[1]:02X}"
    if cmd in (CMD_DISCONN_RESP, CMD_CONN_CLOSED) and payload:
        status = f" status=0x{payload[1]:02X}" if len(payload) >= 2 else ""
        return f"conn={payload[0]}{status}"
    if cmd == CMD_INFO_RESP and len(payload) >= 8:
        max_payload = (payload[2] << 8) | payload[3]
        return f"version={payload[0]} max_conn={payload[1]} max_payload={max_payload} features=0x{payload[4]:02X}"
    return f"payload_len={len(payload)} head={_short_hex(payload)}"


def build_frame(addr: int, cmd: int, payload: bytes = b"") -> bytes:
    """Build a BLE-Eth frame."""
    frame = bytes([FRAME_HEAD1, FRAME_HEAD2, addr & 0xFF, cmd & 0xFF, len(payload)])
    frame += payload
    frame += bytes([_checksum(frame)])
    return frame


def build_connect(remote_ip: str, remote_port: int) -> bytes:
    """Build CONNECT frame."""
    ip_bytes = bytes(int(x) for x in remote_ip.split("."))
    payload = ip_bytes + struct.pack("!H", remote_port)
    return build_frame(0x00, CMD_CONNECT, payload)


def build_disconnect(conn_id: int) -> bytes:
    """Build DISCONNECT frame."""
    return build_frame(0x00, CMD_DISCONNECT, bytes([conn_id]))


def build_data_single(conn_id: int, data: bytes) -> Optional[bytes]:
    """Build a single (unfragmented) DATA frame. Returns None if too large."""
    if len(data) > MAX_FRAG_DATA:
        return None
    payload = bytes([FRAG_FIRST, 0x00, conn_id]) + data
    return build_frame(0x00, CMD_DATA, payload)


def build_data_fragmented(conn_id: int, data: bytes) -> list[bytes]:
    """Build fragmented DATA frames. Returns list of frames."""
    total_frags = (len(data) + MAX_FRAG_DATA - 1) // MAX_FRAG_DATA
    if total_frags > 15:
        raise ValueError(f"Data too large: {len(data)} bytes (> {MAX_REASSEMBLED})")

    frames = []
    offset = 0
    for seq in range(total_frags):
        chunk = data[offset:offset + MAX_FRAG_DATA]
        offset += len(chunk)

        flags = total_frags & FRAG_COUNT_MASK
        if seq == 0:
            flags |= FRAG_FIRST
        if seq < total_frags - 1:
            flags |= FRAG_MORE

        payload = bytes([flags, seq, conn_id]) + chunk
        frames.append(build_frame(0x00, CMD_DATA, payload))

    return frames


def build_info_request() -> bytes:
    """Build INFO request frame."""
    return build_frame(0x00, CMD_INFO, b"")


def build_ping(data: bytes = b"") -> bytes:
    """Build PING request frame (echo test, no network needed)."""
    return build_frame(0x00, CMD_PING, data)


def build_set_default(remote_ip: str, remote_port: int) -> bytes:
    """Build SET_DEFAULT frame to store default target IP/port."""
    ip_bytes = bytes(int(x) for x in remote_ip.split("."))
    payload = ip_bytes + struct.pack("!H", remote_port)
    return build_frame(0x00, CMD_SET_DEFAULT, payload)


def build_get_default() -> bytes:
    """Build GET_DEFAULT frame to retrieve default target IP/port."""
    return build_frame(0x00, CMD_GET_DEFAULT, b"")


def _ipv4_bytes(ip: str) -> bytes:
    parts = [int(x) for x in ip.split(".")]
    if len(parts) != 4 or any(not (0 <= p <= 255) for p in parts):
        raise ValueError(f"Invalid IPv4 address: {ip!r}")
    return bytes(parts)


ECM_IP_GET_ALL = 0x00
ECM_IP_GET_HOST = 0x01
ECM_IP_GET_TARGET = 0x02
ECM_IP_GET_STATUS = 0x03
# 0x05: target re-enumeration / DHCP diagnostics (0x04 is unused for GET).
ECM_IP_GET_DIAG = 0x05

ECM_IP_SET_ALL = 0x00
ECM_IP_SET_HOST = 0x01
ECM_IP_SET_TARGET = 0x02
# Reset uses 0x04, not 0x03: 0x03 is CH9329 bond-count on the same command byte.
ECM_IP_SET_RESET = 0x04


def build_get_ecm_ip(sub_cmd: int) -> bytes:
    """Build GET_ECM_IP frame for sub_cmd 0x00..0x03 and 0x05."""
    return build_frame(0x00, CMD_GET_ECM_IP, bytes([sub_cmd]))


def build_set_ecm_ip_all(
    host_ip: str,
    dev_ip: str,
    subnet_mask: str,
    target_host_ip: str,
    target_dev_ip: str,
) -> bytes:
    """Build SET_ECM_IP (all addresses) frame."""
    payload = (
        bytes([ECM_IP_SET_ALL])
        + _ipv4_bytes(host_ip)
        + _ipv4_bytes(dev_ip)
        + _ipv4_bytes(subnet_mask)
        + _ipv4_bytes(target_host_ip)
        + _ipv4_bytes(target_dev_ip)
    )
    return build_frame(0x00, CMD_SET_ECM_IP, payload)


def build_set_ecm_ip_host(host_ip: str, dev_ip: str, subnet_mask: str) -> bytes:
    """Build SET_ECM_IP (host side) frame."""
    payload = (
        bytes([ECM_IP_SET_HOST])
        + _ipv4_bytes(host_ip)
        + _ipv4_bytes(dev_ip)
        + _ipv4_bytes(subnet_mask)
    )
    return build_frame(0x00, CMD_SET_ECM_IP, payload)


def build_set_ecm_ip_target(target_host_ip: str, target_dev_ip: str) -> bytes:
    """Build SET_ECM_IP (target side) frame."""
    payload = (
        bytes([ECM_IP_SET_TARGET])
        + _ipv4_bytes(target_host_ip)
        + _ipv4_bytes(target_dev_ip)
    )
    return build_frame(0x00, CMD_SET_ECM_IP, payload)


def build_set_ecm_ip_reset() -> bytes:
    """Build SET_ECM_IP (reset to factory defaults) frame."""
    return build_frame(0x00, CMD_SET_ECM_IP, bytes([ECM_IP_SET_RESET]))


# ---------------------------------------------------------------------------
# Frame parser
# ---------------------------------------------------------------------------

@dataclass
class ParsedFrame:
    addr: int
    cmd: int
    payload: bytes


class FrameParser:
    """State machine parser for BLE-Eth frames.

    Feed raw bytes via feed(); completed frames are placed in the
    received_frames deque.
    """

    def __init__(self, lenient: bool = False, trace: bool = False):
        self._state = 0  # 0..6 = wait head1, head2, addr, cmd, len, payload, csum
        self._buf = bytearray()
        self._payload_len = 0
        self._checksum = 0
        self._lenient = lenient
        self._trace = trace
        self.received_frames: deque[ParsedFrame] = deque()

    def feed(self, data: bytes):
        for byte in data:
            self._process(byte)

    def _process(self, b: int):
        if self._state == 0:  # wait head1
            if b == FRAME_HEAD1:
                self._buf = bytearray([b])
                self._checksum = b
                self._state = 1
        elif self._state == 1:  # wait head2
            if b == FRAME_HEAD2:
                self._buf.append(b)
                self._checksum += b
                self._state = 2
            else:
                if b == FRAME_HEAD1:
                    self._buf = bytearray([b])
                    self._checksum = b
                    self._state = 1
                else:
                    self._state = 0
        elif self._state == 2:  # wait addr
            self._buf.append(b)
            self._checksum += b
            self._state = 3
        elif self._state == 3:  # wait cmd
            self._buf.append(b)
            self._checksum += b
            self._state = 4
        elif self._state == 4:  # wait len
            self._buf.append(b)
            self._checksum += b
            self._payload_len = b
            if self._payload_len == 0:
                self._state = 6  # skip to checksum
            elif self._payload_len > MAX_PAYLOAD_LEN:
                self._state = 0  # invalid, resync
            else:
                self._state = 5
        elif self._state == 5:  # wait payload
            self._buf.append(b)
            self._checksum += b
            if len(self._buf) >= FRAME_HEAD_LEN + self._payload_len:
                self._state = 6
        elif self._state == 6:  # wait checksum
            expected = self._checksum & 0xFF
            cmd = self._buf[3]
            payload = bytes(self._buf[5:5 + self._payload_len])
            if b == expected:
                frame = ParsedFrame(
                    addr=self._buf[2],
                    cmd=cmd,
                    payload=payload,
                )
                self.received_frames.append(frame)
                if self._trace:
                    logger.info(
                        "TRACE RX frame ok addr=0x%02X cmd=%s len=%d checksum=0x%02X %s",
                        self._buf[2],
                        _cmd_name(cmd),
                        self._payload_len,
                        b,
                        _describe_payload(cmd, payload),
                    )
                else:
                    logger.debug(f"Frame parsed: cmd=0x{cmd:02X} len={self._payload_len}")
            elif self._lenient:
                if cmd == CMD_DATA_PUSH:
                    logger.warning(
                        "Checksum mismatch on DATA frame: expected 0x%02X, got 0x%02X (dropping) addr=0x%02X cmd=%s len=%d %s",
                        expected,
                        b,
                        self._buf[2],
                        _cmd_name(cmd),
                        self._payload_len,
                        _describe_payload(cmd, payload),
                    )
                else:
                    logger.warning(
                        "Checksum mismatch: expected 0x%02X, got 0x%02X (accepting control frame) addr=0x%02X cmd=%s len=%d %s",
                        expected,
                        b,
                        self._buf[2],
                        _cmd_name(cmd),
                        self._payload_len,
                        _describe_payload(cmd, payload),
                    )
                    frame = ParsedFrame(
                        addr=self._buf[2],
                        cmd=cmd,
                        payload=payload,
                    )
                    self.received_frames.append(frame)
            else:
                logger.warning(f"Checksum mismatch: expected 0x{self._checksum:02X}, got 0x{b:02X}")
            self._state = 0


# ---------------------------------------------------------------------------
# Reassembler
# ---------------------------------------------------------------------------

@dataclass
class ReassembledData:
    conn_id: int
    data: bytes


class DataReassembler:
    """Reassembles fragmented DATA frames into complete payloads."""

    def __init__(self, trace: bool = False):
        self._active = False
        self._conn_id = 0
        self._total_frags = 0
        self._next_seq = 0
        self._buf = bytearray()
        self._trace = trace

    def feed(self, payload: bytes) -> Optional[ReassembledData]:
        """Process a DATA frame payload (including 3-byte fragment header).
        Returns ReassembledData when reassembly is complete, None otherwise.
        """
        if len(payload) < FRAG_HEADER_LEN:
            return None

        flags = payload[0]
        seq = payload[1]
        conn_id = payload[2]
        frag_data = payload[FRAG_HEADER_LEN:]

        total_frags = flags & FRAG_COUNT_MASK
        if total_frags == 0:
            total_frags = 1

        if self._trace:
            logger.info(
                "TRACE RX fragment conn=%d seq=%d/%d first=%d more=%d frag_len=%d active=%d expected_seq=%d",
                conn_id,
                seq,
                total_frags,
                1 if flags & FRAG_FIRST else 0,
                1 if flags & FRAG_MORE else 0,
                len(frag_data),
                1 if self._active else 0,
                self._next_seq,
            )

        if flags & FRAG_FIRST:
            self._active = True
            self._conn_id = conn_id
            self._total_frags = total_frags
            self._next_seq = 0
            self._buf = bytearray()

        if not self._active:
            return None

        if seq != self._next_seq:
            logger.warning(
                "Out of order fragment: conn=%d expected=%d got=%d total=%d flags=0x%02X",
                conn_id,
                self._next_seq,
                seq,
                total_frags,
                flags,
            )
            self._active = False
            return None

        self._buf.extend(frag_data)
        self._next_seq += 1

        if not (flags & FRAG_MORE):
            # Last fragment
            self._active = False
            if self._trace:
                logger.info(
                    "TRACE RX reassembled conn=%d bytes=%d frags=%d head=%s",
                    self._conn_id,
                    len(self._buf),
                    self._total_frags,
                    _short_hex(bytes(self._buf)),
                )
            return ReassembledData(conn_id=self._conn_id, data=bytes(self._buf))

        return None


# ---------------------------------------------------------------------------
# High-level BLE-Eth tunnel client
# ---------------------------------------------------------------------------

class ConnectionState(IntEnum):
    DISCONNECTED = 0
    CONNECTING = 1
    CONNECTED = 2
    ERROR = 3


@dataclass
class TunnelEvent:
    """Event from the tunnel to the application."""
    class Type(IntEnum):
        CONNECTED = 0
        DISCONNECTED = 1
        DATA_RECEIVED = 2
        ERROR = 3

    type: Type
    conn_id: int = 0
    data: bytes = b""
    error_msg: str = ""


@dataclass
class PendingDataAck:
    conn_id: int
    seq: int
    frame: bytes
    waiter: asyncio.Future
    attempts: int


class BleEthClient:
    """High-level BLE-Eth tunnel client.

    Usage::

        client = BleEthClient("KVM-GO")
        async with client.connect():
            conn_id = await client.open_tunnel("192.168.1.100", 22)
            await client.send_data(conn_id, b"hello")
            data = await client.recv_data(conn_id)
    """

    def __init__(
        self,
        device_name: str = "KVM-GO",
        ble_address: Optional[str] = None,
        max_retries: int = 3,
        trace_frames: bool = False,
        data_ack_policy: str = "feature",
        strict_data_ack_window_size: int = 2,
    ):
        self.device_name = device_name
        self.ble_address = ble_address
        self.max_retries = max_retries
        self.trace_frames = trace_frames

        self._bleak_client = None
        self._parser = FrameParser(lenient=True, trace=trace_frames)
        self._reassembler = DataReassembler(trace=trace_frames)
        self._tx_frame_count = 0
        self._rx_notify_count = 0
        # Serialize host->device writes so multi-frame sends from concurrent
        # tasks (data path + control path) cannot interleave at chunk granularity.
        self._tx_lock = asyncio.Lock()
        # Keep no-response writes as default for this device path; response
        # writes were observed to destabilize the BLE link during SSH bursts.
        self._write_response_supported: bool = False
        # Tiny pacing between completed DATA frames to reduce burst overruns.
        self._data_frame_gap_s = 0.001

        # Event queues keyed by conn_id
        self._data_queues: dict[int, asyncio.Queue] = {}
        self._connect_futures: dict[int, asyncio.Future] = {}
        self._disconnect_futures: dict[int, asyncio.Future] = {}

        # State
        self._state = ConnectionState.DISCONNECTED
        self._open_conn_ids: set[int] = set()
        self._connected_event = asyncio.Event()
        self._event_callbacks: list[Callable[[TunnelEvent], None]] = []

        # Notification buffer accumulator
        self._notify_buffer = bytearray()

        # Info response holder (separate from the main frame queue)
        self._info_response: Optional[dict] = None
        self._info_event: Optional[asyncio.Event] = None

        # Ping response holder
        self._pong_response: Optional[bytes] = None
        self._pong_event: Optional[asyncio.Event] = None

        # Default target configuration futures
        self._default_futures: dict[str, asyncio.Future] = {}

        # ECM IP configuration futures ("get" / "set")
        self._ecm_ip_futures: dict[str, asyncio.Future] = {}

        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._disconnecting = False
        # Reliability-hook counters for DATA response telemetry.
        self._reliability_stats = {
            "data_resp_total": 0,
            "data_resp_success": 0,
            "data_resp_error": 0,
            "data_resp_with_meta": 0,
            "last_data_resp_status": None,
            "last_data_resp_conn_id": None,
            "last_data_resp_seq": None,
            "last_data_resp_expected_seq": None,
            "last_data_resp_rx_window": None,
            "data_ack_cumulative_resolved": 0,
            "tx_data_ack_wait": 0,
            "tx_data_ack_timeout": 0,
            "tx_data_ack_non_success": 0,
            "tx_data_retransmit": 0,
            "tx_data_ack_fallback_legacy": 0,
        }
        if data_ack_policy not in ("legacy", "feature", "meta"):
            raise ValueError(
                f"Invalid data_ack_policy='{data_ack_policy}', expected one of: legacy, feature, meta"
            )
        self._data_ack_policy = data_ack_policy
        # Sender-side ACK tracking for reliability baseline (window=1 behavior).
        # Start in legacy mode for compatibility with firmware that only sends
        # end-of-message ACKs, and auto-upgrade to metadata mode when observed.
        self._ack_mode = "legacy"  # unknown | meta | legacy
        self._ack_strict = False
        self._data_ack_waiters: dict[tuple[int, int], asyncio.Future] = {}
        self._data_ack_timeout_s = 0.35
        self._data_ack_retries = 2
        self._data_ack_window_size = 1
        self._data_ack_window_limit: Optional[int] = None
        self._strict_ack_timeout_s = 1.0
        self._strict_ack_retries = 4
        if not (1 <= int(strict_data_ack_window_size) <= 15):
            raise ValueError("strict_data_ack_window_size must be in [1, 15]")
        self._strict_data_ack_window_size = int(strict_data_ack_window_size)
        self._strict_data_frame_gap_s = 0.005
        if self._data_ack_policy == "meta":
            self._ack_mode = "meta"
            self._ack_strict = True
            self._data_ack_window_size = self._strict_data_ack_window_size

    @property
    def is_connected(self) -> bool:
        return bool(
            self._state == ConnectionState.CONNECTED
            and self._bleak_client
            and self._bleak_client.is_connected
        )

    @staticmethod
    def _set_future_exception_if_pending(future: Optional[asyncio.Future], exc: Exception) -> None:
        if future and not future.done():
            future.set_exception(exc)

    def _abort_pending_operations(self, reason: str) -> None:
        error = ConnectionError(reason)

        self._set_future_exception_if_pending(self._connect_futures.pop("pending", None), error)

        for conn_id, future in list(self._disconnect_futures.items()):
            self._set_future_exception_if_pending(future, error)
            self._disconnect_futures.pop(conn_id, None)

        for key, future in list(self._data_ack_waiters.items()):
            self._set_future_exception_if_pending(future, error)
            self._data_ack_waiters.pop(key, None)

        self._abort_ecm_ip_requests(error)

    def _handle_ble_disconnect(self):
        if self._disconnecting:
            self._state = ConnectionState.DISCONNECTED
            self._connected_event.clear()
            self._open_conn_ids.clear()
            return

        self._state = ConnectionState.DISCONNECTED
        self._connected_event.clear()
        self._open_conn_ids.clear()
        self._abort_pending_operations("BLE disconnected")
        self._emit_event(
            TunnelEvent(
                type=TunnelEvent.Type.ERROR,
                error_msg="BLE disconnected",
            )
        )
        logger.warning("BLE link disconnected unexpectedly.")

    def _ble_disconnected_callback(self, _client):
        if self._loop is not None:
            self._loop.call_soon_threadsafe(self._handle_ble_disconnect)
        else:
            self._handle_ble_disconnect()

    async def connect(self) -> "BleEthClient":
        """Connect to the BLE device."""
        from bleak import BleakClient
        from bleak import BleakScanner

        if self.ble_address:
            device = await BleakScanner.find_device_by_address(
                self.ble_address, timeout=10
            )
        else:
            devices = await BleakScanner.discover(timeout=5)
            device = None
            for discovered in devices:
                if _device_matches(discovered, self.device_name):
                    device = discovered
                    break

            if device is None:
                for discovered in devices:
                    if discovered.name:
                        logger.info(f"Found BLE device: {discovered.name} ({discovered.address})")

        if device is None:
            raise ConnectionError(
                f"BLE device '{self.device_name}' not found. "
                "Make sure it is advertising and nearby."
            )

        logger.info(f"Connecting to {device.name} ({device.address})...")
        self._loop = asyncio.get_running_loop()
        self._disconnecting = False
        self._bleak_client = BleakClient(
            device,
            services=[SERVICE_UUID],
            disconnected_callback=self._ble_disconnected_callback,
        )
        await self._bleak_client.connect()
        logger.info("BLE connected.")

        await self._bleak_client.start_notify(TX_CHAR_UUID, self._notification_handler)

        self._state = ConnectionState.CONNECTED
        self._connected_event.set()

        return self

    async def disconnect(self):
        """Disconnect from the BLE device."""
        self._disconnecting = True
        if self._bleak_client and self._bleak_client.is_connected:
            try:
                await self._bleak_client.stop_notify(TX_CHAR_UUID)
            except Exception:
                pass
            await self._bleak_client.disconnect()
        self._state = ConnectionState.DISCONNECTED
        self._connected_event.clear()
        self._open_conn_ids.clear()
        logger.info("BLE disconnected.")

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *args):
        await self.disconnect()

    async def get_info(self) -> dict:
        """Get device info/capabilities."""
        self._info_response = None
        self._info_event = asyncio.Event()

        await self._send_frame(build_info_request())

        # Wait for response (up to 5 seconds)
        try:
            await asyncio.wait_for(self._info_event.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            raise TimeoutError("No info response received")

        info = self._info_response
        self._info_response = None
        self._info_event = None
        return info

    async def ping(self, data: bytes = b"hello", timeout: float = 5.0) -> bytes:
        """Send a PING and wait for PONG echo response.
        Tests the BLE data path without requiring network connectivity.
        """
        self._pong_response = None
        self._pong_event = asyncio.Event()

        await self._send_frame(build_ping(data))

        try:
            await asyncio.wait_for(self._pong_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise TimeoutError("No PONG response received")

        pong = self._pong_response
        self._pong_response = None
        self._pong_event = None
        return pong

    async def set_default_target(self, remote_ip: str, remote_port: int, timeout: float = 5.0) -> bool:
        """Set the default target IP/port for auto-connect on BLE link-up.

        Args:
            remote_ip: Target IP address (e.g., "192.168.1.100")
            remote_port: Target port (e.g., 22)
            timeout: Response timeout in seconds

        Returns:
            True if successfully set, False otherwise
        """
        if not self.is_connected:
            raise ConnectionError("Not connected to BLE device")

        future = asyncio.get_event_loop().create_future()
        self._default_futures["set"] = future

        await self._send_frame(build_set_default(remote_ip, remote_port))

        try:
            success = await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            self._default_futures.pop("set", None)
            raise TimeoutError("SET_DEFAULT timed out")

        return success

    async def get_default_target(self, timeout: float = 5.0) -> dict:
        """Get the currently configured default target IP/port.

        Args:
            timeout: Response timeout in seconds

        Returns:
            Dict with 'ip', 'port', and 'enabled' keys
        """
        if not self.is_connected:
            raise ConnectionError("Not connected to BLE device")

        future = asyncio.get_event_loop().create_future()
        self._default_futures["get"] = future

        await self._send_frame(build_get_default())

        try:
            result = await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            self._default_futures.pop("get", None)
            raise TimeoutError("GET_DEFAULT timed out")

        return result

    def _abort_ecm_ip_requests(self, exc: Exception) -> None:
        for key in ("get", "set"):
            future = self._ecm_ip_futures.pop(key, None)
            self._set_future_exception_if_pending(future, exc)

    async def get_ecm_ip(
        self, sub_cmd: int = ECM_IP_GET_ALL, timeout: float = 5.0
    ) -> dict:
        """Read the ECM IP configuration.

        Returns a dict with 'status' plus, depending on sub_cmd:
            0x00 all:    host, dev, mask, target_host, target_dev
            0x01 host:   host, dev, mask
            0x02 target: target_host, target_dev
            0x03 status: host_active, target_active
            0x05 diag:   'diag' dict with detach_dp_level, reenum_count,
                         bus_reset_count, dhcp_count, dhcp_nak_count,
                         target_active, host_active
        """
        if not self.is_connected:
            raise ConnectionError("Not connected to BLE device")

        future = asyncio.get_running_loop().create_future()
        self._ecm_ip_futures["get"] = future
        await self._send_frame(build_get_ecm_ip(sub_cmd))

        try:
            payload = await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            self._ecm_ip_futures.pop("get", None)
            raise TimeoutError("GET_ECM_IP timed out")

        status = payload[0] if payload else STATUS_FRAME_ERR
        result: dict = {"status": status}

        def ip_at(offset: int) -> str:
            return f"{payload[offset]}.{payload[offset + 1]}.{payload[offset + 2]}.{payload[offset + 3]}"

        if status == STATUS_SUCCESS:
            if len(payload) >= 21:
                # Older firmware always answers with the full layout.
                result.update(
                    host=ip_at(1),
                    dev=ip_at(5),
                    mask=ip_at(9),
                    target_host=ip_at(13),
                    target_dev=ip_at(17),
                )
            elif sub_cmd == ECM_IP_GET_HOST and len(payload) >= 13:
                result.update(host=ip_at(1), dev=ip_at(5), mask=ip_at(9))
            elif sub_cmd == ECM_IP_GET_TARGET and len(payload) >= 9:
                result.update(target_host=ip_at(1), target_dev=ip_at(5))
            elif sub_cmd == ECM_IP_GET_STATUS and len(payload) >= 3:
                result.update(host_active=bool(payload[1]), target_active=bool(payload[2]))
            elif sub_cmd == ECM_IP_GET_DIAG and len(payload) >= 12:
                result["diag"] = {
                    "detach_dp_level": payload[1],
                    "reenum_count": int.from_bytes(payload[2:4], "little"),
                    "bus_reset_count": int.from_bytes(payload[4:6], "little"),
                    "dhcp_count": int.from_bytes(payload[6:8], "little"),
                    "dhcp_nak_count": int.from_bytes(payload[8:10], "little"),
                    "target_active": bool(payload[10]),
                    "host_active": bool(payload[11]),
                }

        return result

    async def _set_ecm_ip_frame(self, frame: bytes, timeout: float) -> int:
        if not self.is_connected:
            raise ConnectionError("Not connected to BLE device")

        future = asyncio.get_running_loop().create_future()
        self._ecm_ip_futures["set"] = future
        await self._send_frame(frame)

        try:
            return await asyncio.wait_for(future, timeout=timeout)
        except asyncio.TimeoutError:
            self._ecm_ip_futures.pop("set", None)
            raise TimeoutError("SET_ECM_IP timed out")

    async def set_ecm_ip_all(
        self,
        host_ip: str,
        dev_ip: str,
        subnet_mask: str,
        target_host_ip: str,
        target_dev_ip: str,
        timeout: float = 5.0,
    ) -> int:
        """Set all ECM addresses. Returns the firmware status byte."""
        frame = build_set_ecm_ip_all(
            host_ip, dev_ip, subnet_mask, target_host_ip, target_dev_ip
        )
        return await self._set_ecm_ip_frame(frame, timeout)

    async def set_ecm_ip_host(
        self, host_ip: str, dev_ip: str, subnet_mask: str, timeout: float = 5.0
    ) -> int:
        """Set the host-side ECM addresses. Returns the firmware status byte."""
        return await self._set_ecm_ip_frame(
            build_set_ecm_ip_host(host_ip, dev_ip, subnet_mask), timeout
        )

    async def set_ecm_ip_target(
        self, target_host_ip: str, target_dev_ip: str, timeout: float = 5.0
    ) -> int:
        """Set the target-side ECM addresses. Returns the firmware status byte."""
        return await self._set_ecm_ip_frame(
            build_set_ecm_ip_target(target_host_ip, target_dev_ip), timeout
        )

    async def reset_ecm_ip(self, timeout: float = 5.0) -> int:
        """Reset the ECM IP configuration to factory defaults."""
        return await self._set_ecm_ip_frame(build_set_ecm_ip_reset(), timeout)

    async def open_tunnel(
        self, remote_ip: str, remote_port: int, timeout: float = 10.0
    ) -> int:
        """Open a TCP tunnel to remote_ip:remote_port.
        Returns the connection ID.
        """
        if not self.is_connected:
            raise ConnectionError("Not connected to BLE device")

        conn_id_future = asyncio.get_event_loop().create_future()
        self._connect_futures["pending"] = conn_id_future

        await self._send_frame(build_connect(remote_ip, remote_port))

        try:
            result = await asyncio.wait_for(conn_id_future, timeout=timeout)
        except asyncio.TimeoutError:
            self._connect_futures.pop("pending", None)
            raise TimeoutError(f"CONNECT to {remote_ip}:{remote_port} timed out")

        if result["status"] != STATUS_SUCCESS:
            raise ConnectionError(
                f"CONNECT failed: status=0x{result['status']:02X}"
            )

        conn_id = result["conn_id"]
        self._data_queues.setdefault(conn_id, asyncio.Queue())
        self._open_conn_ids.add(conn_id)
        logger.info(f"Tunnel opened: conn_id={conn_id} -> {remote_ip}:{remote_port}")
        return conn_id

    async def close_tunnel(self, conn_id: int, timeout: float = 5.0, force: bool = False):
        """Close a TCP tunnel."""
        if (not force) and conn_id not in self._open_conn_ids and conn_id not in self._disconnect_futures:
            logger.debug(f"close_tunnel ignored for already-closed conn_id={conn_id}")
            return

        existing_future = self._disconnect_futures.get(conn_id)
        if existing_future is not None:
            # A close is already in progress (or has completed but cleanup hasn't run yet).
            try:
                if not existing_future.done():
                    await asyncio.wait_for(existing_future, timeout=timeout)
            except asyncio.TimeoutError:
                logger.warning(f"DISCONNECT timeout for conn_id={conn_id}")

            self._data_queues.pop(conn_id, None)
            self._open_conn_ids.discard(conn_id)
            logger.info(f"Tunnel closed: conn_id={conn_id}")
            return

        disc_future = asyncio.get_event_loop().create_future()
        self._disconnect_futures[conn_id] = disc_future

        await self._send_frame(build_disconnect(conn_id))

        try:
            await asyncio.wait_for(disc_future, timeout=timeout)
        except asyncio.TimeoutError:
            logger.warning(f"DISCONNECT timeout for conn_id={conn_id}")

        self._data_queues.pop(conn_id, None)
        self._disconnect_futures.pop(conn_id, None)
        self._open_conn_ids.discard(conn_id)
        logger.info(f"Tunnel closed: conn_id={conn_id}")

    async def send_data(self, conn_id: int, data: bytes):
        """Send data through the tunnel (auto-fragmentation)."""
        if self.trace_frames:
            logger.info(
                "TRACE TX stream conn=%d bytes=%d head=%s",
                conn_id,
                len(data),
                _short_hex(data),
            )
        if len(data) > MAX_MESSAGE_LEN:
            for offset in range(0, len(data), MAX_MESSAGE_LEN):
                await self.send_data(conn_id, data[offset:offset + MAX_MESSAGE_LEN])
            return

        if len(data) <= MAX_FRAG_DATA:
            frame = build_data_single(conn_id, data)
            if frame:
                await self._send_data_frame_with_ack(frame)
        else:
            frames = build_data_fragmented(conn_id, data)
            if self.trace_frames:
                logger.info("TRACE TX fragmented conn=%d bytes=%d frames=%d", conn_id, len(data), len(frames))
            if self._ack_mode != "legacy" and self._data_ack_window_size > 1:
                await self._send_data_frames_windowed(frames)
            else:
                for frame in frames:
                    await self._send_data_frame_with_ack(frame)

    async def recv_data(self, conn_id: int, timeout: float = 30.0) -> bytes:
        """Receive data from the tunnel."""
        queue = self._data_queues.get(conn_id)
        if queue is None:
            raise ValueError(f"No tunnel for conn_id={conn_id}")
        data = await asyncio.wait_for(queue.get(), timeout=timeout)
        return data

    def add_event_callback(self, callback: Callable[[TunnelEvent], None]):
        """Register a callback for tunnel events."""
        self._event_callbacks.append(callback)

    def _emit_event(self, event: TunnelEvent):
        for cb in self._event_callbacks:
            try:
                cb(event)
            except Exception as e:
                logger.error(f"Event callback error: {e}")

    def get_reliability_stats(self) -> dict:
        out = dict(self._reliability_stats)
        out["ack_mode"] = self._ack_mode
        out["ack_policy"] = self._data_ack_policy
        out["ack_strict"] = self._ack_strict
        out["ack_window_size"] = self._data_ack_window_size
        out["ack_window_limit"] = self._data_ack_window_limit
        out["ack_waiters"] = len(self._data_ack_waiters)
        return out

    def apply_device_features(self, features: int) -> None:
        self._data_ack_window_limit = None

        if self._data_ack_policy == "legacy":
            self._ack_mode = "legacy"
            self._ack_strict = False
            self._data_frame_gap_s = 0.001
            self._data_ack_window_size = 1
            self._write_response_supported = False
            logger.info("DATA ACK policy=legacy; metadata ACK waiting disabled")
            return

        if self._data_ack_policy == "meta":
            self._ack_mode = "meta"
            self._ack_strict = True
            self._data_ack_timeout_s = self._strict_ack_timeout_s
            self._data_ack_retries = self._strict_ack_retries
            self._data_ack_window_size = self._strict_data_ack_window_size
            self._data_frame_gap_s = self._strict_data_frame_gap_s
            self._write_response_supported = False
            logger.info("DATA ACK policy=meta; strict metadata ACK waiting enabled")
            return

        if features & FEAT_DATA_ACK_META:
            # Feature-gated strict mode.
            self._ack_mode = "meta"
            self._ack_strict = True
            self._data_ack_timeout_s = self._strict_ack_timeout_s
            self._data_ack_retries = self._strict_ack_retries
            self._data_ack_window_size = self._strict_data_ack_window_size
            self._data_frame_gap_s = self._strict_data_frame_gap_s
            if features & FEAT_DATA_ACK_WINDOW:
                self._data_ack_window_limit = self._strict_data_ack_window_size
            self._write_response_supported = False
            logger.info(
                "DATA ACK policy=feature; metadata ACK feature detected (features=0x%02X), strict mode enabled",
                features,
            )
        else:
            self._ack_mode = "legacy"
            self._ack_strict = False
            self._data_frame_gap_s = 0.001
            self._data_ack_timeout_s = 0.35
            self._data_ack_retries = 2
            self._data_ack_window_size = 1
            self._write_response_supported = False
            logger.info(
                "DATA ACK policy=feature; metadata ACK feature missing (features=0x%02X), using legacy mode",
                features,
            )

    def _effective_data_ack_window(self) -> int:
        window = self._data_ack_window_size
        if self._data_ack_window_limit is not None:
            window = min(window, self._data_ack_window_limit)
        return max(1, window)

    async def _register_and_send_data_ack_frame(self, pending: PendingDataAck) -> None:
        self._data_ack_waiters[(pending.conn_id, pending.seq)] = pending.waiter
        await self._send_frame(pending.frame)
        self._reliability_stats["tx_data_ack_wait"] += 1

    async def _await_pending_data_ack(self, pending: PendingDataAck) -> None:
        key = (pending.conn_id, pending.seq)
        for attempt in range(1, pending.attempts + 1):
            try:
                ack = await asyncio.wait_for(pending.waiter, timeout=self._data_ack_timeout_s)
                if isinstance(ack, dict) and int(ack.get("status", STATUS_SUCCESS)) != STATUS_SUCCESS:
                    self._reliability_stats["tx_data_ack_non_success"] += 1
                    if attempt < pending.attempts:
                        self._reliability_stats["tx_data_retransmit"] += 1
                        logger.warning(
                            "DATA ACK non-success conn=%d seq=%d status=0x%02X expected=%s, retransmit %d/%d",
                            pending.conn_id,
                            pending.seq,
                            int(ack.get("status", STATUS_SUCCESS)),
                            ack.get("expected_seq"),
                            attempt,
                            pending.attempts,
                        )
                        pending.waiter = asyncio.get_event_loop().create_future()
                        await self._register_and_send_data_ack_frame(pending)
                        continue
                    if self._ack_strict:
                        raise TimeoutError(
                            f"DATA ACK non-success conn={pending.conn_id} seq={pending.seq} status=0x{int(ack.get('status', STATUS_SUCCESS)):02X}"
                        )
                    self._ack_mode = "legacy"
                    self._reliability_stats["tx_data_ack_fallback_legacy"] += 1
                    logger.warning(
                        "DATA ACK non-success conn=%d seq=%d after %d attempts; fallback to legacy mode",
                        pending.conn_id,
                        pending.seq,
                        pending.attempts,
                    )
                    return
                return
            except asyncio.TimeoutError:
                self._reliability_stats["tx_data_ack_timeout"] += 1
                self._data_ack_waiters.pop(key, None)

                if attempt < pending.attempts:
                    self._reliability_stats["tx_data_retransmit"] += 1
                    logger.warning(
                        "DATA ACK timeout conn=%d seq=%d, retransmit %d/%d",
                        pending.conn_id,
                        pending.seq,
                        attempt,
                        pending.attempts,
                    )
                    pending.waiter = asyncio.get_event_loop().create_future()
                    await self._register_and_send_data_ack_frame(pending)
                    continue

                if self._ack_strict:
                    raise TimeoutError(
                        f"DATA ACK timeout conn={pending.conn_id} seq={pending.seq} after {pending.attempts} attempts"
                    )

                self._ack_mode = "legacy"
                self._reliability_stats["tx_data_ack_fallback_legacy"] += 1
                logger.warning(
                    "DATA ACK timeout conn=%d seq=%d after %d attempts; fallback to legacy mode",
                    pending.conn_id,
                    pending.seq,
                    pending.attempts,
                )
                return

    async def _send_data_frames_windowed(self, frames: list[bytes]) -> None:
        pending_queue: list[PendingDataAck] = []

        for frame in frames:
            while len(pending_queue) >= self._effective_data_ack_window():
                await self._await_pending_data_ack(pending_queue.pop(0))

            payload_len = frame[4] if len(frame) >= FRAME_HEAD_LEN else 0
            payload = frame[5:5 + payload_len] if len(frame) >= FRAME_HEAD_LEN else b""
            header = self._extract_data_header(payload)
            if header is None:
                await self._send_frame(frame)
                continue

            conn_id, seq = header
            pending = PendingDataAck(
                conn_id=conn_id,
                seq=seq,
                frame=frame,
                waiter=asyncio.get_event_loop().create_future(),
                attempts=self._data_ack_retries + 1,
            )
            await self._register_and_send_data_ack_frame(pending)
            pending_queue.append(pending)

        while pending_queue:
            await self._await_pending_data_ack(pending_queue.pop(0))

    @staticmethod
    def _extract_data_header(payload: bytes) -> tuple[int, int] | None:
        if len(payload) < FRAG_HEADER_LEN:
            return None
        return int(payload[2]), int(payload[1])

    @staticmethod
    def _looks_like_data_ack(payload: bytes) -> bool:
        if not payload:
            return False
        status = payload[0]
        # ACK status values are 0x00 and 0xE* in current protocol.
        return status == STATUS_SUCCESS or (status & 0xF0) == 0xE0

    async def _send_data_frame_with_ack(self, frame: bytes):
        payload_len = frame[4] if len(frame) >= FRAME_HEAD_LEN else 0
        payload = frame[5:5 + payload_len] if len(frame) >= FRAME_HEAD_LEN else b""
        header = self._extract_data_header(payload)
        if header is None:
            await self._send_frame(frame)
            return
        conn_id, seq = header

        if self._ack_mode == "legacy":
            await self._send_frame(frame)
            return

        pending = PendingDataAck(
            conn_id=conn_id,
            seq=seq,
            frame=frame,
            waiter=asyncio.get_event_loop().create_future(),
            attempts=self._data_ack_retries + 1,
        )
        await self._register_and_send_data_ack_frame(pending)
        await self._await_pending_data_ack(pending)

    async def _send_frame(self, frame: bytes):
        """Send a frame to the device via BLE write."""
        if not self._bleak_client or not self._bleak_client.is_connected:
            raise ConnectionError("Not connected to BLE device")
        async with self._tx_lock:
            self._tx_frame_count += 1
            if self.trace_frames:
                cmd = frame[3] if len(frame) >= FRAME_HEAD_LEN else 0
                payload_len = frame[4] if len(frame) >= FRAME_HEAD_LEN else 0
                payload = frame[5:5 + payload_len] if len(frame) >= FRAME_HEAD_LEN else b""
                checksum = frame[5 + payload_len] if len(frame) > 5 + payload_len else None
                logger.info(
                    "TRACE TX frame#%d cmd=%s len=%d checksum=%s %s raw=%s",
                    self._tx_frame_count,
                    _cmd_name(cmd),
                    payload_len,
                    f"0x{checksum:02X}" if checksum is not None else "missing",
                    _describe_payload(cmd, payload),
                    _short_hex(frame),
                )
            else:
                logger.debug(f"TX frame: {frame.hex(' ')}")

            cmd = frame[3] if len(frame) >= FRAME_HEAD_LEN else 0

            for offset in range(0, len(frame), SAFE_BLE_STREAM_CHUNK):
                chunk = frame[offset:offset + SAFE_BLE_STREAM_CHUNK]
                await self._write_chunk(chunk)
                if len(frame) > SAFE_BLE_STREAM_CHUNK:
                    await asyncio.sleep(0.005)

            if cmd == CMD_DATA and self._data_frame_gap_s > 0:
                await asyncio.sleep(self._data_frame_gap_s)

    async def _write_chunk(self, chunk: bytes):
        """Write one BLE chunk with reliability-first behavior.

        Strict mode prefers write-with-response so the BLE stack can apply
        backpressure. If the stack rejects it, we fall back to no-response.
        """
        if not self._bleak_client:
            raise ConnectionError("Not connected to BLE device")

        if self._write_response_supported is False:
            try:
                await self._bleak_client.write_gatt_char(RX_CHAR_UUID, chunk, response=False)
            except Exception as exc:
                raise ConnectionError("BLE write failed") from exc
            return

        try:
            await self._bleak_client.write_gatt_char(RX_CHAR_UUID, chunk, response=True)
        except Exception as exc:
            self._write_response_supported = False
            if not self._bleak_client.is_connected:
                raise ConnectionError("BLE disconnected") from exc
            try:
                await self._bleak_client.write_gatt_char(RX_CHAR_UUID, chunk, response=False)
            except Exception as fallback_exc:
                raise ConnectionError("BLE write failed") from fallback_exc

    def _notification_handler(self, sender, data: bytes):
        """Handle BLE notifications (device → host)."""
        self._rx_notify_count += 1
        if self.trace_frames:
            logger.info(
                "TRACE RX notify#%d len=%d head=%s",
                self._rx_notify_count,
                len(data),
                _short_hex(bytes(data)),
            )
        else:
            logger.debug(f"RX BLE data ({len(data)} bytes): {bytes(data).hex(' ')}")
        self._parser.feed(data)

        # Process all parsed frames
        while self._parser.received_frames:
            frame = self._parser.received_frames.popleft()
            self._handle_frame(frame)

    def _handle_frame(self, frame: ParsedFrame):
        """Process a parsed frame."""
        cmd = frame.cmd
        payload = frame.payload

        if cmd == CMD_CONNECT_RESP:
            # Connect response
            if len(payload) >= 2:
                conn_id = payload[0]
                status = payload[1]
                future = self._connect_futures.pop("pending", None)
                # Avoid a race where server data arrives immediately after
                # CONNECT_RESP: ensure the per-conn RX queue exists before
                # any DATA frames for this conn_id are processed.
                if future is not None and status == STATUS_SUCCESS:
                    self._data_queues.setdefault(conn_id, asyncio.Queue())
                    self._open_conn_ids.add(conn_id)
                if future:
                    future.set_result({"conn_id": conn_id, "status": status})
                self._emit_event(TunnelEvent(
                    type=TunnelEvent.Type.CONNECTED,
                    conn_id=conn_id,
                    error_msg=f"status=0x{status:02X}" if status != STATUS_SUCCESS else "",
                ))
                if self.trace_frames:
                    logger.info("TRACE CONNECT response conn=%d status=0x%02X", conn_id, status)

        elif cmd == CMD_DATA_PUSH:
            # cmd 0x91 is shared by DATA_PUSH and DATA_RESP; demux by payload shape.
            if self._looks_like_data_ack(payload):
                self._reliability_stats["data_resp_total"] += 1
                status = payload[0] if len(payload) >= 1 else STATUS_SUCCESS
                if status == STATUS_SUCCESS:
                    self._reliability_stats["data_resp_success"] += 1
                else:
                    self._reliability_stats["data_resp_error"] += 1
                self._reliability_stats["last_data_resp_status"] = int(status)

                if len(payload) >= 4:
                    # Metadata ACK format: [status, conn_id, seq, expected_seq, rx_window?]
                    self._ack_mode = "meta"
                    ack_conn_id = int(payload[1])
                    ack_seq = int(payload[2])
                    ack_expected_seq = int(payload[3])
                    ack_rx_window = None
                    if len(payload) >= 5:
                        ack_rx_window = max(1, min(int(payload[4]), 15))
                        self._data_ack_window_limit = ack_rx_window

                    self._reliability_stats["data_resp_with_meta"] += 1
                    self._reliability_stats["last_data_resp_conn_id"] = ack_conn_id
                    self._reliability_stats["last_data_resp_seq"] = ack_seq
                    self._reliability_stats["last_data_resp_expected_seq"] = ack_expected_seq
                    self._reliability_stats["last_data_resp_rx_window"] = ack_rx_window

                    # Cumulative ACK handling: expected_seq is the next fragment index
                    # the device expects, implying all seq < expected_seq were received.
                    # This lets us safely unblock earlier waiters even if per-fragment ACK
                    # frames were dropped/delayed.
                    resolved = 0
                    if int(status) == STATUS_SUCCESS:
                        keys = [
                            (cid, seq)
                            for (cid, seq) in list(self._data_ack_waiters.keys())
                            if cid == ack_conn_id and seq < ack_expected_seq
                        ]
                        for key in keys:
                            waiter = self._data_ack_waiters.pop(key, None)
                            if waiter and not waiter.done():
                                waiter.set_result(
                                    {
                                        "status": status,
                                        "conn_id": ack_conn_id,
                                        "seq": key[1],
                                        "expected_seq": ack_expected_seq,
                                        "rx_window": ack_rx_window,
                                    }
                                )
                                resolved += 1

                        if resolved > 1:
                            self._reliability_stats["data_ack_cumulative_resolved"] += (resolved - 1)
                            if self.trace_frames:
                                logger.info(
                                    "DATA ACK cumulatively resolved %d waiters conn=%d expected=%d rx_window=%s",
                                    resolved,
                                    ack_conn_id,
                                    ack_expected_seq,
                                    ack_rx_window,
                                )

                    if resolved == 0:
                        waiter = self._data_ack_waiters.pop((ack_conn_id, ack_seq), None)
                        if waiter and not waiter.done():
                            if self.trace_frames:
                                logger.info(
                                    "DATA ACK matched waiter conn=%d seq=%d status=0x%02X expected=%d rx_window=%s",
                                    ack_conn_id,
                                    ack_seq,
                                    int(status),
                                    ack_expected_seq,
                                    ack_rx_window,
                                )
                            waiter.set_result(
                                {
                                    "status": status,
                                    "conn_id": ack_conn_id,
                                    "seq": ack_seq,
                                    "expected_seq": ack_expected_seq,
                                    "rx_window": ack_rx_window,
                                }
                            )
                        elif self._ack_strict:
                            if self.trace_frames:
                                logger.warning(
                                    "DATA ACK had no waiter conn=%d seq=%d status=0x%02X expected=%d rx_window=%s pending=%d",
                                    ack_conn_id,
                                    ack_seq,
                                    int(status),
                                    ack_expected_seq,
                                    ack_rx_window,
                                    len(self._data_ack_waiters),
                                )
                            else:
                                logger.debug(
                                    "DATA ACK had no waiter conn=%d seq=%d status=0x%02X expected=%d rx_window=%s pending=%d",
                                    ack_conn_id,
                                    ack_seq,
                                    int(status),
                                    ack_expected_seq,
                                    ack_rx_window,
                                    len(self._data_ack_waiters),
                                )
                elif len(payload) == 1:
                    if self._ack_strict:
                        logger.warning(
                            "Strict metadata ACK mode expected 4-byte ACK, got legacy 1-byte ACK"
                        )
                    else:
                        self._ack_mode = "legacy"

                logger.debug("DATA ACK received status=0x%02X len=%d", status, len(payload))
            else:
                # Incoming data (pushed from device)
                result = self._reassembler.feed(payload)
                if result:
                    queue = self._data_queues.get(result.conn_id)
                    if queue:
                        queue.put_nowait(result.data)
                    elif self.trace_frames:
                        logger.info("TRACE RX reassembled conn=%d dropped=no_queue bytes=%d", result.conn_id, len(result.data))
                    self._emit_event(TunnelEvent(
                        type=TunnelEvent.Type.DATA_RECEIVED,
                        conn_id=result.conn_id,
                        data=result.data,
                    ))

        elif cmd == CMD_DISCONN_RESP:
            # Disconnect response
            if len(payload) >= 2:
                conn_id = payload[0]
                future = self._disconnect_futures.pop(conn_id, None)
                self._open_conn_ids.discard(conn_id)
                if future:
                    future.set_result(True)
                self._emit_event(TunnelEvent(
                    type=TunnelEvent.Type.DISCONNECTED,
                    conn_id=conn_id,
                ))

        elif cmd == CMD_CONN_CLOSED:
            # Remote closed
            if len(payload) >= 1:
                conn_id = payload[0]
                future = self._disconnect_futures.pop(conn_id, None)
                if future and not future.done():
                    future.set_result(True)
                self._open_conn_ids.discard(conn_id)
                self._emit_event(TunnelEvent(
                    type=TunnelEvent.Type.DISCONNECTED,
                    conn_id=conn_id,
                ))

        elif cmd == CMD_INFO_RESP:
            if len(payload) >= 8:
                info = {
                    "version": payload[0],
                    "max_connections": payload[1],
                    "max_fragment_payload": (payload[2] << 8) | payload[3],
                    "features": payload[4],
                    "data_ack_window_supported": bool(payload[4] & FEAT_DATA_ACK_WINDOW),
                    "raw": payload,
                }
                self._info_response = info
                if self._info_event:
                    self._info_event.set()
            logger.info(f"Info response: {payload.hex(' ')}")

        elif cmd == CMD_PONG:
            logger.info(f"PONG received ({len(payload)} bytes): {payload.hex(' ')}")
            self._pong_response = payload
            if self._pong_event:
                self._pong_event.set()

        elif cmd == CMD_SET_DEFAULT_RESP:
            if len(payload) >= 1:
                status = payload[0]
                future = self._default_futures.get("set")
                if future and not future.done():
                    future.set_result(status == STATUS_SUCCESS)
                self._default_futures.pop("set", None)
                logger.info(f"SET_DEFAULT response: status=0x{status:02X}")

        elif cmd == CMD_GET_DEFAULT_RESP:
            if len(payload) >= 7:
                ip = f"{payload[0]}.{payload[1]}.{payload[2]}.{payload[3]}"
                port = (payload[4] << 8) | payload[5]
                enabled = bool(payload[6])
                future = self._default_futures.get("get")
                if future and not future.done():
                    future.set_result({"ip": ip, "port": port, "enabled": enabled})
                self._default_futures.pop("get", None)
                logger.info(f"GET_DEFAULT response: ip={ip} port={port} enabled={enabled}")

        elif cmd == CMD_SET_ECM_IP_RESP:
            status = payload[0] if payload else STATUS_FRAME_ERR
            future = self._ecm_ip_futures.pop("set", None)
            if future and not future.done():
                future.set_result(status)
            logger.info(f"SET_ECM_IP response: status=0x{status:02X}")

        elif cmd == CMD_GET_ECM_IP_RESP:
            future = self._ecm_ip_futures.pop("get", None)
            if future and not future.done():
                future.set_result(bytes(payload))
            logger.info(f"GET_ECM_IP response ({len(payload)} bytes): {payload.hex(' ')}")

        elif cmd == CMD_ERROR:
            self._emit_event(TunnelEvent(
                type=TunnelEvent.Type.ERROR,
                error_msg="Device returned error",
            ))


# ---------------------------------------------------------------------------
# BLE TCP proxy — bridges local TCP sockets through BLE tunnel
# ---------------------------------------------------------------------------

class BleTcpProxy:
    """Local TCP proxy that forwards connections through a BLE-Eth tunnel.

    Usage::

        proxy = BleTcpProxy(client, local_port=8022, target_ip="192.168.1.100", target_port=22)
        await proxy.start()
        # Now connect to localhost:8022 → forwarded through BLE → 192.168.1.100:22
        await proxy.stop()
    """

    def __init__(
        self,
        client: BleEthClient,
        local_port: int = 8022,
        target_ip: str = "192.168.1.100",
        target_port: int = 22,
        local_host: str = "127.0.0.1",
        recv_timeout: float = 30.0,
        max_connections: int = 0,
        connect_timeout: float = 10.0,
        connect_retries: int = 3,
        retry_delay: float = 0.35,
    ):
        self.client = client
        self.local_port = local_port
        self.target_ip = target_ip
        self.target_port = target_port
        self.local_host = local_host
        self.recv_timeout = recv_timeout
        self.max_connections = max_connections
        self.connect_timeout = connect_timeout
        self.connect_retries = max(1, connect_retries)
        self.retry_delay = max(0.0, retry_delay)

        self._server: Optional[asyncio.Server] = None
        self._active_connections: dict[asyncio.StreamWriter, int] = {}
        self._connection_lock = asyncio.Lock()

    async def _cleanup_stale_tunnels(self):
        """Try to release stale firmware-side tunnel slots before retrying CONNECT."""
        if self.max_connections <= 0:
            return

        for conn_id in range(self.max_connections):
            try:
                await self.client.close_tunnel(conn_id, timeout=0.5, force=True)
            except Exception:
                pass

    async def start(self):
        """Start the local TCP proxy server."""
        self._server = await asyncio.start_server(
            self._handle_local_connection,
            self.local_host,
            self.local_port,
        )
        addr = self._server.sockets[0].getsockname()
        logger.info(f"BLE TCP proxy listening on {addr[0]}:{addr[1]}")
        logger.info(f"  -> tunnel target: {self.target_ip}:{self.target_port}")

    async def stop(self):
        """Stop the proxy server and close all active connections."""
        active_conn_ids = set(self._active_connections.values())

        for writer in list(self._active_connections):
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

        for conn_id in active_conn_ids:
            if conn_id >= 0 and conn_id in self.client._open_conn_ids:
                try:
                    await self.client.close_tunnel(conn_id, timeout=1.0)
                except Exception:
                    pass

        self._active_connections.clear()

        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        logger.info("BLE TCP proxy stopped.")

    async def _handle_local_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ):
        """Handle a new local TCP connection."""
        peer = writer.get_extra_info("peername")
        logger.info(f"Local connection from {peer}")

        conn_id = -1
        async with self._connection_lock:
            try:
                # Open BLE tunnel
                last_exc: Exception | None = None
                for attempt in range(1, self.connect_retries + 1):
                    try:
                        conn_id = await self.client.open_tunnel(
                            self.target_ip,
                            self.target_port,
                            timeout=self.connect_timeout,
                        )
                        break
                    except Exception as exc:
                        last_exc = exc
                        text = str(exc).lower()
                        retryable = (
                            "0xe1" in text
                            or "status=0xe1" in text
                            or "0xe7" in text
                            or "status=0xe7" in text
                            or "timed out" in text
                        )
                        if retryable and attempt < self.connect_retries:
                            logger.warning(
                                "CONNECT failed on attempt %d/%d (%s); cleaning stale slots and retrying",
                                attempt,
                                self.connect_retries,
                                exc,
                            )
                            if self.max_connections > 0:
                                await self._cleanup_stale_tunnels()
                            if self.retry_delay > 0:
                                await asyncio.sleep(self.retry_delay)
                            continue
                        raise

                if conn_id < 0 and last_exc is not None:
                    raise last_exc
                self._active_connections[writer] = conn_id

                # Bidirectional forwarding
                local_to_ble = asyncio.create_task(
                    self._forward_local_to_ble(reader, conn_id)
                )
                ble_to_local = asyncio.create_task(
                    self._forward_ble_to_local(writer, conn_id)
                )

                done, pending = await asyncio.wait(
                    [local_to_ble, ble_to_local],
                    return_when=asyncio.FIRST_COMPLETED,
                )

                for task in pending:
                    task.cancel()
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass

            except Exception as e:
                logger.error(f"Proxy error: {e}")
                try:
                    writer.close()
                except Exception:
                    pass
            finally:
                if conn_id >= 0:
                    try:
                        await self.client.close_tunnel(conn_id)
                    except Exception:
                        pass
                self._active_connections.pop(writer, None)
                logger.info(f"Local connection from {peer} closed.")

    async def _forward_local_to_ble(
        self, reader: asyncio.StreamReader, conn_id: int
    ):
        """Forward data from local TCP -> BLE tunnel."""
        try:
            while True:
                data = await reader.read(MAX_REASSEMBLED)
                if not data:
                    break
                if self.client.trace_frames:
                    logger.info("TRACE TCP local->ble conn=%d bytes=%d head=%s", conn_id, len(data), _short_hex(data))
                await self.client.send_data(conn_id, data)
                logger.debug(f"Local->BLE: {len(data)} bytes")
        except (asyncio.CancelledError, ConnectionError):
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass

    async def _forward_ble_to_local(
        self, writer: asyncio.StreamWriter, conn_id: int
    ):
        """Forward data from BLE tunnel -> local TCP."""
        try:
            while True:
                data = await self.client.recv_data(conn_id, timeout=self.recv_timeout)
                if not data:
                    break
                if self.client.trace_frames:
                    logger.info("TRACE TCP ble->local conn=%d bytes=%d head=%s", conn_id, len(data), _short_hex(data))
                writer.write(data)
                await writer.drain()
                logger.debug(f"BLE->Local: {len(data)} bytes")
        except (asyncio.CancelledError, ConnectionError):
            pass
        except asyncio.TimeoutError:
            logger.warning(f"BLE recv timeout for conn_id={conn_id}")
        except (ConnectionResetError, BrokenPipeError):
            pass
