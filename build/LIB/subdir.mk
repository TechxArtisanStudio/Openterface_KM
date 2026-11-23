################################################################################
# MRS Version: 2.1.0
# Automatically-generated file. Do not edit!
################################################################################

ifeq ($(ENABLE_BLE),1)
# Add inputs and outputs from these tool invocations to the build variables
S_UPPER_SRCS += \
../LIB/ble_task_scheduler.S

S_UPPER_DEPS += \
./LIB/ble_task_scheduler.d

OBJS += \
./LIB/ble_task_scheduler.o



# Each subdirectory must supply rules for building sources it contributes
LIB/%.o: ../LIB/%.S
	@echo 'Building file: $<'
	@echo 'Invoking: GNU RISC-V Cross C Compiler'
	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -x assembler-with-cpp -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@

endif # ENABLE_BLE