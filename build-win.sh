#!/bin/bash
# =============================================================================
# Openterface KM Firmware Build Script for Windows (Git Bash / MSYS2)
# Usage: ./build-win.sh [options] [product version]
#
# Default Toolchain: MounRiver Studio2
# The script automatically uses the RISC-V toolchain bundled with MounRiver Studio2
# installed at: D:\MounRiver_Studio2
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
ENABLE_BLE=1  # Enabled by default
USE_LTO=1     # LTO enabled for all boards
ENABLE_OSCILLATOR=""
BOARD_VERSION=""
BLE_NAME=""
TARGET_NAME=""

# =============================================================================
# Default Toolchain Path - MounRiver Studio2 (Windows)
# This is the PRIMARY toolchain location. The script will automatically use
# this path if no custom compiler path is specified via --compiler option.
# =============================================================================
MOUNRIVER_TOOLCHAIN="/d/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC/bin"

# Help information
show_help() {
    echo -e "${BLUE}Openterface KM Firmware Build Script for Windows${NC}"
    echo ""
    echo "Usage: ./build-win.sh [options] <product version>"
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
    echo "  --compiler PATH    Specify custom RISC-V compiler path"
    echo "                     (overrides default MounRiver Studio2 toolchain)"
    echo "  --help, -h         Show this help message"
    echo ""
    echo "Default Toolchain:"
    echo "  The script uses MounRiver Studio2's bundled RISC-V toolchain by default:"
    echo "  ${MOUNRIVER_TOOLCHAIN}"
    echo ""
    echo "Examples:"
    echo "  ./build-win.sh kvmgo                    # Build KVMGo firmware"
    echo "  ./build-win.sh keymod-plus              # Build KeyMod Plus firmware"
    echo "  ./build-win.sh --no-ble kvmgo           # Build KVMGo without Bluetooth"
    echo "  ./build-win.sh all                      # Build all versions"
    echo "  ./build-win.sh --compiler /custom/path kvmgo  # Use custom toolchain"
    echo ""
    echo "Output directory: ${OUTPUT_DIR}/"
}

# Check compiler - uses MounRiver Studio2 toolchain by default
check_compiler() {
    local compiler_found=0

    # Priority 1: User-specified compiler path (via --compiler option or env var)
    if [ -n "${RISC_COMPILER_PATH}" ]; then
        export PATH="${RISC_COMPILER_PATH}:${PATH}"
        echo -e "${BLUE}Using custom compiler path: ${RISC_COMPILER_PATH}${NC}"
        compiler_found=1
    # Priority 2: MounRiver Studio2 default toolchain (RECOMMENDED)
    elif [ -d "${MOUNRIVER_TOOLCHAIN}" ]; then
        export PATH="${MOUNRIVER_TOOLCHAIN}:${PATH}"
        echo -e "${GREEN}✓ Using MounRiver Studio2 toolchain (default)${NC}"
        echo -e "${BLUE}  Path: ${MOUNRIVER_TOOLCHAIN}${NC}"
        compiler_found=1
    else
        echo -e "${YELLOW}⚠ MounRiver Studio2 toolchain not found at default location${NC}"
        echo -e "${YELLOW}  Expected: ${MOUNRIVER_TOOLCHAIN}${NC}"
    fi

    # Verify compiler is available
    if ! command -v riscv-none-embed-gcc &> /dev/null; then
        echo -e "${RED}Error: riscv-none-embed-gcc compiler not found${NC}"
        echo ""
        echo "Please install the RISC-V toolchain:"
        echo ""
        echo "  Option 1 (Recommended): Install MounRiver Studio2"
        echo "    Download from: https://www.mounriver.com/"
        echo "    Install to: D:\\MounRiver_Studio2 (default location)"
        echo ""
        echo "  Option 2: Use custom toolchain path"
        echo "    ./build-win.sh --compiler /path/to/toolchain/bin kvmgo"
        echo "    export RISC_COMPILER_PATH=/path/to/toolchain/bin"
        echo ""
        echo "  Option 3: Download standalone toolchain"
        echo "    https://github.com/openwch/risc-none-embed-gcc/releases/tag/8.2.0-3.1.1"
        exit 1
    fi

    local compiler_path=$(which riscv-none-embed-gcc)
    local compiler_version=$(riscv-none-embed-gcc --version | head -1)
    echo -e "${GREEN}✓ Compiler: ${compiler_path}${NC}"
    echo -e "${GREEN}  Version: ${compiler_version}${NC}"

    # Warning: check compiler version
    if echo "${compiler_version}" | grep -q "15\."; then
        echo -e "${YELLOW}⚠ Warning: Compiler version 15.x may be incompatible with WCH chips${NC}"
        echo -e "${YELLOW}  Recommended version 8.2.0 (bundled with MounRiver Studio2)${NC}"
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

    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Building: ${target}${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo "  BOARD_VERSION:     ${board_version}"
    echo "  ENABLE_BLE:        ${ENABLE_BLE}"
    echo "  ENABLE_OSCILLATOR: ${enable_oscillator}"
    echo "  BLE_NAME:          ${ble_name}"
    echo ""

    cd "${BUILD_DIR}"

    # Clean
    make clean > /dev/null 2>&1 || true

    # Build make arguments
    local make_args="BOARD_VERSION=${board_version} ENABLE_BLE=${ENABLE_BLE} BLE_NAME=${ble_name} USE_LTO=${USE_LTO}"
    if [ -n "${enable_oscillator}" ]; then
        make_args="${make_args} ENABLE_OSCILLATOR=${enable_oscillator}"
    fi

    # Compile (use NUMBER_OF_PROCESSORS on Windows)
    local nproc=${NUMBER_OF_PROCESSORS:-4}
    echo -e "${YELLOW}Compiling...${NC}"
    if ! make -j${nproc} ${make_args} all; then
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
echo -e "${BLUE}Openterface KM Firmware Build (Windows)${NC}"
echo "========================"
check_compiler
create_output_dir

# Build counter
BUILD_SUCCESS=0
BUILD_FAILED=0

# Build one function
build_one() {
    local target=$1
    local board_version=$2
    local enable_oscillator=$3
    local ble_name=$4

    if build_firmware "${target}" "${board_version}" "${enable_oscillator}" "${ble_name}"; then
        BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
    else
        BUILD_FAILED=$((BUILD_FAILED + 1))
    fi
}

# Build by target
case ${TARGET} in
    kvmgo)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "kvmgo" "1" "" "KVM-GO"
        ;;
    minikvm)
        ENABLE_BLE=0
        USE_LTO=1
        build_one "minikvm" "2" "" "Mini-KVM"
        ;;
    keymod-plus)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "keymod-plus" "3" "1" "KeyMod"
        ;;
    keymod-mini)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "keymod-mini" "3" "0" "KeyMod"
        ;;
    all)
        ENABLE_BLE=1
        USE_LTO=1
        build_one "kvmgo" "1" "" "KVM-GO"
        ENABLE_BLE=0
        build_one "minikvm" "2" "" "Mini-KVM"
        ENABLE_BLE=1
        build_one "keymod-plus" "3" "1" "KeyMod"
        build_one "keymod-mini" "3" "0" "KeyMod"
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
