################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include Middlewares/ST/STM32_ExtMem_Manager/subdir.mk
-include Middlewares/ST/STM32_ExtMem_Loader/subdir.mk
-include Drivers/STM32H7RSxx_HAL_Driver/subdir.mk
-include Core/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := h7s3_test_platform_ExtMemLoader
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
h7s3_test_platform_ExtMemLoader.elf \

MAP_FILES += \
h7s3_test_platform_ExtMemLoader.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
h7s3_test_platform_ExtMemLoader.list \


# All Target
all:
	+@$(MAKE) --no-print-directory main-build && $(MAKE) --no-print-directory post-build

# Main-build Target
main-build: h7s3_test_platform_ExtMemLoader.elf secondary-outputs

# Tool invocations
h7s3_test_platform_ExtMemLoader.elf h7s3_test_platform_ExtMemLoader.map: $(OBJS) $(USER_OBJS) C:\Users\Jedrz\Desktop\git\ files\git\DRSSTC_2\DRSSTC_Controller_CODE\h7s3_test_platform\ExtMemLoader\STM32H7S3L8HX_extmemloader_default.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-gcc -o "h7s3_test_platform_ExtMemLoader.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m7 -T"C:\Users\Jedrz\Desktop\git files\git\DRSSTC_2\DRSSTC_Controller_CODE\h7s3_test_platform\ExtMemLoader\STM32H7S3L8HX_extmemloader_default.ld" --specs=nosys.specs -Wl,-Map="h7s3_test_platform_ExtMemLoader.map" -Wl,--gc-sections -static --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo 'Finished building: $@'
	@echo ' '

h7s3_test_platform_ExtMemLoader.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "h7s3_test_platform_ExtMemLoader.list"
	@echo 'Finished building: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) default.size.stdout h7s3_test_platform_ExtMemLoader.elf h7s3_test_platform_ExtMemLoader.list h7s3_test_platform_ExtMemLoader.map
	-@echo ' '

post-build:
	bash "../postbuild.sh" "C:\ST\STM32CubeIDE_1.18.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.200.202503041107\tools\bin/ExternalLoader"
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified post-build

-include ../makefile.targets
