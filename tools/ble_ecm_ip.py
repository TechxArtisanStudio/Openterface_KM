#!/usr/bin/env python3
"""Read or change the ECM IP configuration over BLE.

Firmware commands used (BLE-Eth protocol, see docs/ble-eth-protocol.md):
    0x23 GET_ECM_IP  -> 0xA3
    0x22 SET_ECM_IP  -> 0xA2

Examples:
    python3 tools/ble_ecm_ip.py get
    python3 tools/ble_ecm_ip.py get --sub status
    python3 tools/ble_ecm_ip.py get --sub diag
    python3 tools/ble_ecm_ip.py set --target 192.168.20.2 192.168.20.5
    python3 tools/ble_ecm_ip.py set --host 192.168.20.1 192.168.20.3 255.255.255.0
    python3 tools/ble_ecm_ip.py set --all 192.168.20.1 192.168.20.3 255.255.255.0 192.168.20.2 192.168.20.5
    python3 tools/ble_ecm_ip.py reset
"""

from __future__ import annotations

import argparse
import asyncio
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.ble_eth_tunnel import (
    BleEthClient,
    ECM_IP_GET_ALL,
    ECM_IP_GET_DIAG,
    ECM_IP_GET_HOST,
    ECM_IP_GET_STATUS,
    ECM_IP_GET_TARGET,
    STATUS_SUCCESS,
)

GET_SUBCOMMANDS = {
    "all": ECM_IP_GET_ALL,
    "host": ECM_IP_GET_HOST,
    "target": ECM_IP_GET_TARGET,
    "status": ECM_IP_GET_STATUS,
    "diag": ECM_IP_GET_DIAG,
}


def describe_status(status: int) -> str:
    if status == STATUS_SUCCESS:
        return "OK"
    return f"ERROR 0x{status:02X}"


def print_get_result(result: dict) -> None:
    if "host" in result:
        print(f"  host        : {result['host']}")
        print(f"  dev         : {result['dev']}")
        print(f"  mask        : {result['mask']}")
    if "target_host" in result:
        print(f"  target_host : {result['target_host']}")
    if "target_dev" in result:
        print(f"  target_dev  : {result['target_dev']}")
    if "host_active" in result:
        print(f"  host ECM data if active   : {result['host_active']}")
        print(f"  target ECM data if active : {result['target_active']}")
    diag = result.get("diag")
    if diag:
        dp = diag["detach_dp_level"]
        dp_text = {
            0: "0 (target saw the disconnect)",
            1: "1 (D+ still high, no disconnect)",
            0xFF: "unknown (no re-enumeration since reset)",
        }.get(dp, str(dp))
        print(f"  detach D+ level : {dp_text}")
        print(f"  re-enum count   : {diag['reenum_count']}")
        print(f"  bus reset count : {diag['bus_reset_count']}")
        print(f"  DHCP requests   : {diag['dhcp_count']}")
        print(f"  DHCP NAKs       : {diag['dhcp_nak_count']}")
        print(f"  target ECM data if active : {diag['target_active']}")
        print(f"  host ECM data if active   : {diag['host_active']}")


async def run(args: argparse.Namespace) -> int:
    client = BleEthClient(
        device_name=args.name,
        ble_address=args.ble_addr,
        trace_frames=args.trace,
    )

    async with client.connect():
        if args.action == "get":
            sub = GET_SUBCOMMANDS[args.sub]
            print(f"GET_ECM_IP sub={args.sub}")
            result = await client.get_ecm_ip(sub)
            print(f"  status      : {describe_status(result['status'])}")
            print_get_result(result)
            return 0 if result["status"] == STATUS_SUCCESS else 1

        if args.action == "set":
            if args.all:
                status = await client.set_ecm_ip_all(*args.all)
                detail = "all"
            elif args.host:
                status = await client.set_ecm_ip_host(*args.host)
                detail = "host side"
            else:
                status = await client.set_ecm_ip_target(*args.target)
                detail = "target side"
            print(f"SET_ECM_IP ({detail}) -> {describe_status(status)}")
            return 0 if status == STATUS_SUCCESS else 1

        status = await client.reset_ecm_ip()
        print(f"SET_ECM_IP (reset) -> {describe_status(status)}")
        return 0 if status == STATUS_SUCCESS else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Read/change ECM IP config over BLE")
    parser.add_argument("--name", default="KVM-GO", help="BLE device name (default: KVM-GO)")
    parser.add_argument("--ble-addr", default=None, help="BLE address to connect to")
    parser.add_argument("--trace", action="store_true", help="Trace BLE-Eth frames")

    sub = parser.add_subparsers(dest="action", required=True)

    get_p = sub.add_parser("get", help="Read ECM IP configuration")
    get_p.add_argument(
        "--sub", choices=sorted(GET_SUBCOMMANDS), default="all", help="Which block to read"
    )

    set_p = sub.add_parser("set", help="Write ECM IP configuration")
    group = set_p.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--all",
        nargs=5,
        metavar=("HOST", "DEV", "MASK", "TARGET_HOST", "TARGET_DEV"),
        help="Set all addresses",
    )
    group.add_argument(
        "--host",
        nargs=3,
        metavar=("HOST", "DEV", "MASK"),
        help="Set host-side addresses",
    )
    group.add_argument(
        "--target",
        nargs=2,
        metavar=("TARGET_HOST", "TARGET_DEV"),
        help="Set target-side addresses",
    )

    sub.add_parser("reset", help="Reset to factory defaults")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return asyncio.run(run(args))
    except (ConnectionError, TimeoutError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
