cmake_minimum_required(VERSION 3.31)

project(UAM C ASM)

include(CheckCCompilerFlag)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_FLAGS_DEBUG "" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_MINSIZEREL "" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_DEBUG "" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_RELWITHDEBINFO "" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_MINSIZEREL "" CACHE STRING "" FORCE)

set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/STM32H723VETX_FLASH.ld")

set(OPTIONAL_COMPILE_OPTIONS)
check_c_compiler_flag("-fstack-usage" COMPILER_SUPPORTS_FSTACK_USAGE)
if(COMPILER_SUPPORTS_FSTACK_USAGE)
    list(APPEND OPTIONAL_COMPILE_OPTIONS -fstack-usage)
endif()

check_c_compiler_flag("-fcyclomatic-complexity" COMPILER_SUPPORTS_FCYCLOMATIC_COMPLEXITY)
if(COMPILER_SUPPORTS_FCYCLOMATIC_COMPLEXITY)
    list(APPEND OPTIONAL_COMPILE_OPTIONS -fcyclomatic-complexity)
endif()

set(APPLICATION_SOURCES
    Application/Src/CFG/cfg_callbacks.c
    Application/Src/CFG/cfg_import_export.c
    Application/Src/CFG/cfg_main.c
    Application/Src/CFG/cfg_parameters.c
    Application/Src/COMM/comm_commands.c
    Application/Src/COMM/comm_config_menu.c
    Application/Src/COMM/comm_debug_menu.c
    Application/Src/COMM/comm_evaluation_menu.c
    Application/Src/COMM/comm_function_loops.c
    Application/Src/COMM/comm_hist_menu.c
    Application/Src/COMM/comm_janus_menu.c
    Application/Src/COMM/comm_main.c
    Application/Src/COMM/comm_main_menu.c
    Application/Src/COMM/comm_menu_system.c
    Application/Src/COMM/comm_print.c
    Application/Src/COMM/comm_txrx_menu.c
    Application/Src/DAC/dac_main.c
    Application/Src/DAC/dac_waveform.c
    Application/Src/FILT/filt_main.c
    Application/Src/MAC/mac_csma_ca_beb.c
    Application/Src/MAC/mac_main.c
    Application/Src/MAC/mac_no_mac.c
    Application/Src/MESS/mess_background_noise.c
    Application/Src/MESS/mess_calibration.c
    Application/Src/MESS/mess_cargo.c
    Application/Src/MESS/mess_demodulate.c
    Application/Src/MESS/mess_error_correction.c
    Application/Src/MESS/mess_error_detection.c
    Application/Src/MESS/mess_evaluate.c
    Application/Src/MESS/mess_feedback.c
    Application/Src/MESS/mess_feedback_tests.c
    Application/Src/MESS/mess_input.c
    Application/Src/MESS/mess_interleaver.c
    Application/Src/MESS/mess_main.c
    Application/Src/MESS/mess_modulate.c
    Application/Src/MESS/mess_packet.c
    Application/Src/MESS/mess_preamble.c
    Application/Src/MESS/mess_ranging.c
    Application/Src/MESS/mess_sync.c
    Application/Src/SYS/sleep/sleep_config.c
    Application/Src/SYS/sleep/sleep_deep.c
    Application/Src/SYS/sleep/sleep_manager.c
    Application/Src/SYS/sleep/wakeup_tones.c
    Application/Src/SYS/sys_led.c
    Application/Src/SYS/sys_main.c
    Application/Src/SYS/sys_power.c
    Application/Src/SYS/sys_pressure.c
    Application/Src/SYS/sys_sensor_timer.c
    Application/Src/SYS/sys_temperature.c
    Application/Src/afe/internal/dac_switch.c
    Application/Src/afe/internal/feedback.c
    Application/Src/afe/internal/tpa32xx-driver.c
    Application/Src/afe/internal/tr_switch.c
    Application/Src/afe/afe.c
    Application/Src/bsp/bkpsram_layout.c
    Application/Src/bsp/power_leds.c
    Application/Src/bsp/pwr_domains.c
    Application/Src/common/mess_dac_resources.c
    Application/Src/common/mess_filt_resources.c
    Application/Src/common/utils/check_inputs.c
    Application/Src/common/utils/format_timestamp.c
    Application/Src/common/utils/goertzel.c
    Application/Src/common/utils/number_utils.c
    Application/Src/common/utils/prbs.c
    Application/Src/common/utils/uam_math.c
    Application/Src/drivers/dau_card-driver.c
    Application/Src/drivers/ina219-driver.c
    Application/Src/drivers/lps22hh-driver.c
    Application/Src/drivers/pga113-driver.c
    Application/Src/drivers/usb_comm.c
    Application/Src/drivers/ws2812b-driver.c
    Application/Src/error/error_log.c
    Application/Src/error/error_manager.c
    Application/Src/error/error_reset.c
    Application/Src/error/error_subsys.c
    Application/Third_Party/Base64/base64.c
)

set(CORE_SOURCES
    Core/Src/freertos.c
    Core/Src/main.c
    Core/Src/stm32h7xx_hal_msp.c
    Core/Src/stm32h7xx_hal_timebase_tim.c
    Core/Src/stm32h7xx_it.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    Core/Src/system_stm32h7xx.c
)

set(USB_DEVICE_SOURCES
    USB_DEVICE/App/usb_device.c
    USB_DEVICE/App/usbd_cdc_if.c
    USB_DEVICE/App/usbd_desc.c
    USB_DEVICE/Target/usbd_conf.c
)

set(HAL_SOURCES
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_adc_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cordic.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_cortex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dac.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dac_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dma_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_dts.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_exti.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_flash_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_fmac.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_gpio.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hsem.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_i2c_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_mdma.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_ospi.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pcd.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pcd_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_pwr_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rcc_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_rtc_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_spi_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_uart_ex.c
    Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_usb.c
)

set(FREERTOS_SOURCES
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c
    Middlewares/Third_Party/FreeRTOS/Source/croutine.c
    Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
    Middlewares/Third_Party/FreeRTOS/Source/list.c
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c
    Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
    Middlewares/Third_Party/FreeRTOS/Source/queue.c
    Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
    Middlewares/Third_Party/FreeRTOS/Source/tasks.c
    Middlewares/Third_Party/FreeRTOS/Source/timers.c
)

set(USB_MIDDLEWARE_SOURCES
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c
)

set(STARTUP_SOURCES
    Core/Startup/startup_stm32h723vetx.s
)

add_executable(UAM.elf
    ${APPLICATION_SOURCES}
    ${CORE_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
    ${USB_MIDDLEWARE_SOURCES}
    ${STARTUP_SOURCES}
)

target_compile_definitions(UAM.elf PRIVATE
    ARM_MATH_CM7
    USE_PWR_LDO_SUPPLY
    USE_HAL_DRIVER
    STM32H723xx
    $<$<CONFIG:Debug>:DEBUG>
)

target_include_directories(UAM.elf PRIVATE
    Core/Inc
    Drivers/CMSIS/DSP/Include
    Drivers/STM32H7xx_HAL_Driver/Inc
    Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    Drivers/CMSIS/Device/ST/STM32H7xx/Include
    Drivers/CMSIS/Include
    Application/Inc
    Application/Inc/drivers
    Application/Src
    Application/Src/COMM
    Application/Inc/COMM
    Application/Third_Party/Base64
    Application/Inc/common/utils
    Application/Src/common/utils
    Application/Inc/MESS
    Application/Src/MESS
    Application/Inc/SYS
    Application/Src/SYS
    Application/Inc/CFG
    Application/Src/CFG
    Application/Src/DAC
    Application/Inc/DAC
    Application/Inc/common
    Application/Src/common
    Application/Src/SYS/sleep
    Application/Inc/MAC
    Application/Src/MAC
    USB_DEVICE/App
    USB_DEVICE/Target
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
    Application/Inc/afe
    Application/Inc/bsp
    Application/Src/afe
    Application/Src/bsp
    Application/Src/afe/internal
    Application/Inc/afe/internal
    Application/Src/error
    Application/Inc/error
    Application/Inc/FILT
    Application/Src/FILT
)

target_compile_options(UAM.elf PRIVATE
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wswitch-default
    ${OPTIONAL_COMPILE_OPTIONS}
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<CONFIG:Debug>:-Og;-g3>
    $<$<CONFIG:Release>:-Os;-g0>
)

target_link_directories(UAM.elf PRIVATE
    "${CMAKE_SOURCE_DIR}/Drivers/CMSIS/Lib/GCC"
)

target_link_options(UAM.elf PRIVATE
    -mcpu=cortex-m7
    -mthumb
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -T${LINKER_SCRIPT}
    --specs=nosys.specs
    --specs=nano.specs
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/UAM.map
    -Wl,--gc-sections
    -static
    -Wl,-u,_printf_float
    -Wl,-u,_scanf_float
)

target_link_libraries(UAM.elf PRIVATE
    -Wl,--start-group
    c
    m
    arm_cortexM7lfsp_math
    -Wl,--end-group
)

set_target_properties(UAM.elf PROPERTIES
    OUTPUT_NAME UAM
    SUFFIX ".elf"
)

add_custom_command(TARGET UAM.elf POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:UAM.elf>" "${CMAKE_CURRENT_BINARY_DIR}/UAM.bin"
    COMMAND /bin/bash -lc "\"${CMAKE_OBJDUMP}\" -h -S \"$<TARGET_FILE:UAM.elf>\" > \"${CMAKE_CURRENT_BINARY_DIR}/UAM.list\""
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:UAM.elf>"
    BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/UAM.bin"
        "${CMAKE_CURRENT_BINARY_DIR}/UAM.list"
    VERBATIM
)
