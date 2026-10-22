#!/bin/bash

# ECM IP Configuration Tool via CH9329 Serial Protocol
# Usage: ./set_ecm_ip.sh [get|set|reset] [args...]

SERIAL_PORT="/dev/cu.usbmodem212201"
BAUD_RATE=9600

# CH9329 protocol constants
FRAME_HEAD1=0x57
FRAME_HEAD2=0xAB
CMD_SET_ECM_IP=0x26
CMD_GET_ECM_IP=0x27
ADDR=0x00

# Calculate checksum (sum of all bytes except the checksum byte itself)
calc_checksum() {
    local sum=0
    for byte in "$@"; do
        sum=$(( (sum + byte) & 0xFF ))
    done
    echo $sum
}

# Build CH9329 frame
# Args: cmd_code data_len data_bytes...
build_frame() {
    local cmd_code=$1
    local data_len=$2
    shift 2
    local data_bytes=("$@")

    # Frame: HEAD1 HEAD2 ADDR CMD DATA_LEN DATA... CHECKSUM
    local frame=($FRAME_HEAD1 $FRAME_HEAD2 $ADDR $cmd_code $data_len)
    frame+=("${data_bytes[@]}")

    # Calculate checksum
    local checksum=$(calc_checksum "${frame[@]}")
    frame+=($checksum)

    # Output as hex string
    local hex_str=""
    for byte in "${frame[@]}"; do
        hex_str+=$(printf "%02X" $byte)
    done
    echo "$hex_str"
}

# Send command via serial port
send_command() {
    local hex_data=$1
    echo "Sending: $hex_data"

    # Convert hex to binary and send
    echo -ne "\\x${hex_data:0:2}\\x${hex_data:2:2}\\x${hex_data:4:2}\\x${hex_data:6:2}\\x${hex_data:8:2}" > "$SERIAL_PORT"

    # Send remaining bytes
    local i=10
    while [ $i -lt ${#hex_data} ]; do
        echo -ne "\\x${hex_data:$i:2}" > "$SERIAL_PORT"
        i=$((i + 2))
    done
}

# Get current IP configuration
get_ip_config() {
    echo "=========================================="
    echo "获取当前ECM IP配置"
    echo "=========================================="

    # Build GET command: sub_cmd = 0x00 (Get All)
    local frame=$(build_frame $CMD_GET_ECM_IP 1 0)
    send_command "$frame"

    # Wait for response
    sleep 0.5

    # Read response (this is simplified - actual implementation would parse response)
    echo "请检查串口输出以查看响应"
}

# Set all IP configuration
# Args: host_ip dev_ip subnet_mask target_host_ip target_dev_ip
set_ip_config() {
    local host_ip=$1
    local dev_ip=$2
    local subnet=$3
    local target_host=$4
    local target_dev=$5

    echo "=========================================="
    echo "设置ECM IP配置"
    echo "=========================================="
    echo "Host IP:        $host_ip"
    echo "Dev IP:         $dev_ip"
    echo "Subnet Mask:    $subnet"
    echo "Target Host IP: $target_host"
    echo "Target Dev IP:  $target_dev"
    echo "=========================================="

    # Parse IP addresses
    IFS='.' read -r -a host_arr <<< "$host_ip"
    IFS='.' read -r -a dev_arr <<< "$dev_ip"
    IFS='.' read -r -a subnet_arr <<< "$subnet"
    IFS='.' read -r -a target_host_arr <<< "$target_host"
    IFS='.' read -r -a target_dev_arr <<< "$target_dev"

    # Build SET command: sub_cmd = 0x00 (Set All)
    # Data: sub_cmd(1) + host_ip(4) + dev_ip(4) + subnet(4) + target_host(4) + target_dev(4) = 21 bytes
    local data=(0x00)  # sub_cmd
    data+=(${host_arr[0]} ${host_arr[1]} ${host_arr[2]} ${host_arr[3]})
    data+=(${dev_arr[0]} ${dev_arr[1]} ${dev_arr[2]} ${dev_arr[3]})
    data+=(${subnet_arr[0]} ${subnet_arr[1]} ${subnet_arr[2]} ${subnet_arr[3]})
    data+=(${target_host_arr[0]} ${target_host_arr[1]} ${target_host_arr[2]} ${target_host_arr[3]})
    data+=(${target_dev_arr[0]} ${target_dev_arr[1]} ${target_dev_arr[2]} ${target_dev_arr[3]})

    local frame=$(build_frame $CMD_SET_ECM_IP 21 "${data[@]}")
    send_command "$frame"

    echo "命令已发送，等待响应..."
    sleep 1
    echo "完成！请重新连接ECM接口以应用新配置。"
}

# Reset to default configuration
reset_ip_config() {
    echo "=========================================="
    echo "重置ECM IP配置为默认值"
    echo "=========================================="

    # Build RESET command: sub_cmd = 0x03 (Reset to Default)
    local frame=$(build_frame $CMD_SET_ECM_IP 1 3)
    send_command "$frame"

    echo "命令已发送，等待响应..."
    sleep 1
    echo "完成！配置已重置为默认值。"
}

# Show usage
show_usage() {
    echo "用法: $0 <command> [args...]"
    echo ""
    echo "命令:"
    echo "  get                                    - 获取当前IP配置"
    echo "  set <host> <dev> <subnet> <target_host> <target_dev>"
    echo "                                         - 设置所有IP配置"
    echo "  reset                                  - 重置为默认配置"
    echo ""
    echo "示例:"
    echo "  $0 get"
    echo "  $0 set 192.168.20.1 192.168.20.3 255.255.255.0 192.168.20.2 192.168.20.5"
    echo "  $0 reset"
}

# Main
case "$1" in
    get)
        get_ip_config
        ;;
    set)
        if [ $# -ne 6 ]; then
            echo "错误: set命令需要5个参数"
            show_usage
            exit 1
        fi
        set_ip_config "$2" "$3" "$4" "$5" "$6"
        ;;
    reset)
        reset_ip_config
        ;;
    *)
        show_usage
        exit 1
        ;;
esac
