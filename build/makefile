# MRS Version: 2.1.0
# Automatically-generated file. Do not edit!
################################################################################

# Define the source directory
SRC_DIR := $(abspath $(dir $(CURDIR)))

# The first rule pulled in from subdir.mk would otherwise become the default
# goal, so `make` / `make kvmgo` would only build one startup object.
.DEFAULT_GOAL := all

# Include other makefiles
-include ../makefile.init

RM := rm -rf

# ---------------------------------------------------------------------------
# Product table — the only place that decides what a shipped image contains.
# Keyed by product name, not BOARD_VERSION: keymod-plus and keymod-mini are the
# same v3 silicon with different USB composites and different 32KHz clocks.
# The four entry points that used to repeat these values (the CI workflow,
# build.sh, the board targets further down, and the BOARD_VERSION fallbacks
# below) had all drifted apart, so the CI image was not the bench-tested image.
# Everything here is ?=, so a command-line override still wins:
#   make PRODUCT=kvmgo ENABLE_BLE=1
# ---------------------------------------------------------------------------
PRODUCT ?=

ifeq ($(PRODUCT),kvmgo)
# BLE compiled out for now; both CDC ECM interfaces stay.
BOARD_VERSION ?= 1
ENABLE_BLE ?= 0
USE_LTO ?= 1
USB_WORK_MODE ?= hid_acm_ecm_bridge
else ifeq ($(PRODUCT),minikvm)
BOARD_VERSION ?= 2
ENABLE_BLE ?= 0
USE_LTO ?= 1
USB_WORK_MODE ?= hid_acm_ecm
else ifeq ($(PRODUCT),keymod-plus)
BOARD_VERSION ?= 3
ENABLE_BLE ?= 1
USE_LTO ?= 1
ENABLE_OSCILLATOR ?= 1
USB_WORK_MODE ?= hid_acm_ecm_bridge
else ifeq ($(PRODUCT),keymod-mini)
BOARD_VERSION ?= 3
ENABLE_BLE ?= 1
USE_LTO ?= 1
ENABLE_OSCILLATOR ?= 0
USB_WORK_MODE ?= hid_ecm_bridge
else ifneq ($(PRODUCT),)
$(error Unknown PRODUCT '$(PRODUCT)' — use kvmgo, minikvm, keymod-plus or keymod-mini)
else
$(warning PRODUCT is not set — falling back to the defaults below, which is a KVM-GO-era hid_acm composite and not any shipping image. Build with PRODUCT=kvmgo|minikvm|keymod-plus|keymod-mini.)
endif

# Feature flags — define BEFORE includes so subdir.mk files can use them
# BLE support: build with 'make ENABLE_BLE=0' to omit BLE (frees ~14 KB RAM)
ENABLE_BLE ?= 1
ENABLE_UAC_ORIGIN := $(origin ENABLE_UAC)
ENABLE_UAC ?= 0

# External 32KHz crystal oscillator for RTC/BLE timing.
#   1 = board has external 32KHz crystal (CLK_OSC32K=0) — "Plus" variant
#   0 = use internal 32K RC oscillator (CLK_OSC32K=1) — "Mini" variant
ENABLE_OSCILLATOR ?= 0

# EP3 USB endpoint mode: 0 = Absolute Mouse (default), 1 = Stylus/Touchscreen
ENABLE_STYLUS ?= 0

BOARD_VERSION ?= 1

# Fallback work mode for a bare `make` with no PRODUCT (legacy MounRiver IDE
# builds). Every shipping image selects a product above; this block is what a
# PRODUCT-less build gets, and it is NOT a shipping configuration.
override ENABLE_UAC := 0
ifndef USB_WORK_MODE
ifneq ($(filter 1 2,$(BOARD_VERSION)),)
USB_WORK_MODE := hid_acm
else
USB_WORK_MODE := hid_acm_ecm
endif
endif

# USB work modes (see APP/include/usb_work_mode.h for the per-mode capability
# flags):
#   hid_acm             = target HID, host CDC ACM
#   hid_uac_acm         = + UAC
#   hid_acm_bridge      = target CDC ACM scaffold + host CDC ACM
#   hid_ecm_bridge      = CDC ECM toward host and target, no CDC ACM  (keymod-mini)
#   hid_uac_ecm         = ECM + UAC; no shipping product uses it
#   hid_acm_ecm         = CDC ACM + CDC ECM                                   (minikvm)
#   hid_acm_ecm_bridge  = CDC ACM + CDC ECM + target-side ECM bridge         (kvmgo, keymod-plus)
USB_WORK_MODE ?= $(if $(filter 0,$(ENABLE_UAC)),hid_acm,hid_uac_acm)

ifeq ($(USB_WORK_MODE),hid_acm)
USB_WORK_MODE_ID := 1
USB_WORK_MODE_ENABLE_UAC := 0
else ifeq ($(USB_WORK_MODE),hid_uac_acm)
USB_WORK_MODE_ID := 2
USB_WORK_MODE_ENABLE_UAC := 1
else ifeq ($(USB_WORK_MODE),hid_acm_bridge)
USB_WORK_MODE_ID := 3
USB_WORK_MODE_ENABLE_UAC := 0
else ifeq ($(USB_WORK_MODE),hid_ecm_bridge)
USB_WORK_MODE_ID := 4
USB_WORK_MODE_ENABLE_UAC := 0
else ifeq ($(USB_WORK_MODE),hid_acm_ecm)
USB_WORK_MODE_ID := 6
USB_WORK_MODE_ENABLE_UAC := 0
else ifeq ($(USB_WORK_MODE),hid_acm_ecm_bridge)
USB_WORK_MODE_ID := 7
USB_WORK_MODE_ENABLE_UAC := 0
else
$(error Unsupported USB_WORK_MODE '$(USB_WORK_MODE)')
endif

ifeq ($(ENABLE_UAC_ORIGIN),undefined)
ENABLE_UAC := $(USB_WORK_MODE_ENABLE_UAC)
else ifneq ($(ENABLE_UAC),$(USB_WORK_MODE_ENABLE_UAC))
$(error ENABLE_UAC=$(ENABLE_UAC) conflicts with USB_WORK_MODE=$(USB_WORK_MODE))
endif

HOST_CC ?= cc
PKG_CONFIG ?= pkg-config
WCHISP ?= ../tools/wchisp_c
BUILD_CONFIG_STAMP := .build-config.stamp

# BLE name per board version: 1=KVMGo, 2=Mini-KVM, 3=KeyMod
ifeq ($(BOARD_VERSION),1)
BLE_NAME ?= KVM-GO
else ifeq ($(BOARD_VERSION),2)
BLE_NAME ?= Mini-KVM
else
BLE_NAME ?= KeyMod
endif

LIBUSB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS ?= $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

ifeq ($(strip $(LIBUSB_LIBS)),)
ifneq ($(wildcard /opt/homebrew/include/libusb-1.0/libusb.h),)
LIBUSB_CFLAGS := -I/opt/homebrew/include/libusb-1.0
LIBUSB_LIBS := -L/opt/homebrew/lib -lusb-1.0
else ifneq ($(wildcard /usr/local/include/libusb-1.0/libusb.h),)
LIBUSB_CFLAGS := -I/usr/local/include/libusb-1.0
LIBUSB_LIBS := -L/usr/local/lib -lusb-1.0
endif
endif

# All of the sources participating in the build are defined here
-include sources.mk
-include SRC/Startup/subdir.mk
-include SRC/Peripheral/src/subdir.mk
-include SRC/Debug/subdir.mk
-include SRC/Core/subdir.mk
-include Profile/subdir.mk
-include LIB/subdir.mk
-include HAL/subdir.mk
-include APP/USBLIB/USB-Driver/src/subdir.mk
-include APP/USBLIB/CONFIG/subdir.mk
-include APP/UART/subdir.mk
-include APP/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(ASM_UPPER_DEPS)),)
-include $(ASM_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 
SECONDARY_FLASH += \
openterface-km-firmware.hex \

SECONDARY_LIST += \
openterface-km-firmware.lst \

SECONDARY_SIZE += \
openterface-km-firmware.siz \


# All Target
all:
	$(MAKE) --no-print-directory main-build

# Print the flags a product resolves to. Entry points echo this instead of
# restating the table, which is how build.sh and CI drifted from each other.
show-config:
	@echo "PRODUCT=$(PRODUCT) BOARD_VERSION=$(BOARD_VERSION) USB_WORK_MODE=$(USB_WORK_MODE) (mode $(USB_WORK_MODE_ID)) ENABLE_BLE=$(ENABLE_BLE) ENABLE_OSCILLATOR=$(ENABLE_OSCILLATOR) USE_LTO=$(USE_LTO) BLE_NAME=$(BLE_NAME) ENABLE_STYLUS=$(ENABLE_STYLUS)"

# Board targets: convenience wrappers over the product table above, for the
# MounRiver IDE and manual builds. They run cp/objcopy locally, so the CI
# workflow calls make PRODUCT=... directly and copies with PowerShell.
kvmgo:
	$(MAKE) --no-print-directory PRODUCT=kvmgo
	@riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"
	@cp openterface-km-firmware.elf openterface-km-firmware-kvmgo.elf
	@cp openterface-km-firmware.bin openterface-km-firmware-kvmgo.bin
	@cp openterface-km-firmware.hex openterface-km-firmware-kvmgo.hex
	@echo "Built: openterface-km-firmware-kvmgo (HID+ACM+ECM bridge, no BLE)"

# Mini-KVM build configuration (BOARD_VERSION=2, CDC COM + ECM)
minikvm:
	$(MAKE) --no-print-directory PRODUCT=minikvm
	@riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"
	@cp openterface-km-firmware.elf openterface-km-firmware-minikvm.elf
	@cp openterface-km-firmware.bin openterface-km-firmware-minikvm.bin
	@cp openterface-km-firmware.hex openterface-km-firmware-minikvm.hex
	@echo "Built: openterface-km-firmware-minikvm (CDC COM+ECM)"

# KeyMod Plus build configuration (BOARD_VERSION=3, same personality as KVMGo)
keymod-plus:
	$(MAKE) --no-print-directory PRODUCT=keymod-plus
	@riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"
	@cp openterface-km-firmware.elf openterface-km-firmware-keymod-plus.elf
	@cp openterface-km-firmware.bin openterface-km-firmware-keymod-plus.bin
	@cp openterface-km-firmware.hex openterface-km-firmware-keymod-plus.hex
	@echo "Built: openterface-km-firmware-keymod-plus (HID+ACM+ECM bridge, osc=on)"

# KeyMod Mini build configuration (BOARD_VERSION=3, no CDC-ACM serial)
keymod-mini:
	$(MAKE) --no-print-directory PRODUCT=keymod-mini
	@riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"
	@cp openterface-km-firmware.elf openterface-km-firmware-keymod-mini.elf
	@cp openterface-km-firmware.bin openterface-km-firmware-keymod-mini.bin
	@cp openterface-km-firmware.hex openterface-km-firmware-keymod-mini.hex
	@echo "Built: openterface-km-firmware-keymod-mini (HID+ECM bridge, no serial, osc=off)"

main-build: openterface-km-firmware.elf secondary-outputs

bin: openterface-km-firmware.bin

flash: openterface-km-firmware.bin $(WCHISP)
	@echo 'Flashing...'
	$(WCHISP) flash openterface-km-firmware.bin

probe: $(WCHISP)
	$(WCHISP) probe

openterface-km-firmware.bin: openterface-km-firmware.elf
	@echo 'Generating BIN file for flashing...'
	riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"

ifeq ($(strip $(LIBUSB_LIBS)),)
$(WCHISP):
	@echo 'libusb-1.0 development files not found. Install with: brew install libusb pkg-config' >&2
	@false
else
$(WCHISP): ../tools/wchisp_c.c
	@echo 'Building host WCH ISP tool: $@'
	$(HOST_CC) -O2 -Wall -Wextra -o "$@" "$<" $(LIBUSB_CFLAGS) $(LIBUSB_LIBS)
endif

CFLAGS += -include stdint.h -include stddef.h
CFLAGS += -Dversion=$(BOARD_VERSION)
CFLAGS += -DUSB_WORK_MODE=$(USB_WORK_MODE_ID)

# All firmware versions: disable runtime profile switching to prevent
# accidental mode changes. Each board uses its compiled work mode.
CFLAGS += -DUSB_WORK_MODE_RUNTIME_PROFILES=1

ifeq ($(ENABLE_OSCILLATOR),1)
CFLAGS += -DCLK_OSC32K=0
else
CFLAGS += -DCLK_OSC32K=1
endif

# UAC support is derived from USB_WORK_MODE. Legacy ENABLE_UAC=0 maps to hid_acm
# when USB_WORK_MODE is not specified.
ifeq ($(ENABLE_UAC),1)
CFLAGS += -DENABLE_UAC
endif

ifeq ($(ENABLE_STYLUS),1)
CFLAGS += -DEP3_USE_STYLUS
endif

ifeq ($(ENABLE_BLE),1)
CFLAGS += -DENABLE_BLE
CFLAGS += -DBLE_DEVICE_NAME="\"$(BLE_NAME)\""
# BLE heap size in bytes — WCH minimum is 6144 (6 KB); default was 10240.
# The working baseline used 6 KB. Keep that default unless a specific board
# needs more heap for BLE bring-up.
BLE_MEMHEAP_SIZE ?= 6144
CFLAGS += -DBLE_MEMHEAP_SIZE=$(BLE_MEMHEAP_SIZE)
# Max BLE packet length (ATT_MTU = BLE_BUFF_MAX_LEN - 4).
# 64 is the WCH default; fully adequate for HID keyboard/mouse reports.
BLE_BUFF_MAX_LEN ?= 64
CFLAGS += -DBLE_BUFF_MAX_LEN=$(BLE_BUFF_MAX_LEN)
# Max TX packets per connection event (1 is sufficient for HID traffic).
BLE_TX_NUM_EVENT ?= 1
CFLAGS += -DBLE_TX_NUM_EVENT=$(BLE_TX_NUM_EVENT)
# We only use peripheral role, no central connections needed.
CFLAGS += -DCENTRAL_MAX_CONNECTION=0
else
# Strip -lwchble from LIBS (defined in objects.mk) when BLE is disabled
LIBS := -lm
# Exclude BLE-related source files when BLE is disabled
OBJS := $(filter-out ./Profile/%.o, $(OBJS))
OBJS := $(filter-out ./HAL/RTC.o ./HAL/SLEEP.o, $(OBJS))
endif

# Link Time Optimization (LTO) — reduces code size by ~15-25%
# Enable with: make USE_LTO=1
USE_LTO ?= 0
ifeq ($(USE_LTO),1)
CFLAGS += -flto=full -fuse-linker-plugin
LDFLAGS += -flto=full -fuse-linker-plugin
# More aggressive dead code elimination
CFLAGS += -ffat-lto-objects
endif

$(BUILD_CONFIG_STAMP): FORCE
	@{ \
		printf 'BOARD_VERSION=%s\n' "$(BOARD_VERSION)"; \
		printf 'USB_WORK_MODE=%s\n' "$(USB_WORK_MODE)"; \
		printf 'ENABLE_UAC=%s\n' "$(ENABLE_UAC)"; \
		printf 'ENABLE_BLE=%s\n' "$(ENABLE_BLE)"; \
			printf 'ENABLE_STYLUS=%s\n' "$(ENABLE_STYLUS)"; \
		printf 'BLE_NAME=%s\n' "$(BLE_NAME)"; \
		printf 'ENABLE_OSCILLATOR=%s\n' "$(ENABLE_OSCILLATOR)"; \
		printf 'BLE_MEMHEAP_SIZE=%s\n' "$(BLE_MEMHEAP_SIZE)"; \
		printf 'BLE_BUFF_MAX_LEN=%s\n' "$(BLE_BUFF_MAX_LEN)"; \
		printf 'BLE_TX_NUM_EVENT=%s\n' "$(BLE_TX_NUM_EVENT)"; \
	} > "$@.tmp"
	@if test ! -f "$@" || test "$$(cat "$@.tmp")" != "$$(cat "$@")"; then \
		mv "$@.tmp" "$@"; \
	else \
		rm -f "$@.tmp"; \
	fi

$(OBJS): $(BUILD_CONFIG_STAMP)

# Tool invocations
openterface-km-firmware.elf: $(OBJS) $(USER_OBJS)
	@echo 'Building target: $@'
	@echo 'Invoking: GNU RISC-V Cross C Linker'
	riscv-none-embed-gcc -march=rv32imacxw -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g $(CFLAGS) $(LDFLAGS) -T "../HAL/Link.ld" -nostartfiles -Xlinker --gc-sections -L"../" -L"../LIB" -Xlinker --print-memory-usage -Wl,-Map,"openterface-km-firmware.map" --specs=nano.specs --specs=nosys.specs -o "openterface-km-firmware.elf" $(OBJS) $(USER_OBJS) $(LIBS)
	@echo 'Finished building target: $@'
	@
openterface-km-firmware.hex: openterface-km-firmware.elf
	@echo 'Invoking: GNU RISC-V Cross Create Binary Image'
	@	riscv-none-embed-objcopy -O ihex "openterface-km-firmware.elf" "openterface-km-firmware.hex"
	@echo 'Finished building $@'
	@
openterface-km-firmware.lst: openterface-km-firmware.elf
	@echo 'Invoking: GNU RISC-V Cross Create Listing'
	@	riscv-none-embed-objdump --source --all-headers --demangle -M xw --line-numbers --wide "openterface-km-firmware.elf" > "openterface-km-firmware.lst"
	@echo 'Finished building $@'
	@
openterface-km-firmware.siz: openterface-km-firmware.elf
	@echo 'Invoking: GNU RISC-V Cross Print Size'
	riscv-none-embed-size --format=berkeley "openterface-km-firmware.elf"
	@echo 'Finished building $@'
	@

# Other Targets
clean:
	-$(RM) $(OBJS)$(SECONDARY_FLASH)$(SECONDARY_LIST)$(SECONDARY_SIZE)$(S_DEPS)$(S_UPPER_DEPS)$(ASM_DEPS)$(ASM_UPPER_DEPS)$(C_DEPS) openterface-km-firmware.elf openterface-km-firmware.hex openterface-km-firmware.bin openterface-km-firmware-plus.elf openterface-km-firmware-plus.hex openterface-km-firmware-plus.bin openterface-km-firmware-mini.elf openterface-km-firmware-mini.hex openterface-km-firmware-mini.bin $(BUILD_CONFIG_STAMP)

secondary-outputs: $(SECONDARY_FLASH) $(SECONDARY_LIST) $(SECONDARY_SIZE)

.PHONY: all clean dependents bin flash probe FORCE keymod-plus keymod-mini kvmgo minikvm

FORCE:

-include ../makefile.targets