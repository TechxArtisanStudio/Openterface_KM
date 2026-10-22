#!/usr/bin/env python3
"""
ECM IP Configuration Tool via CH9329 Serial Protocol
Usage: python3 set_ecm_ip.py [get|set|reset] [args...]
"""

import sys
import serial
import time

# Configuration
SERIAL_PORT = "/dev/cu.usbmodem212201"
BAUD_RATE = 9600
TIMEOUT = 1.0

# CH9329 protocol constants
FRAME_HEAD1 = 0x57
FRAME_HEAD2 = 0xAB
CMD_SET_ECM_IP = 0x26
CMD_GET_ECM_IP = 0x27
ADDR = 0x00

# Sub-commands
SUB_CMD_SET_ALL = 0x00
SUB_CMD_SET_HOST_SIDE = 0x01
SUB_CMD_SET_TARGET_SIDE = 0x02
SUB_CMD_RESET_DEFAULT = 0x03
SUB_CMD_GET_ALL = 0x00

def calc_checksum(data):
    """Calculate checksum (sum of all bytes mod 256)"""
    return sum(data) & 0xFF

def build_frame(cmd_code, data_bytes):
    """Build CH9329 frame"""
    # Frame: HEAD1 HEAD2 ADDR CMD DATA_LEN DATA... CHECKSUM
    frame = [FRAME_HEAD1, FRAME_HEAD2, ADDR, cmd_code, len(data_bytes)]
    frame.extend(data_bytes)
    frame.append(calc_checksum(frame))
    return bytes(frame)

def parse_ip(ip_str):
    """Parse IP address string to list of 4 bytes"""
    parts = ip_str.split('.')
    if len(parts) != 4:
        raise ValueError(f"Invalid IP address: {ip_str}")
    return [int(p) for p in parts]

def format_ip(ip_bytes):
    """Format IP bytes to string"""
    return '.'.join(str(b) for b in ip_bytes)

def send_command(ser, frame):
    """Send command and wait for response"""
    print(f"Sending: {frame.hex().upper()}")
    ser.write(frame)
    ser.flush()
    time.sleep(0.5)

    # Read response
    response = ser.read(ser.in_waiting or 1)
    if response:
        print(f"Response: {response.hex().upper()}")
        return response
    return None

def parse_response(response):
    """Parse CH9329 response"""
    if not response or len(response) < 6:
        return None, None, None

    if response[0] != FRAME_HEAD1 or response[1] != FRAME_HEAD2:
        return None, None, None

    addr = response[2]
    cmd_code = response[3]
    data_len = response[4]
    data = response[5:5+data_len]
    checksum = response[5+data_len]

    # Verify checksum
    expected_checksum = calc_checksum(response[:5+data_len])
    if checksum != expected_checksum:
        print(f"Checksum mismatch: expected {expected_checksum:02X}, got {checksum:02X}")
        return None, None, None

    return cmd_code, data, checksum

def get_ip_config(ser):
    """Get current IP configuration"""
    print("=" * 50)
    print("获取当前ECM IP配置")
    print("=" * 50)

    # Build GET command: sub_cmd = 0x00 (Get All)
    frame = build_frame(CMD_GET_ECM_IP, [SUB_CMD_GET_ALL])
    response = send_command(ser, frame)

    if response:
        cmd_code, data, _ = parse_response(response)
        if cmd_code == CMD_GET_ECM_IP and data and len(data) >= 21:
            status = data[0]
            if status == 0x00:  # STATUS_SUCCESS
                host_ip = data[1:5]
                dev_ip = data[5:9]
                subnet = data[9:13]
                target_host = data[13:17]
                target_dev = data[17:21]

                print(f"\n当前配置:")
                print(f"  Host IP:        {format_ip(host_ip)}")
                print(f"  Dev IP:         {format_ip(dev_ip)}")
                print(f"  Subnet Mask:    {format_ip(subnet)}")
                print(f"  Target Host IP: {format_ip(target_host)}")
                print(f"  Target Dev IP:  {format_ip(target_dev)}")
                return True
            else:
                print(f"错误: 设备返回状态码 0x{status:02X}")
        else:
            print("错误: 无法解析响应")
    return False

def set_ip_config(ser, host_ip, dev_ip, subnet, target_host, target_dev):
    """Set all IP configuration"""
    print("=" * 50)
    print("设置ECM IP配置")
    print("=" * 50)
    print(f"  Host IP:        {host_ip}")
    print(f"  Dev IP:         {dev_ip}")
    print(f"  Subnet Mask:    {subnet}")
    print(f"  Target Host IP: {target_host}")
    print(f"  Target Dev IP:  {target_dev}")
    print("=" * 50)

    # Parse IP addresses
    host = parse_ip(host_ip)
    dev = parse_ip(dev_ip)
    sub = parse_ip(subnet)
    thost = parse_ip(target_host)
    tdev = parse_ip(target_dev)

    # Build SET command: sub_cmd = 0x00 (Set All)
    # Data: sub_cmd(1) + host_ip(4) + dev_ip(4) + subnet(4) + target_host(4) + target_dev(4) = 21 bytes
    data = [SUB_CMD_SET_ALL]
    data.extend(host)
    data.extend(dev)
    data.extend(sub)
    data.extend(thost)
    data.extend(tdev)

    frame = build_frame(CMD_SET_ECM_IP, data)
    response = send_command(ser, frame)

    if response:
        cmd_code, resp_data, _ = parse_response(response)
        if cmd_code == CMD_SET_ECM_IP and resp_data:
            status = resp_data[0]
            if status == 0x00:  # STATUS_SUCCESS
                print("\n✓ 配置设置成功！")
                print("请重新连接ECM接口以应用新配置。")
                return True
            else:
                print(f"\n✗ 错误: 设备返回状态码 0x{status:02X}")
                if status == 0xE5:
                    print("  原因: 参数无效（IP地址不在同一子网或格式错误）")
                elif status == 0xE9:
                    print("  原因: Flash擦除失败")
                elif status == 0xEA:
                    print("  原因: Flash编程失败")
                elif status == 0xEB:
                    print("  原因: Flash验证失败")
        else:
            print("\n✗ 错误: 无法解析响应")
    return False

def reset_ip_config(ser):
    """Reset to default configuration"""
    print("=" * 50)
    print("重置ECM IP配置为默认值")
    print("=" * 50)

    # Build RESET command: sub_cmd = 0x03 (Reset to Default)
    frame = build_frame(CMD_SET_ECM_IP, [SUB_CMD_RESET_DEFAULT])
    response = send_command(ser, frame)

    if response:
        cmd_code, resp_data, _ = parse_response(response)
        if cmd_code == CMD_SET_ECM_IP and resp_data:
            status = resp_data[0]
            if status == 0x00:  # STATUS_SUCCESS
                print("\n✓ 配置已重置为默认值！")
                print("默认配置:")
                print("  Host IP:        192.168.20.1")
                print("  Dev IP:         192.168.20.3")
                print("  Subnet Mask:    255.255.255.0")
                print("  Target Host IP: 192.168.20.2")
                print("  Target Dev IP:  192.168.20.5")
                return True
            else:
                print(f"\n✗ 错误: 设备返回状态码 0x{status:02X}")
    return False

def main():
    if len(sys.argv) < 2:
        print("用法: python3 set_ecm_ip.py <command> [args...]")
        print()
        print("命令:")
        print("  get                                    - 获取当前IP配置")
        print("  set <host> <dev> <subnet> <target_host> <target_dev>")
        print("                                         - 设置所有IP配置")
        print("  reset                                  - 重置为默认配置")
        print()
        print("示例:")
        print("  python3 set_ecm_ip.py get")
        print("  python3 set_ecm_ip.py set 192.168.20.1 192.168.20.3 255.255.255.0 192.168.20.2 192.168.20.5")
        print("  python3 set_ecm_ip.py reset")
        sys.exit(1)

    command = sys.argv[1]

    # Open serial port
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
        print(f"已连接到串口 {SERIAL_PORT}")
    except serial.SerialException as e:
        print(f"错误: 无法打开串口 {SERIAL_PORT}: {e}")
        sys.exit(1)

    try:
        if command == "get":
            get_ip_config(ser)
        elif command == "set":
            if len(sys.argv) != 7:
                print("错误: set命令需要5个IP地址参数")
                print("用法: python3 set_ecm_ip.py set <host> <dev> <subnet> <target_host> <target_dev>")
                sys.exit(1)
            set_ip_config(ser, sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
        elif command == "reset":
            reset_ip_config(ser)
        else:
            print(f"错误: 未知命令 '{command}'")
            sys.exit(1)
    finally:
        ser.close()

if __name__ == "__main__":
    main()
