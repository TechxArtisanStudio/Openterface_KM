#!/bin/bash

echo "=========================================="
echo "ECM IP配置诊断"
echo "=========================================="
echo ""

# 1. 检查当前网络接口
echo "1. 当前ECM接口状态:"
ifconfig en13 2>/dev/null | grep "inet " | awk '{print "   IP:", $2}'
ifconfig en13 2>/dev/null | grep "ether" | awk '{print "   MAC:", $2}'
echo ""

# 2. 检查串口
echo "2. 串口状态:"
if [ -e "/dev/cu.usbmodem212201" ]; then
    echo "   ✓ 串口设备存在"
else
    echo "   ✗ 串口设备不存在"
fi
echo ""

# 3. 测试旧IP
echo "3. 测试旧IP (192.168.11.3):"
ping -c 1 -W 1 192.168.11.3 > /dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "   ✓ 旧IP可以ping通"
    echo "   → Flash中保存的是旧配置"
else
    echo "   ✗ 旧IP无法ping通"
fi
echo ""

# 4. 测试新IP
echo "4. 测试新IP (192.168.20.3):"
ping -c 1 -W 1 192.168.20.3 > /dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "   ✓ 新IP可以ping通"
    echo "   → 配置已成功更新"
else
    echo "   ✗ 新IP无法ping通"
fi
echo ""

echo "=========================================="
echo "诊断结果:"
echo "=========================================="

# 判断当前状态
OLD_IP_WORKS=0
NEW_IP_WORKS=0

ping -c 1 -W 1 192.168.11.3 > /dev/null 2>&1 && OLD_IP_WORKS=1
ping -c 1 -W 1 192.168.20.3 > /dev/null 2>&1 && NEW_IP_WORKS=1

if [ $NEW_IP_WORKS -eq 1 ]; then
    echo "✓ 配置已成功更新为新IP (192.168.20.x)"
elif [ $OLD_IP_WORKS -eq 1 ]; then
    echo "⚠ Flash中保存的是旧配置 (192.168.11.x)"
    echo ""
    echo "解决方案:"
    echo "1. 重新烧录固件时清除Flash配置"
    echo "2. 或者通过串口命令设置新IP"
    echo "3. 或者修改代码强制使用默认配置"
else
    echo "⚠ 新旧IP都无法ping通"
    echo ""
    echo "可能原因:"
    echo "1. USB需要重新枚举（拔插USB线）"
    echo "2. 设备未正确启动"
    echo "3. ECM接口未正确初始化"
fi

echo "=========================================="
