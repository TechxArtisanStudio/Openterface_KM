#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

REMOTE_USER="admin"
REMOTE_HOST="192.168.100.63"
REMOTE_SSH="${REMOTE_USER}@${REMOTE_HOST}"

LOCAL_ZIP="/tmp/openterface-km-build.zip"
REMOTE_ZIP="/tmp/openterface-km-build.zip"
REMOTE_PROJECT_UNIX="/c/tmp/openterface-km-build"
REMOTE_BIN="/tmp/openterface-km-build/obj/openterface-km-firmware.bin"
LOCAL_BIN="/tmp/openterface-km-firmware.bin"

WINDOWS_BUILD_DIR='C:\tmp\openterface-km-build'
WINDOWS_ZIP='C:\tmp\openterface-km-build.zip'
MSYS_BASH='C:\msys64\usr\bin\bash.exe'

CONTROL_PORT="/dev/ttyACM0"   # Linux: control board (USB_Serial at 115200 baud)
WCHISP_BIN="${HOME}/.cargo/bin/wchisp"

# Firmware build variables (exported and forwarded through SSH)
BOARD_VERSION="${BOARD_VERSION:-1}"
ENABLE_BLE="${ENABLE_BLE:-1}"
ENABLE_UAC="${ENABLE_UAC:-0}"
ENABLE_OSCILLATOR="${ENABLE_OSCILLATOR:-0}"
USB_WORK_MODE="${USB_WORK_MODE:-hid_acm_ecm}"
# BLE_NAME is resolved after argument parsing (see finalize_ble_name)
# so that --board-version is honoured.
BLE_NAME="${BLE_NAME:-}"

DO_BUILD=1
DO_FLASH=1
DO_VERIFY=1
DO_SKIP_ISP=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Build on remote Windows + flash locally via wchisp.

Usage:
  tools/build_flash_cycle.sh [options]

Options:
  --remote-user <user>         Default: admin
  --remote-host <host>         Default: 192.168.100.63
  --control-port <port>        Default: /dev/cu.usbmodem212101
  --local-bin <path>           Default: /tmp/openterface-km-firmware.bin
  --wchisp <path>              Default: ~/.cargo/bin/wchisp
  --board-version <1-3>        Default: 1 (1=kvm go, 2=mini kvm, 3=keymod)
  --ble-name <name>            Default: auto from board-version (KVM-GO/Mini-KVM/KeyMod)
  --enable-ble <0|1>           Default: 1
  --enable-uac <0|1>           Default: 0
  --enable-oscillator <0|1>    Default: 0 (0=internal RC, 1=external 32K crystal)
  --usb-work-mode <mode>       Default: hid_acm_ecm (1=hid_acm 4=hid_ecm_bridge 6=hid_acm_ecm ...)
  --no-build                   Skip zip/upload/remote build/download
  --no-flash                   Skip flash/power-cycle steps
  --no-verify                  Skip post-flash ioreg verification
  --skip-isp                   Skip ISP mode entry (device already in ISP mode)
  --dry-run                    Print commands without executing
  -h, --help                   Show this help

Examples:
  tools/build_flash_cycle.sh
  tools/build_flash_cycle.sh --control-port /dev/cu.usbmodem212101
  tools/build_flash_cycle.sh --board-version 3 --ble-name KeyMod --enable-uac 1
  tools/build_flash_cycle.sh --no-build --local-bin /tmp/openterface-km-firmware.bin
EOF
}

log() {
    printf '[build-flash] %s\n' "$*"
}

run() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf '+ %q ' "$@"
        printf '\n'
        return 0
    fi
    "$@"
}

run_in_root() {
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf '+ (cd %q && ' "$ROOT_DIR"
        printf '%q ' "$@"
        printf ')\n'
        return 0
    fi
    (
        cd "$ROOT_DIR"
        "$@"
    )
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: required command not found: $1" >&2
        exit 1
    fi
}

send_control_frame() {
    local hex="$1"
    if [[ "$DRY_RUN" -eq 1 ]]; then
        printf '+ python3 -c "import serial; s=serial.Serial(\"%s\", 115200, timeout=1); s.write(bytes.fromhex(\"%s\"))"\n' "$CONTROL_PORT" "$hex"
        return 0
    fi
    python3 -c "
import serial
with serial.Serial('$CONTROL_PORT', 115200, timeout=1) as ser:
    ser.write(bytes.fromhex('$hex'))
"
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --remote-user)
                REMOTE_USER="$2"
                shift 2
                ;;
            --remote-host)
                REMOTE_HOST="$2"
                shift 2
                ;;
            --control-port)
                CONTROL_PORT="$2"
                shift 2
                ;;
            --local-bin)
                LOCAL_BIN="$2"
                shift 2
                ;;
            --wchisp)
                WCHISP_BIN="$2"
                shift 2
                ;;
            --board-version)
                BOARD_VERSION="$2"
                shift 2
                ;;
            --ble-name)
                BLE_NAME="$2"
                shift 2
                ;;
            --enable-ble)
                ENABLE_BLE="$2"
                shift 2
                ;;
            --enable-uac)
                ENABLE_UAC="$2"
                shift 2
                ;;
            --enable-oscillator)
                ENABLE_OSCILLATOR="$2"
                shift 2
                ;;
            --usb-work-mode)
                USB_WORK_MODE="$2"
                shift 2
                ;;
            --no-build)
                DO_BUILD=0
                shift
                ;;
            --no-flash)
                DO_FLASH=0
                shift
                ;;
            --no-verify)
                DO_VERIFY=0
                shift
                ;;
            --skip-isp)
                DO_SKIP_ISP=1
                shift
                ;;
            --dry-run)
                DRY_RUN=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "ERROR: unknown option: $1" >&2
                usage
                exit 1
                ;;
        esac
    done

    REMOTE_SSH="${REMOTE_USER}@${REMOTE_HOST}"

    # Resolve BLE_NAME after parsing so --board-version takes effect.
    if [[ -z "$BLE_NAME" ]]; then
      case "$BOARD_VERSION" in
        1) BLE_NAME="KVM-GO" ;;
        2) BLE_NAME="Mini-KVM" ;;
        3) BLE_NAME="KeyMod" ;;
        *) BLE_NAME="KeyMod" ;;
      esac
    fi
    log "Build config: BOARD_VERSION=${BOARD_VERSION} BLE_NAME=${BLE_NAME} ENABLE_BLE=${ENABLE_BLE} ENABLE_UAC=${ENABLE_UAC} ENABLE_OSCILLATOR=${ENABLE_OSCILLATOR} USB_WORK_MODE=${USB_WORK_MODE}"
}

build_remote() {
    log "Cleaning local stale object files"
    run_in_root rm -f obj/APP/pwm_led.o obj/peripheral_main.o obj/RGB.o

    log "Creating project zip"
    run_in_root zip -r "$LOCAL_ZIP" \
        SRC/ Profile/ HAL/ LIB/ APP/ \
        tools/build_firmware.sh \
        tools/wchisp_c.c \
        HAL/Link.ld \
        -x "*/.DS_Store" -x "__MACOSX/*"

    log "Uploading zip to remote Windows host"
    run scp "$LOCAL_ZIP" "${REMOTE_SSH}:${REMOTE_ZIP}"

    log "Extracting remote build tree"
    run ssh "$REMOTE_SSH" "powershell -NoProfile -Command \"Remove-Item -Recurse -Force ${WINDOWS_BUILD_DIR} -ErrorAction SilentlyContinue; Expand-Archive -Path ${WINDOWS_ZIP} -DestinationPath ${WINDOWS_BUILD_DIR} -Force\""

    log "Building firmware on remote MSYS2"
    run ssh "$REMOTE_SSH" "${MSYS_BASH} -l -c \"cd ${REMOTE_PROJECT_UNIX} && rm -rf obj && BOARD_VERSION=${BOARD_VERSION} BLE_NAME=${BLE_NAME} ENABLE_BLE=${ENABLE_BLE} ENABLE_UAC=${ENABLE_UAC} ENABLE_OSCILLATOR=${ENABLE_OSCILLATOR} USB_WORK_MODE=${USB_WORK_MODE} bash tools/build_firmware.sh\""

    log "Downloading built firmware binary"
    run scp "${REMOTE_SSH}:${REMOTE_BIN}" "$LOCAL_BIN"
}

flash_local() {
    if [[ ! -f "$LOCAL_BIN" ]]; then
        echo "ERROR: firmware binary not found: $LOCAL_BIN" >&2
        exit 1
    fi

    if [[ "$DO_SKIP_ISP" -eq 0 ]]; then
        log "Entering ISP mode via control port ${CONTROL_PORT}"
        send_control_frame "57AB001A01001D" # power off
        run sleep 0.5
        send_control_frame "57AB001901011D" # BOOT high
        run sleep 0.5
        send_control_frame "57AB001A01011E" # power on
        run sleep 3
    else
        log "Skipping ISP mode entry (device already in ISP mode)"
    fi

    log "Flashing binary with wchisp"
    run "$WCHISP_BIN" flash "$LOCAL_BIN"

    if [[ "$DO_SKIP_ISP" -eq 0 ]]; then
        log "Releasing BOOT and power-cycling target"
        send_control_frame "57AB001901001C" # BOOT low
        run sleep 0.5
        send_control_frame "57AB001A01001D" # power off
        run sleep 1
        send_control_frame "57AB001A01011E" # power on
        run sleep 5
    else
        log "Skipping power-cycle (device already running)"
    fi
}

verify_local() {
    log "Verifying target USB identity"
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "+ ioreg -p IOUSB -l | grep 'Openterface Target'"
        return 0
    fi

    if ioreg -p IOUSB -l 2>/dev/null | grep -q "Openterface Target"; then
        log "Verification passed: Openterface Target detected"
    else
        echo "WARNING: Openterface Target not detected in ioreg output" >&2
        return 1
    fi
}

main() {
    parse_args "$@"

    require_cmd zip
    require_cmd scp
    require_cmd ssh
    require_cmd xxd
    if [[ "$DO_VERIFY" -eq 1 ]]; then
        require_cmd ioreg
    fi
    if [[ "$DO_FLASH" -eq 1 && "$DRY_RUN" -eq 0 && ! -x "$WCHISP_BIN" ]]; then
        echo "ERROR: wchisp binary not found or not executable: $WCHISP_BIN" >&2
        exit 1
    fi

    if [[ "$DO_BUILD" -eq 1 ]]; then
        build_remote
    else
        log "Skipping remote build"
    fi

    if [[ "$DO_FLASH" -eq 1 ]]; then
        flash_local
    else
        log "Skipping flash"
    fi

    if [[ "$DO_VERIFY" -eq 1 ]]; then
        verify_local
    else
        log "Skipping verification"
    fi

    log "Done"
}

main "$@"
