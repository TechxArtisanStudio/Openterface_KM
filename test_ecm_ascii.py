#!/usr/bin/env python3
"""
Test script for ECM IP configuration commands (0x26/0x27) - ASCII version for Windows
"""

import serial
import struct
import time
import sys

# CH9329 protocol constants
CH9329_HEADER = 0x57
CH9329_ADDR_BROADCAST = 0x00

# ECM IP Config commands
CMD_SET_ECM_IP_CONFIG = 0x26
CMD_GET_ECM_IP_CONFIG = 0x27

# Sub-commands
SUBCMD_GET_ALL = 0x00
SUBCMD_GET_HOST = 0x01
SUBCMD_GET_TARGET = 0x02
SUBCMD_SET_ALL = 0x00
SUBCMD_SET_HOST = 0x01
SUBCMD_SET_TARGET = 0x02
SUBCMD_RESET = 0x03

# Status codes
STATUS_SUCCESS = 0x00
STATUS_ERR_PARAM = 0xE1
STATUS_ERR_CMD = 0xE2

def calculate_crc(data):
    """Calculate CRC16 for CH9329 protocol"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def build_ch9329_frame(addr, cmd, data):
    """Build a CH9329 command frame"""
    length = len(data) + 3  # cmd + data + crc
    frame = [CH9329_HEADER, addr, cmd, length] + list(data)
    crc = calculate_crc(frame)
    frame.extend([crc & 0xFF, (crc >> 8) & 0xFF])
    return bytes(frame)

def parse_ch9329_response(response):
    """Parse a CH9329 response frame"""
    if len(response) < 6:
        return None, None, None, "Response too short"

    if response[0] != CH9329_HEADER:
        return None, None, None, "Invalid header"

    addr = response[1]
    cmd = response[2]
    length = response[3]

    if len(response) < length + 4:
        return None, None, None, "Incomplete response"

    data = response[4:4+length-2]  # Exclude CRC
    crc_received = response[4+length-2] | (response[4+length-1] << 8)
    crc_calculated = calculate_crc(response[:4+length-2])

    if crc_received != crc_calculated:
        return None, None, None, "CRC mismatch"

    return addr, cmd, data, None

def test_get_ecm_config(ser, sub_cmd, test_name):
    """Test GET ECM IP config command"""
    print(f"\n[Test] {test_name}")
    print(f"  Sending CMD_GET_ECM_IP_CONFIG (0x27) with sub_cmd=0x{sub_cmd:02X}")

    data = bytes([sub_cmd])
    frame = build_ch9329_frame(CH9329_ADDR_BROADCAST, CMD_GET_ECM_IP_CONFIG, data)

    print(f"  Frame: {frame.hex()}")
    ser.write(frame)

    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 1)
    if not response:
        print("  FAIL: No response received")
        return False

    print(f"  Response: {response.hex()}")

    addr, cmd, data, error = parse_ch9329_response(response)
    if error:
        print(f"  FAIL: Parse error: {error}")
        return False

    if cmd != CMD_GET_ECM_IP_CONFIG:
        print(f"  FAIL: Wrong command in response: 0x{cmd:02X}")
        return False

    status = data[0]
    if status != STATUS_SUCCESS:
        print(f"  FAIL: Command failed with status: 0x{status:02X}")
        return False

    # Parse IP addresses based on sub-command
    if sub_cmd == SUBCMD_GET_ALL:
        if len(data) < 21:
            print(f"  FAIL: Response too short: {len(data)} bytes")
            return False
        host_ip = '.'.join(str(b) for b in data[1:5])
        dev_ip = '.'.join(str(b) for b in data[5:9])
        subnet = '.'.join(str(b) for b in data[9:13])
        target_host_ip = '.'.join(str(b) for b in data[13:17])
        target_dev_ip = '.'.join(str(b) for b in data[17:21])
        print(f"  PASS: Host IP: {host_ip}")
        print(f"  PASS: Dev IP: {dev_ip}")
        print(f"  PASS: Subnet: {subnet}")
        print(f"  PASS: Target Host IP: {target_host_ip}")
        print(f"  PASS: Target Dev IP: {target_dev_ip}")
    elif sub_cmd == SUBCMD_GET_HOST:
        if len(data) < 13:
            print(f"  FAIL: Response too short: {len(data)} bytes")
            return False
        host_ip = '.'.join(str(b) for b in data[1:5])
        dev_ip = '.'.join(str(b) for b in data[5:9])
        subnet = '.'.join(str(b) for b in data[9:13])
        print(f"  PASS: Host IP: {host_ip}")
        print(f"  PASS: Dev IP: {dev_ip}")
        print(f"  PASS: Subnet: {subnet}")
    elif sub_cmd == SUBCMD_GET_TARGET:
        if len(data) < 9:
            print(f"  FAIL: Response too short: {len(data)} bytes")
            return False
        target_host_ip = '.'.join(str(b) for b in data[1:5])
        target_dev_ip = '.'.join(str(b) for b in data[5:9])
        print(f"  PASS: Target Host IP: {target_host_ip}")
        print(f"  PASS: Target Dev IP: {target_dev_ip}")

    print("  PASS: Test passed")
    return True

def test_set_ecm_config(ser, sub_cmd, test_name, test_data):
    """Test SET ECM IP config command"""
    print(f"\n[Test] {test_name}")
    print(f"  Sending CMD_SET_ECM_IP_CONFIG (0x26) with sub_cmd=0x{sub_cmd:02X}")

    data = bytes([sub_cmd]) + test_data
    frame = build_ch9329_frame(CH9329_ADDR_BROADCAST, CMD_SET_ECM_IP_CONFIG, data)

    print(f"  Frame: {frame.hex()}")
    ser.write(frame)

    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 1)
    if not response:
        print("  FAIL: No response received")
        return False

    print(f"  Response: {response.hex()}")

    addr, cmd, data, error = parse_ch9329_response(response)
    if error:
        print(f"  FAIL: Parse error: {error}")
        return False

    if cmd != CMD_SET_ECM_IP_CONFIG:
        print(f"  FAIL: Wrong command in response: 0x{cmd:02X}")
        return False

    status = data[0]
    if status != STATUS_SUCCESS:
        print(f"  FAIL: Command failed with status: 0x{status:02X}")
        return False

    print("  PASS: Test passed")
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_ecm_ascii.py <serial_port>")
        print("Example: python3 test_ecm_ascii.py COM7")
        sys.exit(1)

    port = sys.argv[1]
    baudrate = 115200

    print(f"Connecting to {port} at {baudrate} baud...")

    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except serial.SerialException as e:
        print(f"FAIL: Failed to open serial port: {e}")
        sys.exit(1)

    time.sleep(2)  # Wait for device to initialize

    print("\n" + "="*60)
    print("ECM IP Configuration Command Test")
    print("="*60)

    all_passed = True

    # Test 1: Get All Configuration
    if not test_get_ecm_config(ser, SUBCMD_GET_ALL, "Get All Configuration"):
        all_passed = False

    # Test 2: Get Host Side Configuration
    if not test_get_ecm_config(ser, SUBCMD_GET_HOST, "Get Host Side Configuration"):
        all_passed = False

    # Test 3: Get Target Side Configuration
    if not test_get_ecm_config(ser, SUBCMD_GET_TARGET, "Get Target Side Configuration"):
        all_passed = False

    # Test 4: Set Host Side Configuration
    # Format: host_ip(4) + dev_ip(4) + subnet(4) = 12 bytes
    host_config = bytes([
        192, 168, 11, 1,    # Host IP
        192, 168, 11, 3,    # Dev IP
        255, 255, 255, 0    # Subnet mask
    ])
    if not test_set_ecm_config(ser, SUBCMD_SET_HOST, "Set Host Side Configuration", host_config):
        all_passed = False

    # Verify the change
    if not test_get_ecm_config(ser, SUBCMD_GET_HOST, "Verify Host Side Configuration After Set"):
        all_passed = False

    # Test 5: Set Target Side Configuration
    # Format: target_host_ip(4) + target_dev_ip(4) = 8 bytes
    target_config = bytes([
        192, 168, 11, 2,    # Target Host IP
        192, 168, 11, 5     # Target Dev IP
    ])
    if not test_set_ecm_config(ser, SUBCMD_SET_TARGET, "Set Target Side Configuration", target_config):
        all_passed = False

    # Verify the change
    if not test_get_ecm_config(ser, SUBCMD_GET_TARGET, "Verify Target Side Configuration After Set"):
        all_passed = False

    # Test 6: Reset to Default
    if not test_set_ecm_config(ser, SUBCMD_RESET, "Reset to Default Configuration", b''):
        all_passed = False

    # Verify reset
    if not test_get_ecm_config(ser, SUBCMD_GET_ALL, "Verify Configuration After Reset"):
        all_passed = False

    print("\n" + "="*60)
    if all_passed:
        print("PASS: All tests passed!")
    else:
        print("FAIL: Some tests failed")
    print("="*60)

    ser.close()
    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()