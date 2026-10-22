#!/bin/bash

echo "=========================================="
echo "目标主机连接诊断"
echo "=========================================="
echo ""

# 1. 检查ARP表
echo "1. 检查ARP表..."
echo "   192.168.11.1 (Mac):"
arp -a | grep "192.168.11.1" | sed 's/^/   /'
echo ""
echo "   192.168.11.2 (目标主机):"
arp -a | grep "192.168.11.2" | sed 's/^/   /'
echo ""
echo "   192.168.11.3 (设备):"
arp -a | grep "192.168.11.3" | sed 's/^/   /'
echo ""

# 2. 尝试ping目标主机
echo "2. 尝试ping目标主机 (192.168.11.2)..."
ping -c 3 -W 2 192.168.11.2 > /tmp/ping_target.log 2>&1
if [ $? -eq 0 ]; then
    echo "   ✓ ping成功"
    grep "packets received" /tmp/ping_target.log | sed 's/^/   /'
else
    echo "   ✗ ping失败"
    grep "packets transmitted" /tmp/ping_target.log | sed 's/^/   /'
fi
echo ""

# 3. 检查目标主机的ARP状态
echo "3. 分析ARP状态..."
ARP_STATUS=$(arp -a | grep "192.168.11.2" | awk '{print $4}')
if [ "$ARP_STATUS" = "(incomplete)" ]; then
    echo "   ✗ 目标主机ARP状态: incomplete"
    echo "   说明: 目标主机没有响应ARP请求"
    echo "   可能原因:"
    echo "   - 目标主机没有连接到Target端口"
    echo "   - 目标主机没有启用网络接口"
    echo "   - 目标主机没有发送DHCP请求"
    echo "   - 目标主机的网络接口配置有问题"
elif [ "$ARP_STATUS" = "at" ]; then
    echo "   ✓ 目标主机ARP状态: 已解析"
    MAC_ADDR=$(arp -a | grep "192.168.11.2" | awk '{print $4}')
    echo "   MAC地址: $MAC_ADDR"
else
    echo "   ? 目标主机ARP状态: 未知 ($ARP_STATUS)"
fi
echo ""

# 4. 检查设备侧的网络配置
echo "4. 检查设备侧网络配置..."
echo "   尝试通过串口读取ECM IP配置..."
python3 -c "
import serial
import time

try:
    ser = serial.Serial('/dev/cu.usbmodem212201', 115200, timeout=2)
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # 发送GET_ECM_IP命令 (0x27)
    def calculate_checksum(data):
        return sum(data) % 256

    cmd = bytes([0x57, 0xAB, 0x00, 0x27, 0x00])
    checksum = calculate_checksum(cmd)
    cmd += bytes([checksum])

    ser.write(cmd)
    ser.flush()
    time.sleep(1)

    if ser.in_waiting > 0:
        resp = ser.read(ser.in_waiting)
        if len(resp) >= 30 and resp[0] == 0x57 and resp[1] == 0xAB:
            cmd_code = resp[3]
            data_len = resp[4]
            data = resp[5:5+data_len]

            if cmd_code == 0xA7:
                print('   ✓ 成功读取ECM IP配置:')
                print(f'     Host IP: {data[1]}.{data[2]}.{data[3]}.{data[4]}')
                print(f'     Dev IP: {data[5]}.{data[6]}.{data[7]}.{data[8]}')
                print(f'     Subnet: {data[9]}.{data[10]}.{data[11]}.{data[12]}')
                print(f'     Target Host IP: {data[13]}.{data[14]}.{data[15]}.{data[16]}')
                print(f'     Target Dev IP: {data[17]}.{data[18]}.{data[19]}.{data[20]}')
            else:
                print(f'   ✗ 命令响应错误: 0x{cmd_code:02X}')
        else:
            print('   ✗ 响应格式错误')
    else:
        print('   ✗ 串口无响应')

    ser.close()
except Exception as e:
    print(f'   ✗ 串口通信错误: {e}')
" 2>&1 | sed 's/^/   /'
echo ""

# 5. 检查USB设备状态
echo "5. 检查USB设备状态..."
if [ -e "/dev/cu.usbmodem212201" ]; then
    echo "   ✓ 串口设备存在: /dev/cu.usbmodem212201"
else
    echo "   ✗ 串口设备不存在"
fi

if ifconfig en13 > /dev/null 2>&1; then
    echo "   ✓ ECM接口存在: en13"
    IP_ADDR=$(ifconfig en13 | grep "inet " | awk '{print $2}')
    echo "   ECM接口IP: $IP_ADDR"
else
    echo "   ✗ ECM接口不存在"
fi
echo ""

# 6. 诊断总结
echo "6. 诊断总结..."
if [ "$ARP_STATUS" = "(incomplete)" ]; then
    echo ""
    echo "   ⚠️  目标主机无法连接"
    echo ""
    echo "   建议操作:"
    echo "   1. 检查目标主机是否已连接到Target端口"
    echo "   2. 确认目标主机的网络接口已启用"
    echo "   3. 检查目标主机是否配置为DHCP模式"
    echo "   4. 尝试重新插拔目标主机的USB连接"
    echo "   5. 检查目标主机的防火墙设置"
    echo ""
elif [ "$ARP_STATUS" = "at" ]; then
    echo ""
    echo "   ✓ 目标主机已连接"
    echo ""
    echo "   目标主机的MAC地址已解析，说明目标主机已正确连接并获取了IP地址。"
    echo ""
else
    echo ""
    echo "   ? 无法确定目标主机状态"
    echo ""
fi

echo "=========================================="
echo "诊断完成"
echo "=========================================="
