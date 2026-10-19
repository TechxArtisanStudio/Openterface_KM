#!/usr/bin/env python3
import serial
import time

def calculate_checksum(data):
    """Calculate checksum (sum of all bytes)"""
    return sum(data) & 0xFF

def build_command(addr, cmd_code, data):
    """Build CH9329 command frame"""
    frame = [0x57, 0xAB, addr, cmd_code, len(data)] + data
    checksum = calculate_checksum(frame)
    frame.append(checksum)
    return bytes(frame)

def parse_usb_work_mode_response(response):
    """Parse USB work mode query response"""
    if len(response) < 17:
        return None, "Response too short"

    if response[0] != 0x57 or response[1] != 0xAB:
        return None, "Invalid frame header"

    # Response format:
    # 57 AB addr cmd_code data_len status compiled_mode active_mode stored_mode config_status flags(2) capability_mask(4) checksum

    data = response[5:]
    status = data[0]
    compiled_mode = data[1]
    active_mode = data[2]
    stored_mode = data[3]
    config_status = data[4]
    flags = data[5] | (data[6] << 8)
    capability_mask = data[7] | (data[8] << 8) | (data[9] << 16) | (data[10] << 24)

    mode_names = {
        1: 'HID+ACM',
        2: 'HID+UAC+ACM',
        3: 'HID+ACM Bridge',
        4: 'HID+ECM Bridge',
        5: 'HID+UAC+ECM',
        6: 'HID+ACM+ECM'
    }

    return {
        'status': status,
        'compiled_mode': compiled_mode,
        'compiled_mode_name': mode_names.get(compiled_mode, 'Unknown'),
        'active_mode': active_mode,
        'active_mode_name': mode_names.get(active_mode, 'Unknown'),
        'stored_mode': stored_mode,
        'stored_mode_name': mode_names.get(stored_mode, 'Unknown'),
        'config_status': config_status,
        'flags': flags,
        'capability_mask': capability_mask
    }, None

def main():
    port = 'COM5'
    baudrate = 115200

    print(f"Opening {port}...")
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
    except Exception as e:
        print(f"Error opening serial port: {e}")
        return

    print("[OK] Serial port opened")
    time.sleep(0.5)

    # Clear buffers
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Query USB work mode (CMD_USB_WORK_MODE = 0x30)
    # Send query command with sub-command 0x00 (query current mode)
    print("\n=== Query USB Work Mode ===")
    cmd = build_command(0x00, 0x30, [0x00])
    print(f"Sending: {cmd.hex()}")
    ser.write(cmd)
    time.sleep(1)

    response = ser.read(ser.in_waiting or 100)
    if response:
        print(f"Received: {response.hex()}")
        result, error = parse_usb_work_mode_response(response)
        if error:
            print(f"Error: {error}")
        else:
            print(f"\nUSB Work Mode Status:")
            print(f"  Status: 0x{result['status']:02X}")
            print(f"  Compiled Mode: {result['compiled_mode']} ({result['compiled_mode_name']})")
            print(f"  Active Mode: {result['active_mode']} ({result['active_mode_name']})")
            print(f"  Stored Mode: {result['stored_mode']} ({result['stored_mode_name']})")
            print(f"  Config Status: 0x{result['config_status']:02X}")
            print(f"  Flags: 0x{result['flags']:04X}")
            print(f"  Capability Mask: 0x{result['capability_mask']:08X}")

            print(f"\nSupported modes:")
            for i in range(1, 7):
                if result['capability_mask'] & (1 << i):
                    mode_names = {
                        1: 'HID+ACM',
                        2: 'HID+UAC+ACM',
                        3: 'HID+ACM Bridge',
                        4: 'HID+ECM Bridge',
                        5: 'HID+UAC+ECM',
                        6: 'HID+ACM+ECM'
                    }
                    print(f"  [OK] Mode {i}: {mode_names.get(i, 'Unknown')}")
    else:
        print("No response received")

    ser.close()
    print("\n[OK] Done")

if __name__ == '__main__':
    main()
