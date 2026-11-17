#!/usr/bin/env python3
"""
Debug script to read memory from Openterface target via USB vendor control request.

Usage:
    python3 debug_memory_dump.py [--addr ADDR] [--len LEN] [--all]

Examples:
    python3 debug_memory_dump.py --all
    python3 debug_memory_dump.py --addr 0x20000458 --len 16
"""

import usb.core
import usb.util
import sys
import argparse

# Target device VID:PID
VID = 0x1a86
PID = 0xfe00

# USB vendor request for memory read
REQ_MEMORY_READ = 0x50
BM_REQUEST_TYPE = 0xC1  # Vendor, device-to-host

# Key addresses
ADDR_USBFS_ENDP_BUSY = 0x20000458
ADDR_SYSTICK_MS = 0x20000580
ADDR_KB_QUEUE = 0x20007824
ADDR_USBFS_BASE = 0x50000000

# USBFS register offsets (from struct USBFSD_TypeDef)
OFF_BASE_CTRL = 0x00
OFF_INT_EN = 0x02
OFF_MIS_ST = 0x05
OFF_INT_FG = 0x06
OFF_INT_ST = 0x07
OFF_UEP4_1_MOD = 0x0C
OFF_UEP2_3_MOD = 0x0D
OFF_UEP1_TX_LEN = 0x34
OFF_UEP1_TX_CTRL = 0x36
OFF_UEP2_TX_LEN = 0x38
OFF_UEP2_TX_CTRL = 0x3A
OFF_UEP3_TX_LEN = 0x3C
OFF_UEP3_TX_CTRL = 0x3E
OFF_UEP4_TX_LEN = 0x40
OFF_UEP4_TX_CTRL = 0x42
OFF_UEP7_TX_LEN = 0x4C
OFF_UEP7_TX_CTRL = 0x4E


def read_memory(dev, addr, length):
    """Read memory via USB vendor control request."""
    wValue = (addr >> 16) & 0xFFFF
    wIndex = addr & 0xFFFF
    try:
        data = dev.ctrl_transfer(BM_REQUEST_TYPE, REQ_MEMORY_READ, wValue, wIndex, length)
        return data
    except usb.core.USBError as e:
        print(f"USB error reading 0x{addr:08X}: {e}", file=sys.stderr)
        return None


def print_hex(data, addr):
    """Print data as hex dump."""
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ' '.join(f'{b:02X}' for b in chunk)
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'  {addr+i:08X}: {hex_str:<48} {ascii_str}')


def print_register(name, value, bits_desc=None):
    """Print a register value with optional bit descriptions."""
    print(f'  {name}: 0x{value:02X} ({value:08b})')
    if bits_desc:
        for bit, desc in bits_desc.items():
            state = (value >> bit) & 1
            print(f'    bit {bit}: {state} - {desc}')


def dump_all(dev):
    """Dump all key variables and registers."""
    print('=' * 80)
    print('KEY VARIABLES')
    print('=' * 80)

    # systick_ms (4 bytes, volatile counter)
    print('\n[systick_ms @ 0x{:08X}]'.format(ADDR_SYSTICK_MS))
    data = read_memory(dev, ADDR_SYSTICK_MS, 4)
    if data:
        value = int.from_bytes(data, 'little')
        print(f'  value: {value} ms ({value/1000:.2f} s)')

    # USBFS_Endp_Busy (8 bytes, array of endpoint busy flags)
    print('\n[USBFS_Endp_Busy @ 0x{:08X}]'.format(ADDR_USBFS_ENDP_BUSY))
    data = read_memory(dev, ADDR_USBFS_ENDP_BUSY, 8)
    if data:
        for i in range(8):
            print(f'  EP{i}: {data[i]} ({"BUSY" if data[i] else "IDLE"})')

    # kb_queue (check head/tail pointers)
    print('\n[kb_queue @ 0x{:08X}]'.format(ADDR_KB_QUEUE))
    data = read_memory(dev, ADDR_KB_QUEUE, 16)
    if data:
        head = int.from_bytes(data[0:4], 'little')
        tail = int.from_bytes(data[4:8], 'little')
        size = int.from_bytes(data[8:12], 'little')
        capacity = int.from_bytes(data[12:16], 'little')
        print(f'  head: {head} (offset 0x{head:08X})')
        print(f'  tail: {tail} (offset 0x{tail:08X})')
        print(f'  size: {size}')
        print(f'  capacity: {capacity}')
        print(f'  queue full: {size >= capacity}')
        print(f'  queue empty: {head == tail}')

    print('\n' + '=' * 80)
    print('USBFS REGISTERS')
    print('=' * 80)

    # BASE_CTRL
    print('\n[BASE_CTRL @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_BASE_CTRL))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_BASE_CTRL, 1)
    if data:
        print_register('BASE_CTRL', data[0], {
            7: 'FORCE_RESET',
            6: 'INT_BUSY',
            5: 'DEV_PUEN',
            4: 'RD_MOD',
            3: 'IE_TOGGLE',
            2: 'IE_TRANSFER',
            1: 'IE_DETECT',
            0: 'IE_BUS_RST'
        })

    # INT_EN
    print('\n[INT_EN @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_INT_EN))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_INT_EN, 1)
    if data:
        print_register('INT_EN', data[0], {
            7: 'IE_DEV_SOF',
            6: 'IE_FIFO_OV',
            5: 'IE_SUSPEND',
            4: 'IE_TRANSFER',
            3: 'IE_DETECT',
            2: 'IE_BUS_RST',
            1: 'reserved',
            0: 'reserved'
        })

    # MIS_ST
    print('\n[MIS_ST @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_MIS_ST))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_MIS_ST, 1)
    if data:
        print_register('MIS_ST', data[0], {
            7: 'UMS_SOF_PRES',
            6: 'UMS_SOF_ACT',
            5: 'UMS_SIE_FREE',
            4: 'UMS_R_FIFO_RDY',
            3: 'UMS_BUS_RESET',
            2: 'UMS_SUSPEND',
            1: 'UMS_DM_LEVEL',
            0: 'UMS_DEV_ATTACH'
        })

    # INT_FG
    print('\n[INT_FG @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_INT_FG))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_INT_FG, 1)
    if data:
        print_register('INT_FG', data[0], {
            7: 'U_IS_NAK',
            6: 'U_TOG_OK',
            5: 'U_SIE_FREE',
            4: 'UIF_FIFO_OV',
            3: 'UIF_HST_SOF',
            2: 'UIF_SUSPEND',
            1: 'UIF_TRANSFER',
            0: 'UIF_BUS_RST'
        })

    # INT_ST
    print('\n[INT_ST @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_INT_ST))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_INT_ST, 1)
    if data:
        print_register('INT_ST', data[0], {
            7: 'UIS_IS_NAK',
            6: 'UIS_TOG_OK',
            5: 'UIS_TOKEN1',
            4: 'UIS_TOKEN0',
            3: 'UIS_ENDP3',
            2: 'UIS_ENDP2',
            1: 'UIS_ENDP1',
            0: 'UIS_ENDP0'
        })

    # UEP4_1_MOD
    print('\n[UEP4_1_MOD @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_UEP4_1_MOD))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_UEP4_1_MOD, 1)
    if data:
        print_register('UEP4_1_MOD', data[0], {
            7: 'UEP1_RX_EN',
            6: 'UEP1_TX_EN',
            5: 'reserved',
            4: 'UEP1_BUF_MOD',
            3: 'UEP4_RX_EN',
            2: 'UEP4_TX_EN',
            1: 'reserved',
            0: 'UEP4_BUF_MOD'
        })

    # UEP2_3_MOD
    print('\n[UEP2_3_MOD @ 0x{:08X}]'.format(ADDR_USBFS_BASE + OFF_UEP2_3_MOD))
    data = read_memory(dev, ADDR_USBFS_BASE + OFF_UEP2_3_MOD, 1)
    if data:
        print_register('UEP2_3_MOD', data[0], {
            7: 'UEP3_RX_EN',
            6: 'UEP3_TX_EN',
            5: 'reserved',
            4: 'UEP3_BUF_MOD',
            3: 'UEP2_RX_EN',
            2: 'UEP2_TX_EN',
            1: 'reserved',
            0: 'UEP2_BUF_MOD'
        })

    # Endpoint TX control registers
    for ep_num, tx_ctrl_off, tx_len_off in [
        (1, OFF_UEP1_TX_CTRL, OFF_UEP1_TX_LEN),
        (2, OFF_UEP2_TX_CTRL, OFF_UEP2_TX_LEN),
        (3, OFF_UEP3_TX_CTRL, OFF_UEP3_TX_LEN),
        (4, OFF_UEP4_TX_CTRL, OFF_UEP4_TX_LEN),
        (7, OFF_UEP7_TX_CTRL, OFF_UEP7_TX_LEN),
    ]:
        print(f'\n[EP{ep_num} TX registers]')

        # TX_LEN
        data = read_memory(dev, ADDR_USBFS_BASE + tx_len_off, 2)
        if data:
            tx_len = int.from_bytes(data, 'little')
            print(f'  UEP{ep_num}_TX_LEN: {tx_len} bytes')

        # TX_CTRL
        data = read_memory(dev, ADDR_USBFS_BASE + tx_ctrl_off, 1)
        if data:
            print_register(f'UEP{ep_num}_TX_CTRL', data[0], {
                7: 'reserved',
                6: 'reserved',
                5: 'reserved',
                4: 'reserved',
                3: 'UEP_T_AUTO_TOG',
                2: 'UEP_T_TOG',
                1: 'UEP_T_RES1',
                0: 'UEP_T_RES0'
            })
            res = data[0] & 0x03
            res_names = {0: 'ACK (ready)', 1: 'NONE (no response)', 2: 'NAK', 3: 'STALL'}
            print(f'    Response: {res_names.get(res, "unknown")}')


def main():
    parser = argparse.ArgumentParser(description='Read memory from Openterface target')
    parser.add_argument('--addr', type=lambda x: int(x, 0), help='Memory address (hex)')
    parser.add_argument('--len', type=int, default=16, help='Number of bytes to read')
    parser.add_argument('--all', action='store_true', help='Dump all key variables and registers')
    args = parser.parse_args()

    # Find device
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f'Error: Device {VID:04x}:{PID:04x} not found', file=sys.stderr)
        print('Check that the target device is connected and running firmware', file=sys.stderr)
        sys.exit(1)

    print(f'Found device {VID:04x}:{PID:04x}')

    try:
        if args.all:
            dump_all(dev)
        elif args.addr is not None:
            print(f'Reading {args.len} bytes from 0x{args.addr:08X}:')
            data = read_memory(dev, args.addr, args.len)
            if data:
                print_hex(data, args.addr)
        else:
            parser.print_help()
    except Exception as e:
        print(f'Error: {e}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
