################################################################################
# MRS Version: 2.1.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables
C_SRCS += \
../APP/RingMem.c \
../APP/acm_bridge.c \
../APP/ble_eth_protocol.c \
../APP/ble_name_config.c \
../APP/CH32_TEMP.c \
../APP/ch32v20x_it.c \
../APP/ch32v20x_usbfs_device.c \
../APP/default_target_config.c \
../APP/DS18B20.c \
../APP/ecm_bridge.c \
../APP/ecm_ip_config.c \
../APP/iwdg_auto.c \
../APP/keyboard_handler.c \
../APP/km_ring.c \
../APP/mouse_handler.c \
../APP/peripheral.c \
../APP/peripheral_main.c \
../APP/pwm_led.c \
../APP/RGB.c \
../APP/sd_switch.c \
../APP/system_ch32v20x.c \
../APP/uac_handler.c \
../APP/usb_mode_config.c \
../APP/usbd_compostie_km.c \
../APP/usbd_desc.c

C_DEPS += \
./APP/RingMem.d \
./APP/acm_bridge.d \
./APP/ble_eth_protocol.d \
./APP/ble_name_config.d \
./APP/CH32_TEMP.d \
./APP/ch32v20x_it.d \
./APP/ch32v20x_usbfs_device.d \
./APP/default_target_config.d \
./APP/DS18B20.d \
./APP/ecm_bridge.d \
./APP/ecm_ip_config.d \
./APP/iwdg_auto.d \
./APP/keyboard_handler.d \
./APP/km_ring.d \
./APP/mouse_handler.d \
./APP/peripheral.d \
./APP/peripheral_main.d \
./APP/pwm_led.d \
./APP/RGB.d \
./APP/sd_switch.d \
./APP/system_ch32v20x.d \
./APP/uac_handler.d \
./APP/usb_mode_config.d \
./APP/usbd_compostie_km.d \
./APP/usbd_desc.d

OBJS += \
./APP/RingMem.o \
./APP/acm_bridge.o \
./APP/ble_eth_protocol.o \
./APP/ble_name_config.o \
./APP/CH32_TEMP.o \
./APP/ch32v20x_it.o \
./APP/ch32v20x_usbfs_device.o \
./APP/default_target_config.o \
./APP/DS18B20.o \
./APP/ecm_bridge.o \
./APP/ecm_ip_config.o \
./APP/iwdg_auto.o \
./APP/keyboard_handler.o \
./APP/km_ring.o \
./APP/mouse_handler.o \
./APP/peripheral.o \
./APP/peripheral_main.o \
./APP/pwm_led.o \
./APP/RGB.o \
./APP/sd_switch.o \
./APP/system_ch32v20x.o \
./APP/uac_handler.o \
./APP/usb_mode_config.o \
./APP/usbd_compostie_km.o \
./APP/usbd_desc.o

# Each subdirectory must supply rules for building sources it contributes
APP/%.o: ../APP/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GNU RISC-V Cross C Compiler'
	riscv-none-embed-gcc -march=rv32imacxw -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DCH32V20x_D8W -I"$(SRC_DIR)/SRC/Core" -I"$(SRC_DIR)/SRC/Startup" -I"$(SRC_DIR)/SRC/Debug" -I"$(SRC_DIR)/SRC/Peripheral/inc" -I"$(SRC_DIR)/APP/include" -I"$(SRC_DIR)/Profile/include" -I"$(SRC_DIR)/HAL/include" -I"$(SRC_DIR)/LIB" -I"$(SRC_DIR)/APP/UART" -I"$(SRC_DIR)/APP/USBLIB/CONFIG" -I"$(SRC_DIR)/APP/USBLIB/USB-Driver/inc" $(CFLAGS) -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@