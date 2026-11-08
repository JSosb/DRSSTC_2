################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/extmem_manager.c \
../Core/Src/extmemloader_init.c \
../Core/Src/stm32h7rsxx_hal_msp.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32h7rsxx.c 

OBJS += \
./Core/Src/extmem_manager.o \
./Core/Src/extmemloader_init.o \
./Core/Src/stm32h7rsxx_hal_msp.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32h7rsxx.o 

C_DEPS += \
./Core/Src/extmem_manager.d \
./Core/Src/extmemloader_init.d \
./Core/Src/stm32h7rsxx_hal_msp.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32h7rsxx.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32H7S3xx -DSTM32_EXTMEMLOADER_STM32CUBETARGET -c -I../Core/Inc -I../../Drivers/STM32H7RSxx_HAL_Driver/Inc -I../../Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -I../../Middlewares/ST/STM32_ExtMem_Loader/core -I../../Middlewares/ST/STM32_ExtMem_Loader/MDK-ARM -I../../Middlewares/ST/STM32_ExtMem_Loader/STM32Cube -I../../Middlewares/ST/STM32_ExtMem_Manager -I../../Middlewares/ST/STM32_ExtMem_Manager/sal -I../../Middlewares/ST/STM32_ExtMem_Manager/nor_sfdp -I../../Middlewares/ST/STM32_ExtMem_Manager/psram -I../../Middlewares/ST/STM32_ExtMem_Manager/sdcard -I../../Middlewares/ST/STM32_ExtMem_Manager/user -I../../Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/extmem_manager.cyclo ./Core/Src/extmem_manager.d ./Core/Src/extmem_manager.o ./Core/Src/extmem_manager.su ./Core/Src/extmemloader_init.cyclo ./Core/Src/extmemloader_init.d ./Core/Src/extmemloader_init.o ./Core/Src/extmemloader_init.su ./Core/Src/stm32h7rsxx_hal_msp.cyclo ./Core/Src/stm32h7rsxx_hal_msp.d ./Core/Src/stm32h7rsxx_hal_msp.o ./Core/Src/stm32h7rsxx_hal_msp.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32h7rsxx.cyclo ./Core/Src/system_stm32h7rsxx.d ./Core/Src/system_stm32h7rsxx.o ./Core/Src/system_stm32h7rsxx.su

.PHONY: clean-Core-2f-Src

