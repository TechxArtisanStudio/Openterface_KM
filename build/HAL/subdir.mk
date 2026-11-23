################################################################################
# MRS Version: 2.1.0
# Automatically-generated file. Do not edit!
################################################################################

ifeq ($(ENABLE_BLE),1)
# Add inputs and outputs from these tool invocations to the build variables
C_SRCS += \
../HAL/KEY.c \
../HAL/LED.c \
../HAL/MCU.c \
../HAL/RTC.c \
../HAL/SLEEP.c

C_DEPS += \
./HAL/KEY.d \
./HAL/LED.d \
./HAL/MCU.d \
./HAL/RTC.d \
./HAL/SLEEP.d

OBJS += \
./HAL/KEY.o \
./HAL/LED.o \
./HAL/MCU.o \
./HAL/RTC.o \
./HAL/SLEEP.o



# Each subdirectory must supply rules for building sources it contributes
HAL/%.o: ../HAL/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GNU RISC-V Cross C Compiler'
	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DCH32V20x_D8W -I"$(SRC_DIR)/SRC/Core" -I"$(SRC_DIR)/SRC/Startup" -I"$(SRC_DIR)/SRC/Debug" -I"$(SRC_DIR)/SRC/Peripheral/inc" -I"$(SRC_DIR)/APP/include" -I"$(SRC_DIR)/Profile/include" -I"$(SRC_DIR)/HAL/include" -I"$(SRC_DIR)/LIB" -I"$(SRC_DIR)/APP/UART" -I"$(SRC_DIR)/APP/USBLIB/CONFIG" -I"$(SRC_DIR)/APP/USBLIB/USB-Driver/inc" $(CFLAGS) -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@

endif # ENABLE_BLE