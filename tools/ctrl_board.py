#!/usr/bin/env python3
"""
Control Board Tool for Openterface KM Dev Kit.

Controls the target's BOOT pin and power via the control board (USB Serial).
Uses 115200 baud serial commands.

Usage:
    python3 tools/ctrl_board.py power off
    python3 tools/ctrl_board.py power on
    python3 tools/ctrl_board.py boot high
    python3 tools/ctrl_board.py boot low
    python3 tools/ctrl_board.py isp on          # BOOT HIGH + power ON
    python3 tools/ctrl_board.py isp off         # BOOT LOW + power cycle
    python3 tools/ctrl_board.py flash <firmware.bin>  # full ISP flash cycle
    python3 tools/ctrl_board.py status          # show USB devices
    python3 tools/ctrl_board.py verify          # verify firmware is running
"""

import argparse
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip3 install pyserial", file=sys.stderr)
    sys.exit(1)


# ── Command frames ──────────────────────────────────────────────────────────
CMD_BOOT_HIGH  = bytes.fromhex("57AB001901011D")
CMD_BOOT_LOW   = bytes.fromhex("57AB001901001C")
CMD_POWER_ON   = bytes.fromhex("57AB001A01011E")
CMD_POWER_OFF  = bytes.fromhex("57AB001A01001D")

# The dev-kit control board firmware listens at 460800, not 115200.
# At 115200 it silently ignores frames, so power/BOOT commands appear to
# succeed (the tool prints the label) but never take effect.
BAUD = 460800


# ── Helpers ─────────────────────────────────────────────────────────────────

def find_control_port():
    """Find the control board serial port by model name."""
    import glob
    for dev in sorted(glob.glob("/dev/ttyACM*")):
        try:
            result = subprocess.run(
                ["udevadm", "info", "-q", "property", "-n", dev],
                capture_output=True, text=True, timeout=2,
            )
            props = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
            model = props.get("ID_MODEL", "")
            if model == "USB_Serial":
                return dev
        except Exception:
            continue
    return None


def send_cmd(port, cmd, label="", read_resp=True):
    """Send a command frame to the control board."""
    with serial.Serial(port, BAUD, timeout=1) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        ser.write(cmd)
        if label:
            print(f"  {label}")
        if read_resp:
            time.sleep(0.5)
            if ser.in_waiting:
                resp = ser.read(ser.in_waiting)
                return resp
    return None


def lsusb_filter(patterns):
    """Return lsusb lines matching any of the patterns."""
    try:
        out = subprocess.check_output(["lsusb"], text=True)
        matches = []
        for line in out.splitlines():
            for p in patterns:
                if p.lower() in line.lower():
                    matches.append(line.strip())
                    break
        return matches
    except Exception:
        return []


# ── Commands ────────────────────────────────────────────────────────────────

def cmd_power(port, state):
    """Power the target on or off."""
    if state == "on":
        send_cmd(port, CMD_POWER_ON, "Power ON")
    else:
        send_cmd(port, CMD_POWER_OFF, "Power OFF")


def cmd_boot(port, state):
    """Set BOOT pin high or low."""
    if state == "high":
        send_cmd(port, CMD_BOOT_HIGH, "BOOT → HIGH")
    else:
        send_cmd(port, CMD_BOOT_LOW, "BOOT → LOW")


def cmd_isp_on(port):
    """Enter ISP mode: BOOT HIGH → power ON."""
    print("Entering ISP mode...")
    send_cmd(port, CMD_POWER_OFF, "1. Power OFF")
    time.sleep(1)
    send_cmd(port, CMD_BOOT_HIGH, "2. BOOT → HIGH")
    time.sleep(1)
    send_cmd(port, CMD_POWER_ON, "3. Power ON")
    time.sleep(3)

    devs = lsusb_filter(["55e0"])
    if devs:
        print(f"  ✓ ISP device found: {devs[0]}")
    else:
        print("  ✗ ISP device NOT found")
        print("  Current USB devices:")
        for d in lsusb_filter(["1a86", "4348"]):
            print(f"    {d}")
        return False
    return True


def cmd_isp_off(port):
    """Exit ISP mode: BOOT LOW → power cycle."""
    print("Exiting ISP mode...")
    send_cmd(port, CMD_BOOT_LOW, "1. BOOT → LOW")
    time.sleep(1)
    send_cmd(port, CMD_POWER_OFF, "2. Power OFF")
    time.sleep(1)
    send_cmd(port, CMD_POWER_ON, "3. Power ON")
    time.sleep(5)

    devs = lsusb_filter(["fe00"])
    if devs:
        print(f"  ✓ Target running: {devs[0]}")
    else:
        print("  ✗ Target NOT found")
        for d in lsusb_filter(["1a86", "4348"]):
            print(f"    {d}")


def verify_firmware():
    """Verify firmware is running by checking USB device descriptors."""
    # Check for Openterface Target device
    devs = lsusb_filter(["fe00"])
    if not devs:
        print("  ✗ Openterface Target not found")
        return False

    print(f"  ✓ Target device found: {devs[0]}")

    # Get device descriptors for firmware version info
    try:
        result = subprocess.run(
            ["lsusb", "-d", "1a86:fe00", "-v"],
            capture_output=True, text=True, timeout=5,
        )
        for line in result.stdout.splitlines():
            line = line.strip()
            if "bcdDevice" in line:
                # bcdDevice 4.00 = firmware version 4.0.0
                print(f"  Firmware version: {line.split()[-1]}")
            elif "iProduct" in line:
                print(f"  Product: {line.split(maxsplit=2)[-1]}")
            elif "iManufacturer" in line:
                print(f"  Manufacturer: {line.split(maxsplit=2)[-1]}")
            elif "iSerial" in line:
                print(f"  Serial: {line.split(maxsplit=2)[-1]}")
    except Exception:
        pass

    return True


def cmd_flash(port, firmware):
    """Full flash cycle: enter ISP → flash → exit ISP → verify."""
    print(f"Firmware: {firmware}")

    # Step 1: enter ISP
    if not cmd_isp_on(port):
        print("\nFailed to enter ISP mode. Aborting.")
        return False

    # Step 2: flash
    print("\nFlashing...")
    try:
        result = subprocess.run(
            ["/home/bbot/.cargo/bin/wchisp", "flash", firmware],
            capture_output=True, text=True, timeout=60,
        )
        print(result.stdout)
        if result.returncode != 0:
            print(f"wchisp failed: {result.stderr}", file=sys.stderr)
            return False
    except FileNotFoundError:
        print("Error: wchisp not found at ~/.cargo/bin/wchisp", file=sys.stderr)
        return False

    # Step 3: exit ISP
    cmd_isp_off(port)

    # Step 4: verify firmware is running
    print("\nVerifying firmware...")
    if verify_firmware():
        print("  ✓ Firmware is running")
    else:
        print("  ✗ Firmware verification failed")

    return True


def cmd_status():
    """Show current USB device status."""
    print("USB devices:")
    for d in lsusb_filter(["1a86", "4348"]):
        print(f"  {d}")

    print("\nSerial ports:")
    import glob
    for dev in sorted(glob.glob("/dev/ttyACM*")):
        try:
            result = subprocess.run(
                ["udevadm", "info", "-q", "property", "-n", dev],
                capture_output=True, text=True, timeout=2,
            )
            props = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
            model = props.get("ID_MODEL", "unknown")
            print(f"  {dev} -> {model}")
        except Exception:
            print(f"  {dev} -> (unknown)")


def cmd_verify():
    """Verify firmware is running."""
    print("Verifying firmware...")
    if verify_firmware():
        print("\n✓ Firmware is running correctly")
    else:
        print("\n✗ Firmware verification failed")
        sys.exit(1)


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Openterface KM Dev Kit Control Board Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command")

    # power
    p_power = sub.add_parser("power", help="Control target power")
    p_power.add_argument("state", choices=["on", "off"])

    # boot
    p_boot = sub.add_parser("boot", help="Control BOOT pin")
    p_boot.add_argument("state", choices=["high", "low"])

    # isp
    p_isp = sub.add_parser("isp", help="Enter/exit ISP mode")
    p_isp.add_argument("action", choices=["on", "off"])

    # flash
    p_flash = sub.add_parser("flash", help="Full ISP flash cycle")
    p_flash.add_argument("firmware", help="Path to firmware .bin file")

    # status
    sub.add_parser("status", help="Show USB device status")

    # verify
    sub.add_parser("verify", help="Verify firmware is running")

    # port override
    parser.add_argument("--port", "-p", help="Serial port (auto-detect if omitted)")

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command == "status":
        cmd_status()
        return

    if args.command == "verify":
        cmd_verify()
        return

    # Find control port
    port = args.port or find_control_port()
    if not port:
        print("Error: control board not found. Use --port to specify.", file=sys.stderr)
        sys.exit(1)
    print(f"Control board: {port}")

    if args.command == "power":
        cmd_power(port, args.state)
    elif args.command == "boot":
        cmd_boot(port, args.state)
    elif args.command == "isp":
        if args.action == "on":
            cmd_isp_on(port)
        else:
            cmd_isp_off(port)
    elif args.command == "flash":
        ok = cmd_flash(port, args.firmware)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
