#!/bin/bash

echo "=========================================="
echo "ECM接口连通性测试"
echo "=========================================="
echo ""

# 检查ECM接口
echo "1. 检查ECM网络接口..."
EN13_INFO=$(ifconfig en13 2>/dev/null)
if [ $? -eq 0 ]; then
    echo "✓ en13接口存在"
    echo "$EN13_INFO" | grep "inet " | awk '{print "  主机IP:", $2}'
    echo "$EN13_INFO" | grep "ether" | awk '{print "  MAC地址:", $2}'
    echo "$EN13_INFO" | grep "status" | awk '{print "  状态:", $2}'
else
    echo "✗ en13接口不存在"
    exit 1
fi
echo ""

# 检查ARP表
echo "2. 检查ARP表..."
arp -a | grep en13 | while read line; do
    echo "  $line"
done
echo ""

# 测试ping设备IP (192.168.20.3)
echo "3. 测试ping设备IP (192.168.20.3)..."
ping -c 3 -W 2 192.168.20.3 > /tmp/ping_dev.log 2>&1
if [ $? -eq 0 ]; then
    echo "✓ ping 192.168.20.3 成功"
    grep "packets received" /tmp/ping_dev.log | awk '{print "  " $0}'
else
    echo "✗ ping 192.168.20.3 失败"
    grep "packets transmitted" /tmp/ping_dev.log | awk '{print "  " $0}'
fi
echo ""

# 测试ping目标主机IP (192.168.20.2)
echo "4. 测试ping目标主机IP (192.168.20.2)..."
ping -c 3 -W 2 192.168.20.2 > /tmp/ping_target.log 2>&1
if [ $? -eq 0 ]; then
    echo "✓ ping 192.168.20.2 成功"
    grep "packets received" /tmp/ping_target.log | awk '{print "  " $0}'
else
    echo "✗ ping 192.168.20.2 失败"
    grep "packets transmitted" /tmp/ping_target.log | awk '{print "  " $0}'
fi
echo ""

# 测试SSH连接
echo "5. 测试SSH连接..."
if [ -f "test_ssh.sh" ]; then
    chmod +x test_ssh.sh
    ./test_ssh.sh > /tmp/ssh_test.log 2>&1
    if [ $? -eq 0 ]; then
        echo "✓ SSH连接成功"
        grep "SSH connection successful" /tmp/ssh_test.log
    else
        echo "✗ SSH连接失败"
        tail -5 /tmp/ssh_test.log | sed 's/^/  /'
    fi
else
    echo "✗ test_ssh.sh不存在"
fi
echo ""

# 检查串口设备
echo "6. 检查串口设备..."
if [ -e "/dev/cu.usbmodem212201" ]; then
    echo "✓ 串口设备存在: /dev/cu.usbmodem212201"
else
    echo "✗ 串口设备不存在"
fi
echo ""

echo "=========================================="
echo "测试完成"
echo "=========================================="
