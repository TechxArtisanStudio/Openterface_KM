#!/usr/bin/env python3
"""
Firmware Verification Tool for Openterface KM.

Sends CMD_GET_INFO (0x01) via CDC ACM to verify firmware is running
and returns device status information.

Usage:
    python3 tools/verify_firmware.py
    python3 tools/verify_firmware.py --port /dev/ttyACM1
"""

import argparse
import serial
import sys
import time


def calculate_checksum(frame):
    """Calculate CH9329 checksum (sum of all bytes except last, masked to 8 bits)."""
    return sum(frame[:-1]) & 0xFF


def send_get_info(port, timeout=2):
    """Send CMD_GET_INFO and return parsed response."""
    # Build frame: 57 AB <addr> <cmd> <len> <checksum>
    addr = 0x00
    cmd = 0x01  # CMD_GET_INFO
    data_len = 0
    frame = bytes([0x57, 0xAB, addr, cmd, data_len])
    checksum = calculate_checksum(frame + bytes([0]))
    frame = frame + bytes([checksum])

    with serial.Serial(port, 115200, timeout=timeout) as ser:
        ser.reset_input_buffer()
        ser.write(frame)

        # Wait for response
        for _ in range(10):
            time.sleep(0.3)
            if ser.in_waiting:
                resp = ser.read(ser.in_waiting)

                # Verify response format
                if len(resp) < 14:
                    return None, f"Response too short: {len(resp)} bytes"

                if resp[0] != 0x57 or resp[1] != 0xAB:
                    return None, f"Invalid header: {resp[:2].hex()}"

                r_addr = resp[2]
                r_cmd = resp[3]
                r_len = resp[4]
                r_data = resp[5:5+r_len]
                r_checksum = resp[5+r_len]

                # Verify checksum
                expected_checksum = calculate_checksum(resp)
                if r_checksum != expected_checksum:
                    return None, f"Checksum mismatch: expected {expected_checksum:02X}, got {r_checksum:02X}"

                # Parse data
                if r_cmd == 0x81 and r_len == 8:
                    version = r_data[0]
                    connected = r_data[1]
                    led_status = r_data[2]
                    flags = r_data[3]

                    return {
                        'version': version,
                        'connected': bool(connected),
                        'led_status': led_status,
                        'flags': flags,
                        'raw': r_data.hex()
                    }, None
                else:
                    return None, f"Unexpected response: cmd={r_cmd:02X}, len={r_len}"

        return None, "No response received"


def main():
    parser = argparse.ArgumentParser(description="Verify Openterface KM firmware")
    parser.add_argument("--port", "-p", default="/dev/ttyACM1",
                        help="Serial port (default: /dev/ttyACM1)")
    args = parser.parse_args()

    print(f"Verifying firmware on {args.port}...")

    info, error = send_get_info(args.port)

    if error:
        print(f"✗ Verification failed: {error}")
        sys.exit(1)

    print("✓ Firmware is running")
    print(f"  Board version: {info['version']}")
    print(f"  USB connected: {'Yes' if info['connected'] else 'No'}")
    print(f"  LED status: 0x{info['led_status']:02X}")
    print(f"  Flags: 0x{info['flags']:02X}")
    print(f"  Raw data: {info['raw']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
