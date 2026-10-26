#!/bin/bash
# =============================================================================
# Openterface KM Firmware Local Build Script
# Usage: ./build.sh [options] [product version]
# =============================================================================

set -e

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Project directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# Default parameters
ENABLE_BLE=1  # Enabled by default, only minikvm disables it
USE_LTO=1     # LTO enabled for all boards
ENABLE_OSCILLATOR=""
BOARD_VERSION=""
BLE_NAME=""
TARGET_NAME=""

# Compiler path - specify via --compiler or use env variable RISC_COMPILER_PATH
RISC_COMPILER_PATH="${RISC_COMPILER_PATH:-}"

# Help information
show_help() {
    echo -e "${BLUE}Openterface KM Firmware Build Script${NC}"
    echo ""
    echo "Usage: ./build.sh [options] <product version>"
    echo ""
    echo "Product versions:"
    echo "  kvmgo          Build KVMGo (BOARD_VERSION=1)"
    echo "  minikvm        Build Mini-KVM v1.2 (BOARD_VERSION=2)"
    echo "  keymod-plus    Build KeyMod Plus (external 32KHz oscillator)"
    echo "  keymod-mini    Build KeyMod Mini (internal RC oscillator)"
    echo "  all            Build all versions"
    echo ""
    echo "Options:"
    echo "  --no-ble           Disable Bluetooth (saves ~14KB RAM)"
    echo "  --compiler PATH    Specify RISC-V compiler path"
    echo "  --help, -h         Show this help message"
    echo ""
    echo "Examples:"
    echo "  ./build.sh kvmgo                    # Build KVMGo firmware"
    echo "  ./build.sh keymod-plus              # Build KeyMod Plus firmware"
    echo "  ./build.sh --no-ble kvmgo           # Build KVMGo firmware without Bluetooth"
    echo "  ./build.sh all                      # Build all versions"
    echo "  ./build.sh --compiler /opt/risc-none-embed-gcc-8.2.0-3.1.1/bin kvmgo"
    echo ""
    echo "Output directory: ${OUTPUT_DIR}/"
}

# Check compiler
check_compiler() {
    # If compiler path is specified, add it to PATH
    if [ -n "${RISC_COMPILER_PATH}" ]; then
        export PATH="${RISC_COMPILER_PATH}:${PATH}"
        echo -e "${BLUE}Using specified compiler path: ${RISC_COMPILER_PATH}${NC}"
    fi

    if ! command -v riscv-none-embed-gcc &> /dev/null; then
        echo -e "${RED}Error: riscv-none-embed-gcc compiler not found${NC}"
        echo ""
        echo "Please install the RISC-V toolchain (version 8.2.0) and add it to PATH:"
        echo "  1. Download: https://github.com/openwch/risc-none-embed-gcc/releases/tag/8.2.0-3.1.1"
        echo "     Select: risc-none-embed-gcc-8.2.0-3.1.1-linux-x86_64.tar.bz2"
        echo "  2. Extract to: /opt/risc-none-embed-gcc-8.2.0-3.1.1"
        echo "  3. Use one of the following methods:"
        echo "     a) export PATH=/opt/risc-none-embed-gcc-8.2.0-3.1.1/bin:\$PATH"
        echo "     b) ./build.sh --compiler /opt/risc-none-embed-gcc-8.2.0-3.1.1/bin kvmgo"
        echo "     c) export RISC_COMPILER_PATH=/opt/risc-none-embed-gcc-8.2.0-3.1.1/bin"
        exit 1
    fi

    local compiler_path=$(which riscv-none-embed-gcc)
    local compiler_version=$(riscv-none-embed-gcc --version | head -1)
    echo -e "${GREEN}✓ Compiler: ${compiler_path}${NC}"
    echo -e "${GREEN}  Version: ${compiler_version}${NC}"

    # Warning: check compiler version
    if echo "${compiler_version}" | grep -q "15\."; then
        echo -e "${YELLOW}⚠ Warning: Compiler version 15.x may be incompatible with WCH chips${NC}"
        echo -e "${YELLOW}  Recommended version 8.2.0: https://github.com/openwch/risc-none-embed-gcc/releases/tag/8.2.0-3.1.1${NC}"
    fi
}

# Create output directory
create_output_dir() {
    mkdir -p "${OUTPUT_DIR}"
}

# Build firmware
build_firmware() {
    local target=$1
    local board_version=$2
    local enable_oscillator=$3
    local ble_name=$4
    local usb_work_mode=$5

    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Building: ${target}${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo "  BOARD_VERSION:     ${board_version}"
    echo "  ENABLE_BLE:        ${ENABLE_BLE}"
    echo "  ENABLE_OSCILLATOR: ${enable_oscillator}"
    echo "  BLE_NAME:          ${ble_name}"
    echo "  USB_WORK_MODE:     ${usb_work_mode}"
    echo ""

    cd "${BUILD_DIR}"

    # Clean
    make clean > /dev/null 2>&1 || true

    # Build make arguments
    local make_args="BOARD_VERSION=${board_version} ENABLE_BLE=${ENABLE_BLE} BLE_NAME=${ble_name} USE_LTO=${USE_LTO}"
    if [ -n "${enable_oscillator}" ]; then
        make_args="${make_args} ENABLE_OSCILLATOR=${enable_oscillator}"
    fi
    if [ -n "${usb_work_mode}" ]; then
        make_args="${make_args} USB_WORK_MODE=${usb_work_mode}"
    fi

    # Compile
    echo -e "${YELLOW}Compiling...${NC}"
    if ! make -j$(nproc) ${make_args} all; then
        echo -e "${RED}✗ Build failed: ${target}${NC}"
        return 1
    fi

    # Generate binary file
    riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"

    # Get firmware size
    local size_info=$(riscv-none-embed-size --format=berkeley openterface-km-firmware.elf)

    # Copy to output directory
    local output_name="openterface-km-firmware-${target}"
    cp "openterface-km-firmware.hex" "${OUTPUT_DIR}/${output_name}.hex"
    cp "openterface-km-firmware.bin" "${OUTPUT_DIR}/${output_name}.bin"
    cp "openterface-km-firmware.elf" "${OUTPUT_DIR}/${output_name}.elf"
    cp "openterface-km-firmware.map" "${OUTPUT_DIR}/${output_name}.map"

    echo -e "${GREEN}✓ Build succeeded: ${target}${NC}"
    echo -e "${GREEN}  Output file: ${OUTPUT_DIR}/${output_name}.bin${NC}"
    echo ""
    echo "Firmware size:"
    echo "${size_info}"
    echo ""

    return 0
}

# Parse arguments
TARGET=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --help|-h)
            show_help
            exit 0
            ;;
        --no-ble)
            ENABLE_BLE=0
            shift
            ;;
        --compiler)
            RISC_COMPILER_PATH="$2"
            shift 2
            ;;
        kvmgo|minikvm|keymod-plus|keymod-mini|all)
            TARGET=$1
            shift
            ;;
        *)
            echo -e "${RED}Error: Unknown argument '$1'${NC}"
            show_help
            exit 1
            ;;
    esac
done

# Check if target is specified
if [ -z "${TARGET}" ]; then
    echo -e "${RED}Error: Please specify a product version to build${NC}"
    echo ""
    show_help
    exit 1
fi

# Start build
echo -e "${BLUE}Openterface KM Firmware Build${NC}"
echo "========================"
check_compiler
create_output_dir

# Build counter
BUILD_SUCCESS=0
BUILD_FAILED=0

# Build by target
build_one() {
    local target=$1
    local board_version=$2
    local enable_oscillator=$3
    local ble_name=$4
    local usb_work_mode=$5

    if build_firmware "${target}" "${board_version}" "${enable_oscillator}" "${ble_name}" "${usb_work_mode}"; then
        BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
    else
        BUILD_FAILED=$((BUILD_FAILED + 1))
    fi
}

case ${TARGET} in
    kvmgo)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "kvmgo" "1" "" "KVM-GO" "hid_acm_ecm_bridge"
        ;;
    minikvm)
        ENABLE_BLE=0
        USE_LTO=1
        build_one "minikvm" "2" "" "Mini-KVM" "hid_acm_ecm"
        ;;
    keymod-plus)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "keymod-plus" "3" "1" "KeyMod" "hid_acm_ecm"
        ;;
    keymod-mini)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "keymod-mini" "3" "0" "KeyMod" "hid_acm_ecm"
        ;;
    all)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "kvmgo" "1" "" "KVM-GO" "hid_acm_ecm"
        ENABLE_BLE=0
        build_one "minikvm" "2" "" "Mini-KVM" "hid_acm_ecm"
        ENABLE_BLE=1
        build_one "keymod-plus" "3" "1" "KeyMod" "hid_acm_ecm"
        build_one "keymod-mini" "3" "0" "KeyMod" "hid_acm_ecm"
        ;;
esac

# Summary
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Build complete${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "  Success: ${GREEN}${BUILD_SUCCESS}${NC}"
echo -e "  Failed:  ${RED}${BUILD_FAILED}${NC}"
echo ""
echo "Output directory: ${OUTPUT_DIR}/"
ls -lh "${OUTPUT_DIR}"/*.bin 2>/dev/null || echo "(no output files)"

if [ ${BUILD_FAILED} -gt 0 ]; then
    exit 1
fi
