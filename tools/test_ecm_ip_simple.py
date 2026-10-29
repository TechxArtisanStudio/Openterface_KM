#!/usr/bin/env python3
"""简单的 ECM IP 配置测试脚本"""

import serial
import time
import sys

SERIAL_PORT = "/dev/cu.usbmodem211201"
BAUD_RATE = 115200

def send_command(ser, cmd_bytes, wait_time=0.5):
    """发送命令并等待响应"""
    ser.reset_input_buffer()
    ser.write(cmd_bytes)
    ser.flush()
    time.sleep(wait_time)

    response = b""
    while ser.in_waiting > 0:
        response += ser.read(ser.in_waiting)
        time.sleep(0.1)

    return response

def main():
    print("=" * 60)
    print("ECM IP 配置测试 (串口)")
    print("=" * 60)

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2)
        print(f"\n✓ 已打开串口: {SERIAL_PORT}")
    except Exception as e:
        print(f"\n✗ 打开串口失败: {e}")
        return

    try:
        # 1. 激活串口
        print("\n[1] 激活串口...")
        activate_cmd = bytes([0x57, 0xAB, 0x00, 0x01, 0x00, 0x03])
        response = send_command(ser, activate_cmd, 0.5)
        print(f"   响应: {response.hex() if response else '无'}")

        # 2. 获取当前 IP 配置
        print("\n[2] 获取当前 IP 配置...")
        get_cmd = bytes([0x57, 0xAB, 0x00, 0x27, 0x01, 0x00, 0x2A])
        response = send_command(ser, get_cmd, 1.0)

        if response and len(response) >= 27 and response[0] == 0x57:
            print(f"   响应: {response.hex()}")
            host_ip = f"{response[6]}.{response[7]}.{response[8]}.{response[9]}"
            dev_ip = f"{response[10]}.{response[11]}.{response[12]}.{response[13]}"
            subnet = f"{response[14]}.{response[15]}.{response[16]}.{response[17]}"
            target_host = f"{response[18]}.{response[19]}.{response[20]}.{response[21]}"
            target_dev = f"{response[22]}.{response[23]}.{response[24]}.{response[25]}"

            print(f"\n   当前配置:")
            print(f"     Host IP:      {host_ip}")
            print(f"     Dev IP:       {dev_ip}")
            print(f"     Subnet:       {subnet}")
            print(f"     Target Host:  {target_host}")
            print(f"     Target Dev:   {target_dev}")
        else:
            print(f"   ✗ 获取失败，响应: {response.hex() if response else '无'}")

        # 3. 设置 IP 到 20 网段
        print("\n[3] 设置 IP 到 20 网段...")
        data = [0x00]  # sub_cmd: Set All
        data.extend([192, 168, 20, 1])    # Host IP
        data.extend([192, 168, 20, 3])    # Dev IP
        data.extend([255, 255, 255, 0])   # Subnet
        data.extend([192, 168, 20, 2])    # Target Host
        data.extend([192, 168, 20, 5])    # Target Dev

        frame = [0x57, 0xAB, 0x00, 0x26, len(data)]
        frame.extend(data)
        frame.append(sum(frame) & 0xFF)

        set_cmd = bytes(frame)
        print(f"   发送: {set_cmd.hex()}")
        response = send_command(ser, set_cmd, 1.0)

        if response and len(response) >= 6:
            print(f"   响应: {response.hex()}")
            if response[5] == 0x00:
                print("   ✓ 设置成功")
            else:
                print(f"   ✗ 设置失败，状态码: 0x{response[5]:02X}")
        else:
            print(f"   ✗ 设置失败，响应: {response.hex() if response else '无'}")

        # 4. 验证新配置
        print("\n[4] 验证新配置...")
        time.sleep(0.5)
        response = send_command(ser, get_cmd, 1.0)

        if response and len(response) >= 27 and response[0] == 0x57:
            host_ip = f"{response[6]}.{response[7]}.{response[8]}.{response[9]}"
            dev_ip = f"{response[10]}.{response[11]}.{response[12]}.{response[13]}"

            print(f"   新配置:")
            print(f"     Host IP: {host_ip}")
            print(f"     Dev IP:  {dev_ip}")

            if host_ip == "192.168.20.1" and dev_ip == "192.168.20.3":
                print("   ✓ 配置验证成功")
            else:
                print("   ✗ 配置验证失败")
        else:
            print(f"   ✗ 验证失败，响应: {response.hex() if response else '无'}")

        # 5. 提示刷新 DHCP
        print("\n[5] 刷新 Mac 端 DHCP 租约...")
        print("   请运行: networksetup -setdhcp \"Openterface Host\"")

    except Exception as e:
        print(f"\n✗ 错误: {e}")
        import traceback
        traceback.print_exc()

    finally:
        ser.close()
        print("\n✓ 已关闭串口")

if __name__ == "__main__":
    main()
