# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD solderparty_rp2350_stamp_xl CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(hstxDDS C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()
set(PICO_BOARD pico2)
set(PICO_PLATFORM rp2350-arm-s)
# Add executable. Default name is the project name, version 0.1
# Add the directory to the include path so "dma_dds.h" can be found
include_directories(dma_dds
                other/SEGGER_RTT/RTT
                board)

add_executable(hstxDDS 
                hstxDDS.c 
                dma_dds/dma_dds.c
                other/SEGGER_RTT/RTT/SEGGER_RTT.c
                other/SEGGER_RTT/RTT/SEGGER_RTT_printf.c
                )

pico_set_program_name(hstxDDS "hstxDDS")
pico_set_program_version(hstxDDS "0.1")

# Generate PIO header
pico_generate_pio_header(hstxDDS ${CMAKE_CURRENT_LIST_DIR}/blink.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(hstxDDS 1)
pico_enable_stdio_usb(hstxDDS 0)

# # Add the standard library to the build
# target_link_libraries(hstxDDS
        
#     hardware_dma
#     hardware_hstx
#     )

# Add the standard include files to the build
target_include_directories(hstxDDS PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(hstxDDS 
    pico_stdlib
        hardware_pwm 
        hardware_spi
        hardware_i2c
        hardware_dma
        hardware_pio
        hardware_interp
        hardware_timer
        hardware_watchdog
        hardware_clocks
        pico_base_headers
	    pico_util
        )

pico_add_extra_outputs(hstxDDS)

