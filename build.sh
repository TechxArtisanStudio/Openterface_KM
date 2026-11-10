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
# Per-board flags live in build/makefile (PRODUCT=...). Anything set here only
# overrides that table.
EXTRA_MAKE_ARGS=""
TARGET_NAME=""

# macOS ships no nproc, and an empty substitution turned this into a bare -j
# (unbounded parallel jobs), which stalls the machine on a full LTO build.
JOBS=$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )

# Compiler path - specify via --compiler or use env variable RISC_COMPILER_PATH
RISC_COMPILER_PATH="${RISC_COMPILER_PATH:-}"

# Help information
show_help() {
    echo -e "${BLUE}Openterface KM Firmware Build Script${NC}"
    echo ""
    echo "Usage: ./build.sh [options] <product version>"
    echo ""
    echo "Product versions (flags come from the PRODUCT table in build/makefile):"
    echo "  kvmgo          KVMGo (BOARD_VERSION=1, HID+ACM+ECM bridge, no BLE)"
    echo "  minikvm        Mini-KVM v1.2 (BOARD_VERSION=2, CDC COM+ECM, no BLE)"
    echo "  keymod-plus    KeyMod Plus (BOARD_VERSION=3, external 32KHz oscillator, BLE)"
    echo "  keymod-mini    KeyMod Mini (BOARD_VERSION=3, internal RC oscillator, BLE, no serial)"
    echo "  all            Build all versions"
    echo ""
    echo "Options:"
    echo "  --no-ble           Disable Bluetooth (saves ~14KB RAM)"
    echo "  --make-arg VAR=V   Override one build/makefile flag for this run"
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

    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Building: ${target}${NC}"
    echo -e "${BLUE}========================================${NC}"
    cd "${BUILD_DIR}"

    # Clean
    make clean > /dev/null 2>&1 || true

    # PRODUCT is the single flag table (build/makefile). Extra make arguments
    # from the command line, e.g. --no-ble, override it.
    local make_args="PRODUCT=${target} ${EXTRA_MAKE_ARGS}"

    echo "  $(make --no-print-directory ${make_args} show-config)"

    # Compile
    echo -e "${YELLOW}Compiling...${NC}"
    if ! make -j${JOBS} ${make_args} all; then
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
            EXTRA_MAKE_ARGS="${EXTRA_MAKE_ARGS} ENABLE_BLE=0"
            shift
            ;;
        --make-arg)
            # Pass a make variable straight through, overriding the PRODUCT
            # table, e.g. --make-arg USB_WORK_MODE=hid_acm_ecm_bridge
            EXTRA_MAKE_ARGS="${EXTRA_MAKE_ARGS} $2"
            shift 2
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

    if build_firmware "${target}"; then
        BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
    else
        BUILD_FAILED=$((BUILD_FAILED + 1))
    fi
}

case ${TARGET} in
    all)
        build_one "kvmgo"
        build_one "minikvm"
        build_one "keymod-plus"
        build_one "keymod-mini"
        ;;
    *)
        build_one "${TARGET}"
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
