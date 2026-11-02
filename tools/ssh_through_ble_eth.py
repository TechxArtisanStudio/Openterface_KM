#!/usr/bin/env python3
"""SSH through the Openterface BLE-Eth tunnel.

This command connects to the Openterface BLE device, starts a local TCP
proxy, then optionally launches an SSH client through that proxy.

Examples:
    # Start an interactive SSH session to 192.168.11.1:22 through BLE.
    python3 tools/ssh_through_ble_eth.py --user pi

    # Keep only the local proxy running, then SSH from another terminal.
    python3 tools/ssh_through_ble_eth.py --proxy-only
    ssh -p 8022 user@127.0.0.1

    # Verify the SSH banner and exit.
    python3 tools/ssh_through_ble_eth.py --test-banner
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys
from pathlib import Path

# Allow running this script directly from either the repo root or tools/.
REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.ble_eth_tunnel import BleEthClient, BleTcpProxy, TunnelEvent, ECM_IP_GET_ALL


LOG = logging.getLogger("ssh_through_ble_eth")

SAFE_SSH_OPTIONS = [
    "-o", "PreferredAuthentications=keyboard-interactive,password",
    "-o", "PubkeyAuthentication=no",
    "-o", "Compression=no",
    "-o", "KexAlgorithms=curve25519-sha256",
    "-o", "HostKeyAlgorithms=ssh-ed25519,rsa-sha2-512,rsa-sha2-256",
    "-o", "Ciphers=aes128-ctr",
    "-o", "MACs=hmac-sha2-256",
]


def _format_event(event: TunnelEvent) -> str:
    names = {
        TunnelEvent.Type.CONNECTED: "CONNECTED",
        TunnelEvent.Type.DISCONNECTED: "DISCONNECTED",
        TunnelEvent.Type.DATA_RECEIVED: "DATA",
        TunnelEvent.Type.ERROR: "ERROR",
    }
    name = names.get(event.type, f"type={event.type}")
    if event.type == TunnelEvent.Type.DATA_RECEIVED:
        return f"{name} conn={event.conn_id} bytes={len(event.data)}"
    if event.error_msg:
        return f"{name} conn={event.conn_id} {event.error_msg}"
    return f"{name} conn={event.conn_id}"


async def _read_ssh_banner(local_host: str, local_port: int, timeout: float) -> str:
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(local_host, local_port),
        timeout=timeout,
    )
    try:
        banner = await asyncio.wait_for(reader.readline(), timeout=timeout)
        return banner.decode(errors="replace").strip()
    finally:
        writer.close()
        await writer.wait_closed()


def _is_retryable_ssh_exit(exit_code: int) -> bool:
    return exit_code == 255


def _is_retryable_banner_error(exc: Exception) -> bool:
    text = str(exc).lower()
    return (
        isinstance(exc, (TimeoutError, asyncio.TimeoutError))
        or "connection reset by peer" in text
        or "timed out during banner exchange" in text
        or "connection timed out" in text
        or "broken pipe" in text
    )


async def _retry_async_operation(
    label: str,
    attempts: int,
    delay_s: float,
    operation,
    should_retry,
):
    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            return await operation()
        except Exception as exc:
            last_error = exc
            if attempt >= attempts or not should_retry(exc):
                raise
            LOG.warning(
                "%s failed on attempt %d/%d: %s; retrying in %.1fs",
                label,
                attempt,
                attempts,
                exc,
                delay_s,
            )
            if delay_s > 0:
                await asyncio.sleep(delay_s)
    if last_error is not None:
        raise last_error


async def _retry_ssh_login(args: argparse.Namespace) -> int:
    attempts = max(1, args.ssh_retries)
    delay_s = max(0.0, args.ssh_retry_delay)

    for attempt in range(1, attempts + 1):
        exit_code = await _run_ssh(args)
        if not _is_retryable_ssh_exit(exit_code) or attempt >= attempts:
            return exit_code

        # When whole-session retry is available, reconnect BLE+proxy instead of
        # retrying SSH inside a potentially corrupted tunnel session.
        if args.session_retries > 1:
            LOG.warning(
                "SSH login failed with exit code %d on attempt %d/%d; escalating to whole-session reconnect",
                exit_code,
                attempt,
                attempts,
            )
            return exit_code

        LOG.warning(
            "SSH login failed with exit code %d on attempt %d/%d; retrying in %.1fs",
            exit_code,
            attempt,
            attempts,
            delay_s,
        )
        if delay_s > 0:
            await asyncio.sleep(delay_s)

    return exit_code


async def _cleanup_stale_tunnels(client: BleEthClient, max_connections: int) -> None:
    """Ask firmware to release any stale TCP tunnel slots from a previous run."""
    if max_connections <= 0:
        return

    print(f"Cleaning stale BLE-Eth tunnel slots 0..{max_connections - 1}...")
    for conn_id in range(max_connections):
        try:
            await client.close_tunnel(conn_id, timeout=0.75, force=True)
        except Exception:
            # Some firmware revisions only respond for active slots. A timeout
            # here is harmless; the DISCONNECT frame was still sent.
            LOG.debug("stale cleanup ignored for conn_id=%s", conn_id, exc_info=True)


async def _run_ssh(args: argparse.Namespace) -> int:
    ssh_target = f"{args.user}@127.0.0.1" if args.user else "127.0.0.1"
    cmd = [
        args.ssh_bin,
        "-p",
        str(args.local_port),
        "-o",
        "HostKeyAlias=openterface-ble-eth",
        "-tt",
    ]

    if args.no_host_key_check:
        cmd += [
            "-o",
            "StrictHostKeyChecking=no",
            "-o",
            "UserKnownHostsFile=/dev/null",
        ]

    if args.no_keystroke_obfuscation:
        cmd += [
            "-o",
            "ObscureKeystrokeTiming=no",
        ]

    if args.safe_ssh_profile:
        cmd += SAFE_SSH_OPTIONS

    cmd += args.ssh_arg
    cmd.append(ssh_target)

    if args.remote_command:
        cmd.append(args.remote_command)

    print("Launching SSH:")
    print("  " + " ".join(cmd))
    print()

    proc = await asyncio.create_subprocess_exec(*cmd)
    return await proc.wait()


async def _resolve_target_from_device(client: BleEthClient) -> str:
    """Read the target host address from the device's ECM IP configuration."""
    try:
        config = await client.get_ecm_ip(ECM_IP_GET_ALL, timeout=3.0)
        target = config.get("target_host")
        if target:
            return target
    except (TimeoutError, ConnectionError) as exc:
        LOG.warning("Could not read ECM IP configuration: %s", exc)

    try:
        default = await client.get_default_target(timeout=3.0)
        return default["ip"]
    except (TimeoutError, ConnectionError) as exc:
        raise ConnectionError(
            "Could not determine the target IP; pass --target explicitly or "
            "configure the ECM target address on the device."
        ) from exc


async def run(args: argparse.Namespace) -> int:
    client = BleEthClient(
        device_name=args.device_name,
        ble_address=args.ble_addr,
        trace_frames=args.trace_frames,
        data_ack_policy=args.data_ack_policy,
    )
    ble_link_lost = False
    session_attempts = max(1, args.session_retries)
    session_retry_delay = max(0.0, args.session_retry_delay)
    current_stop_event: asyncio.Event | None = None

    def _event_logger(event: TunnelEvent) -> None:
        nonlocal ble_link_lost, current_stop_event
        # DATA events are high-frequency during interactive SSH and can corrupt
        # terminal readability. Keep them visible only in explicit trace mode.
        if event.type == TunnelEvent.Type.DATA_RECEIVED and not args.trace_frames:
            return
        LOG.info("event: %s", _format_event(event))
        if event.type == TunnelEvent.Type.ERROR and "BLE disconnected" in event.error_msg:
            ble_link_lost = True
            if current_stop_event is not None:
                current_stop_event.set()

    client.add_event_callback(_event_logger)

    async def _run_single_session() -> int:
        nonlocal ble_link_lost, current_stop_event
        ble_link_lost = False
        proxy: BleTcpProxy | None = None
        stop_event = asyncio.Event()
        current_stop_event = stop_event

        def _request_stop() -> None:
            stop_event.set()

        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _request_stop)
            except NotImplementedError:
                pass

        try:
            print(f"Connecting to BLE device '{args.device_name}'...")
            await client.connect()
            print("BLE connected.")

            info = await client.get_info()
            print(
                "Device info: "
                f"version={info['version']} "
                f"max_connections={info['max_connections']} "
                f"max_fragment_payload={info['max_fragment_payload']} "
                f"features=0x{info['features']:02X}"
            )
            client.apply_device_features(int(info.get("features", 0)))

            target_ip = args.target
            if target_ip is None:
                target_ip = await _resolve_target_from_device(client)
                print(f"Using ECM target host from device: {target_ip}")

            if not args.no_cleanup_stale:
                await _cleanup_stale_tunnels(client, int(info.get("max_connections", 0)))

            max_port_attempts = 10 if args.auto_port else 1
            bind_error: OSError | None = None
            selected_local_port = args.local_port

            for offset in range(max_port_attempts):
                candidate_port = args.local_port + offset
                proxy = BleTcpProxy(
                    client=client,
                    local_host=args.local_host,
                    local_port=candidate_port,
                    target_ip=target_ip,
                    target_port=args.target_port,
                    recv_timeout=args.recv_timeout,
                    max_connections=info.get("max_connections", 0),
                )
                try:
                    await proxy.start()
                    selected_local_port = candidate_port
                    if offset > 0:
                        print(
                            f"Local port {args.local_port} busy, switched to {candidate_port}."
                        )
                    break
                except OSError as exc:
                    # 48 = macOS EADDRINUSE, 98 = Linux EADDRINUSE
                    if exc.errno in (48, 98):
                        bind_error = exc
                        proxy = None
                        continue
                    raise
            else:
                if bind_error is not None:
                    raise bind_error
                raise RuntimeError("Failed to bind local proxy port")

            # Keep the rest of the flow (status text + SSH launch) aligned with
            # the actual listening port.
            args.local_port = selected_local_port

            print()
            print("BLE-Eth SSH proxy is active:")
            print(f"  local:  {args.local_host}:{args.local_port}")
            print(f"  remote: {target_ip}:{args.target_port}")
            print()

            if args.test_banner:
                print("Reading SSH banner through BLE-Eth tunnel...")

                async def _banner_operation() -> str:
                    return await _read_ssh_banner(args.local_host, args.local_port, args.connect_timeout)

                banner = await _retry_async_operation(
                    label="SSH banner",
                    attempts=max(1, args.ssh_retries),
                    delay_s=max(0.0, args.ssh_retry_delay),
                    operation=_banner_operation,
                    should_retry=_is_retryable_banner_error,
                )
                print(f"SSH banner: {banner}")
                if not banner.startswith("SSH-"):
                    print("ERROR: remote did not return an SSH banner.", file=sys.stderr)
                    return 2
                return 0

            if args.proxy_only:
                print("Proxy-only mode. Connect from another terminal with:")
                print(f"  ssh -p {args.local_port} <user>@127.0.0.1")
                print("Press Ctrl+C to stop.")
                await stop_event.wait()
                if ble_link_lost:
                    print("ERROR: BLE link disconnected while proxy was running.", file=sys.stderr)
                    return 1
                return 0

            ssh_exit = await _retry_ssh_login(args)
            return ssh_exit

        except asyncio.CancelledError:
            raise
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            if args.verbose:
                raise
            return 1
        finally:
            current_stop_event = None
            if proxy is not None:
                await proxy.stop()
            await client.disconnect()
            print("Disconnected from BLE device.")

    if args.proxy_only or args.test_banner:
        last_exit = 1
        for attempt in range(1, session_attempts + 1):
            last_exit = await _run_single_session()
            if last_exit == 0:
                return 0
            if not ble_link_lost or attempt >= session_attempts:
                return last_exit
            LOG.warning(
                "BLE session lost during proxy/banner run on attempt %d/%d; reconnecting in %.1fs",
                attempt,
                session_attempts,
                session_retry_delay,
            )
            if session_retry_delay > 0:
                await asyncio.sleep(session_retry_delay)
        return last_exit

    last_exit = 1
    for attempt in range(1, session_attempts + 1):
        last_exit = await _run_single_session()
        if last_exit != 255 or attempt >= session_attempts:
            return last_exit
        LOG.warning(
            "SSH session exited with code %d on attempt %d/%d; reconnecting in %.1fs",
            last_exit,
            attempt,
            session_attempts,
            session_retry_delay,
        )
        if session_retry_delay > 0:
            await asyncio.sleep(session_retry_delay)

    return last_exit


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="SSH through Openterface BLE-Eth by starting a local TCP proxy.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--target",
        default=None,
        help="Remote SSH server IP reachable from the firmware ECM side "
        "(default: read the target host from the device's ECM configuration).",
    )
    parser.add_argument(
        "--target-port",
        type=int,
        default=22,
        help="Remote SSH server port (default: 22).",
    )
    parser.add_argument(
        "--local-host",
        default="127.0.0.1",
        help="Local proxy bind address (default: 127.0.0.1).",
    )
    parser.add_argument(
        "--local-port",
        type=int,
        default=18048,
        help="Local proxy port used by ssh (default: 18048).",
    )
    parser.add_argument(
        "--auto-port",
        action="store_true",
        help="Auto-increment local port if the requested one is in use (default enabled).",
    )
    parser.add_argument(
        "--no-auto-port",
        dest="auto_port",
        action="store_false",
        help="Do not auto-increment local port if bind fails.",
    )
    parser.add_argument(
        "--device-name",
        default="KVM-GO",
        help="BLE device name substring to scan for (default: KVM-GO).",
    )
    parser.add_argument(
        "--ble-addr",
        default=None,
        help="BLE address/identifier. If provided, skips name-based selection.",
    )
    parser.add_argument(
        "--user",
        default=os.environ.get("USER"),
        help="SSH username for interactive SSH mode (default: current local user).",
    )
    parser.add_argument(
        "--ssh-bin",
        default="ssh",
        help="SSH executable to launch (default: ssh).",
    )
    parser.add_argument(
        "--ssh-arg",
        action="append",
        default=[],
        help="Extra argument passed to ssh. Repeat for multiple arguments.",
    )
    parser.add_argument(
        "--remote-command",
        default="",
        help="Remote command to run after SSH connects. Leave empty for an interactive shell.",
    )
    parser.add_argument(
        "--no-host-key-check",
        action="store_true",
        help="Disable SSH host-key prompts for quick lab testing (default enabled).",
    )
    parser.add_argument(
        "--host-key-check",
        dest="no_host_key_check",
        action="store_false",
        help="Enable normal SSH host-key checking.",
    )
    parser.add_argument(
        "--no-keystroke-obfuscation",
        action="store_true",
        help="Disable SSH keystroke timing obfuscation to reduce interactive packet noise (default enabled).",
    )
    parser.add_argument(
        "--keystroke-obfuscation",
        dest="no_keystroke_obfuscation",
        action="store_false",
        help="Enable SSH keystroke timing obfuscation.",
    )
    parser.add_argument(
        "--safe-ssh-profile",
        action="store_true",
        help="Apply known-good SSH options for BLE-Eth interactive stability (default enabled).",
    )
    parser.add_argument(
        "--unsafe-ssh-profile",
        dest="safe_ssh_profile",
        action="store_false",
        help="Disable known-good SSH profile and use broader OpenSSH defaults.",
    )
    parser.add_argument(
        "--no-cleanup-stale",
        action="store_true",
        help="Do not send startup DISCONNECT frames for stale firmware tunnel slots.",
    )
    parser.add_argument(
        "--proxy-only",
        action="store_true",
        help="Start the local BLE-Eth proxy and wait; do not launch ssh.",
    )
    parser.add_argument(
        "--test-banner",
        action="store_true",
        help="Read the remote SSH banner through the tunnel and exit.",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=20.0,
        help="Timeout for local SSH banner test connection (default: 20 seconds).",
    )
    parser.add_argument(
        "--recv-timeout",
        type=float,
        default=180.0,
        help="BLE-to-local receive timeout used by the proxy (default: 180 seconds).",
    )
    parser.add_argument(
        "--ssh-retries",
        type=int,
        default=3,
        help="Retry SSH login/banner attempts on transient banner/kex failures (default: 3).",
    )
    parser.add_argument(
        "--ssh-retry-delay",
        type=float,
        default=1.5,
        help="Delay between SSH retry attempts in seconds (default: 1.5).",
    )
    parser.add_argument(
        "--session-retries",
        type=int,
        default=5,
        help="Retry the whole BLE+SSH session on exit 255 (default: 5).",
    )
    parser.add_argument(
        "--session-retry-delay",
        type=float,
        default=2.0,
        help="Delay between whole-session retries in seconds (default: 2.0).",
    )
    parser.add_argument(
        "--trace-frames",
        action="store_true",
        help="Log BLE-Eth frame summaries, checksums, fragments, and TCP forwarding direction.",
    )
    parser.add_argument(
        "--data-ack-policy",
        choices=["legacy", "feature", "meta"],
        default="feature",
        help=(
            "Host DATA ACK policy: 'legacy' disables per-fragment ACK waiting, "
            "'feature' enables strict metadata ACK only when device advertises support, "
            "'meta' forces strict metadata ACK mode."
        ),
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable debug logging.",
    )
    parser.set_defaults(
        auto_port=True,
        no_host_key_check=True,
        no_keystroke_obfuscation=True,
        safe_ssh_profile=True,
        session_retries=5,
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    try:
        import bleak  # noqa: F401
    except ImportError:
        print("ERROR: bleak library not installed. Install with: pip install bleak", file=sys.stderr)
        return 1

    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())