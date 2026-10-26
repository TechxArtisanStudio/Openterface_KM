#!/usr/bin/env python3
"""
ECM IP 配置修改脚本

用法:
  python3 set_ecm_ip.py <host_ip> <dev_ip> [subnet] [target_host] [target_dev]

示例:
  python3 set_ecm_ip.py 192.168.21.1 192.168.21.3
  python3 set_ecm_ip.py 192.168.50.1 192.168.50.3 255.255.255.0 192.168.50.2 192.168.50.5
  python3 set_ecm_ip.py 192.168.100.1 192.168.100.3

  # 常用网段快捷方式:
  python3 set_ecm_ip.py --preset 20    # 192.168.20.x
  python3 set_ecm_ip.py --preset 21    # 192.168.21.x
  python3 set_ecm_ip.py --preset 50    # 192.168.50.x
  python3 set_ecm_ip.py --preset 60    # 192.168.60.x
  python3 set_ecm_ip.py --preset 100   # 192.168.100.x

  python3 set_ecm_ip.py --get          # 获取当前配置
"""

import serial
import time
import sys
import argparse
import subprocess
import platform

SERIAL_PORT = "/dev/cu.usbmodem212201"
BAUD_RATE = 115200

# 预设网段配置
PRESETS = {
    "20":  ("192.168.20.1", "192.168.20.3", "255.255.255.0", "192.168.20.2", "192.168.20.5"),
    "21":  ("192.168.21.1", "192.168.21.3", "255.255.255.0", "192.168.21.2", "192.168.21.5"),
    "50":  ("192.168.50.1", "192.168.50.3", "255.255.255.0", "192.168.50.2", "192.168.50.5"),
    "60":  ("192.168.60.1", "192.168.60.3", "255.255.255.0", "192.168.60.2", "192.168.60.5"),
    "100": ("192.168.100.1", "192.168.100.3", "255.255.255.0", "192.168.100.2", "192.168.100.5"),
}

def activate_serial(ser):
    """发送0x01命令激活串口"""
    cmd = bytes([0x57, 0xAB, 0x00, 0x01, 0x00, 0x03])
    ser.write(cmd)
    ser.flush()
    time.sleep(0.5)
    ser.read(100)

def get_ip_config(ser):
    """获取当前IP配置"""
    frame = bytes([0x57, 0xAB, 0x00, 0x27, 0x01, 0x00, 0x2A])
    ser.write(frame)
    ser.flush()
    time.sleep(0.5)

    response = ser.read(ser.in_waiting or 100)
    if response and len(response) >= 27 and response[0] == 0x57:
        host_ip = f"{response[6]}.{response[7]}.{response[8]}.{response[9]}"
        dev_ip = f"{response[10]}.{response[11]}.{response[12]}.{response[13]}"
        subnet = f"{response[14]}.{response[15]}.{response[16]}.{response[17]}"
        target_host = f"{response[18]}.{response[19]}.{response[20]}.{response[21]}"
        target_dev = f"{response[22]}.{response[23]}.{response[24]}.{response[25]}"
        return host_ip, dev_ip, subnet, target_host, target_dev
    return None, None, None, None, None

def set_ip_config(ser, host_ip, dev_ip, subnet, target_host, target_dev):
    """设置IP配置"""
    data = [0x00]  # sub_cmd: Set All
    data.extend([int(x) for x in host_ip.split('.')])
    data.extend([int(x) for x in dev_ip.split('.')])
    data.extend([int(x) for x in subnet.split('.')])
    data.extend([int(x) for x in target_host.split('.')])
    data.extend([int(x) for x in target_dev.split('.')])

    frame = [0x57, 0xAB, 0x00, 0x26, len(data)]
    frame.extend(data)
    frame.append(sum(frame) & 0xFF)

    ser.write(bytes(frame))
    ser.flush()
    time.sleep(1)

    response = ser.read(ser.in_waiting or 100)
    if response and len(response) >= 6:
        return response[5] == 0x00
    return False

def validate_ip(ip_str):
    """验证IP地址格式"""
    parts = ip_str.split('.')
    if len(parts) != 4:
        return False
    try:
        return all(0 <= int(p) <= 255 for p in parts)
    except ValueError:
        return False

def main():
    parser = argparse.ArgumentParser(description="ECM IP 配置修改工具")
    parser.add_argument("host_ip", nargs="?", help="主机侧 IP (如 192.168.21.1)")
    parser.add_argument("dev_ip", nargs="?", help="设备侧 IP (如 192.168.21.3)")
    parser.add_argument("subnet", nargs="?", default="255.255.255.0", help="子网掩码 (默认 255.255.255.0)")
    parser.add_argument("target_host", nargs="?", help="目标主机 IP")
    parser.add_argument("target_dev", nargs="?", help="目标设备 IP")
    parser.add_argument("--preset", "-p", help="使用预设网段 (任意数字如 20, 21, 40, 50, 60, 100 等)")
    parser.add_argument("--get", "-g", action="store_true", help="获取当前配置")
    parser.add_argument("--port", default=SERIAL_PORT, help=f"串口设备 (默认: {SERIAL_PORT})")
    parser.add_argument("--auto-dhcp", action="store_true", help="自动刷新 Mac 端 DHCP 租约 (仅 macOS)")

    args = parser.parse_args()

    ser = serial.Serial(args.port, BAUD_RATE, timeout=2)
    activate_serial(ser)

    try:
        if args.get:
            host_ip, dev_ip, subnet, target_host, target_dev = get_ip_config(ser)
            if host_ip:
                print(f"当前配置:")
                print(f"  Host IP:      {host_ip}")
                print(f"  Dev IP:       {dev_ip}")
                print(f"  Subnet Mask:  {subnet}")
                print(f"  Target Host:  {target_host}")
                print(f"  Target Dev:   {target_dev}")
            else:
                print("获取配置失败")
            return

        if args.preset:
            # 支持任意网段数字
            try:
                subnet_num = int(args.preset)
                if not (0 <= subnet_num <= 255):
                    print(f"错误: 网段号必须在 0-255 之间，当前: {subnet_num}")
                    return
                host_ip = f"192.168.{subnet_num}.1"
                dev_ip = f"192.168.{subnet_num}.3"
                subnet = "255.255.255.0"
                target_host = f"192.168.{subnet_num}.2"
                target_dev = f"192.168.{subnet_num}.5"
                print(f"使用网段: 192.168.{subnet_num}.x")
            except ValueError:
                print(f"错误: 网段号必须是数字，当前: {args.preset}")
                return
        elif args.host_ip and args.dev_ip:
            host_ip = args.host_ip
            dev_ip = args.dev_ip
            subnet = args.subnet
            # 自动推导 target_host 和 target_dev
            if args.target_host:
                target_host = args.target_host
            else:
                # target_host = host_ip 最后一位改为 .2
                parts = host_ip.split('.')
                parts[-1] = '2'
                target_host = '.'.join(parts)

            if args.target_dev:
                target_dev = args.target_dev
            else:
                # target_dev = dev_ip 最后一位改为 .5
                parts = dev_ip.split('.')
                parts[-1] = '5'
                target_dev = '.'.join(parts)
        else:
            parser.print_help()
            return

        # 验证 IP 格式
        for ip in [host_ip, dev_ip, subnet, target_host, target_dev]:
            if not validate_ip(ip):
                print(f"错误: 无效的 IP 格式 - {ip}")
                return

        print(f"设置配置:")
        print(f"  Host IP:      {host_ip}")
        print(f"  Dev IP:       {dev_ip}")
        print(f"  Subnet Mask:  {subnet}")
        print(f"  Target Host:  {target_host}")
        print(f"  Target Dev:   {target_dev}")

        success = set_ip_config(ser, host_ip, dev_ip, subnet, target_host, target_dev)

        if success:
            print("\n✓ 设置成功")
            time.sleep(0.5)
            host_ip, dev_ip, subnet, target_host, target_dev = get_ip_config(ser)
            if host_ip:
                print(f"\n验证配置:")
                print(f"  Host IP: {host_ip}")
                print(f"  Dev IP:  {dev_ip}")

            # 自动刷新 DHCP 租约
            if args.auto_dhcp and platform.system() == "Darwin":
                print("\n正在刷新 Mac 端 DHCP 租约...")
                try:
                    result = subprocess.run(
                        ["networksetup", "-setdhcp", "Openterface Host"],
                        capture_output=True,
                        text=True,
                        timeout=10
                    )
                    if result.returncode == 0:
                        print("✓ DHCP 刷新成功，等待获取新 IP...")
                        time.sleep(3)
                        # 显示当前 IP
                        result = subprocess.run(
                            ["ifconfig", "en13"],
                            capture_output=True,
                            text=True
                        )
                        for line in result.stdout.split('\n'):
                            if 'inet ' in line:
                                print(f"  {line.strip()}")
                    else:
                        print(f"✗ DHCP 刷新失败: {result.stderr}")
                except Exception as e:
                    print(f"✗ DHCP 刷新异常: {e}")
            else:
                print("\n提示: 运行以下命令刷新 Mac 端 DHCP 租约:")
                print("  networksetup -setdhcp \"Openterface Host\"")
                print("  或使用 --auto-dhcp 参数自动刷新")
        else:
            print("\n✗ 设置失败")

    finally:
        ser.close()

if __name__ == "__main__":
    main()