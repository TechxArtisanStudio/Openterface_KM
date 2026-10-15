#!/usr/bin/env python3
"""
Test script for ECM IP configuration commands (0x26/0x27) - Fixed version
"""
import serial
import time
import sys

# CH9329 command codes
CMD_GET_ECM_IP_CONFIG = 0x27
CMD_SET_ECM_IP_CONFIG = 0x26

# Sub-commands
SUBCMD_GET_ALL = 0x00
SUBCMD_GET_HOST_SIDE = 0x01
SUBCMD_GET_TARGET_SIDE = 0x02

SUBCMD_SET_ALL = 0x00
SUBCMD_SET_HOST_SIDE = 0x01
SUBCMD_SET_TARGET_SIDE = 0x02
SUBCMD_RESET_DEFAULT = 0x03

def calculate_checksum(data):
    """Calculate checksum (sum of all bytes)"""
    return sum(data) & 0xFF

def build_command(addr, cmd_code, data):
    """Build CH9329 command frame"""
    frame = [0x57, 0xAB, addr, cmd_code, len(data)] + data
    checksum = calculate_checksum(frame)
    frame.append(checksum)
    return bytes(frame)

def parse_response(response):
    """Parse CH9329 response frame"""
    if len(response) < 6:
        return None, "Response too short"

    if response[0] != 0x57 or response[1] != 0xAB:
        return None, "Invalid frame header"

    addr = response[2]
    cmd_code = response[3]
    data_len = response[4]

    if len(response) < 5 + data_len + 1:
        return None, "Response incomplete"

    data = response[5:5+data_len]
    checksum = response[5+data_len]

    # Verify checksum
    expected_checksum = calculate_checksum(response[:5+data_len])
    if checksum != expected_checksum:
        return None, f"Checksum mismatch: got {checksum:02X}, expected {expected_checksum:02X}"

    return {
        'addr': addr,
        'cmd_code': cmd_code,
        'data': data,
        'status': data[0] if data else None
    }, None

def ip_to_str(ip_bytes):
    """Convert 4 bytes to IP string"""
    return '.'.join(str(b) for b in ip_bytes)

def str_to_ip(ip_str):
    """Convert IP string to 4 bytes"""
    return [int(x) for x in ip_str.split('.')]

def test_get_ecm_ip_config(ser):
    """Test GET ECM IP CONFIG command"""
    print("\n=== Test GET ECM IP CONFIG (All) ===")
    cmd = build_command(0x00, CMD_GET_ECM_IP_CONFIG, [SUBCMD_GET_ALL])
    print(f"Sending: {cmd.hex()}")
    ser.write(cmd)
    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 100)
    if response:
        print(f"Received: {response.hex()}")
        result, error = parse_response(response)
        if error:
            print(f"Error: {error}")
            return False

        if result['status'] == 0x00:
            data = result['data'][1:]  # Skip status byte
            if len(data) >= 20:
                host_ip = ip_to_str(data[0:4])
                dev_ip = ip_to_str(data[4:8])
                subnet = ip_to_str(data[8:12])
                target_host_ip = ip_to_str(data[12:16])
                target_dev_ip = ip_to_str(data[16:20])

                print(f"Success!")
                print(f"  Host IP:        {host_ip}")
                print(f"  Device IP:      {dev_ip}")
                print(f"  Subnet Mask:    {subnet}")
                print(f"  Target Host IP: {target_host_ip}")
                print(f"  Target Dev IP:  {target_dev_ip}")
                return True
            else:
                print(f"Error: Response data too short ({len(data)} bytes)")
                return False
        else:
            print(f"Error: Status code 0x{result['status']:02X}")
            return False
    else:
        print("Error: No response received")
        return False

def test_set_ecm_ip_config(ser, host_ip, dev_ip, subnet, target_host_ip, target_dev_ip):
    """Test SET ECM IP CONFIG command"""
    print(f"\n=== Test SET ECM IP CONFIG (All) ===")
    print(f"Setting IPs:")
    print(f"  Host IP:        {host_ip}")
    print(f"  Device IP:      {dev_ip}")
    print(f"  Subnet Mask:    {subnet}")
    print(f"  Target Host IP: {target_host_ip}")
    print(f"  Target Dev IP:  {target_dev_ip}")

    data = [SUBCMD_SET_ALL]
    data.extend(str_to_ip(host_ip))
    data.extend(str_to_ip(dev_ip))
    data.extend(str_to_ip(subnet))
    data.extend(str_to_ip(target_host_ip))
    data.extend(str_to_ip(target_dev_ip))

    cmd = build_command(0x00, CMD_SET_ECM_IP_CONFIG, data)
    print(f"Sending: {cmd.hex()}")
    ser.write(cmd)
    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 100)
    if response:
        print(f"Received: {response.hex()}")
        result, error = parse_response(response)
        if error:
            print(f"Error: {error}")
            return False

        if result['status'] == 0x00:
            print(f"Success!")
            return True
        else:
            print(f"Error: Status code 0x{result['status']:02X}")
            return False
    else:
        print("Error: No response received")
        return False

def test_reset_default(ser):
    """Test RESET to DEFAULT command"""
    print(f"\n=== Test RESET to DEFAULT ===")
    cmd = build_command(0x00, CMD_SET_ECM_IP_CONFIG, [SUBCMD_RESET_DEFAULT])
    print(f"Sending: {cmd.hex()}")
    ser.write(cmd)
    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 100)
    if response:
        print(f"Received: {response.hex()}")
        result, error = parse_response(response)
        if error:
            print(f"Error: {error}")
            return False

        if result['status'] == 0x00:
            print(f"Success!")
            return True
        else:
            print(f"Error: Status code 0x{result['status']:02X}")
            return False
    else:
        print("Error: No response received")
        return False

def main():
    if len(sys.argv) < 2:
        port = '/dev/ttyUSB0'  # Default for Unix-like systems
        if sys.platform == 'win32':
            port = 'COM7'  # Default for Windows based on our earlier check
        print(f"No port specified, using default: {port}")
    else:
        port = sys.argv[1]

    baudrate = 115200

    print(f"Opening serial port {port} at {baudrate} baud...")
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except Exception as e:
        print(f"Error opening serial port: {e}")
        sys.exit(1)

    print("✓ Serial port opened")
    time.sleep(2)  # Increased wait for device to be ready

    # Clear any pending data
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Send a dummy command to initialize communication state
    try:
        dummy_cmd = build_command(0x00, 0x01, [])  # CMD_GET_INFO
        ser.write(dummy_cmd)
        time.sleep(0.5)
        ser.read(ser.in_waiting or 100)  # Discard response
    except:
        pass  # Ignore errors in dummy command

    all_passed = True

    # Test 1: Get current config
    if not test_get_ecm_ip_config(ser):
        all_passed = False

    # Test 2: Set new config
    if not test_set_ecm_ip_config(
        ser,
        host_ip="192.168.10.1",
        dev_ip="192.168.10.3",
        subnet="255.255.255.0",
        target_host_ip="192.168.10.2",
        target_dev_ip="192.168.10.5"
    ):
        all_passed = False

    # Test 3: Verify new config
    if not test_get_ecm_ip_config(ser):
        all_passed = False

    # Test 4: Reset to default
    if not test_reset_default(ser):
        all_passed = False

    # Test 5: Verify default config
    if not test_get_ecm_ip_config(ser):
        all_passed = False

    ser.close()

    print("\n" + "="*50)
    if all_passed:
        print("✓ ALL TESTS PASSED")
        sys.exit(0)
    else:
        print("✗ SOME TESTS FAILED")
        sys.exit(1)

if __name__ == '__main__':
    main()