#ifndef REBOOT_HELPER_H
#define REBOOT_HELPER_H

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/sync.h"
#include "hardware/structs/io_qspi.h"
#include "hardware/structs/sio.h"
#include "hardware/gpio.h"

/**
 * @brief Checks the BOOTSEL button status on RP2350.
 * * This must be marked __no_inline_not_in_flash_func because pressing 
 * BOOTSEL disconnects the Flash CS line. Running this from Flash 
 * would cause an immediate hard fault.
 */
static bool __no_inline_not_in_flash_func(get_bootsel_button)() {
    // On Pico 2 (RP2350), the QSPI SS (CS) is index 1 in the IO_QSPI bank
    const uint CS_PIN_INDEX = 1;

    // 1. Disable interrupts so no code tries to access Flash while we are messing with CS
    uint32_t flags = save_and_disable_interrupts();

    // 2. High-level override: Disconnect the QSPI controller from the pin 
    // so we can use the SIO to read the raw state.
    // We set OEOVER to DISABLE (GPIO_OVERRIDE_LOW = 2)
    uint32_t prev_ctrl = io_qspi_hw->io[CS_PIN_INDEX].ctrl;
    io_qspi_hw->io[CS_PIN_INDEX].ctrl = (prev_ctrl & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) |
                                         (2 << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);

    // 3. Small delay for synchronization (must use NOPs in RAM)
    for (volatile int i = 0; i < 30; ++i) __asm("nop");

    // 4. Read the HI GPIO input register. 
    // The BOOTSEL button pulls this line LOW when pressed.
    bool button_pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    // 5. Restore the original register state to reconnect Flash
    io_qspi_hw->io[CS_PIN_INDEX].ctrl = prev_ctrl;

    // 6. Restore interrupts
    restore_interrupts(flags);

    return button_pressed;
}

/**
 * @brief Logic to handle the reboot sequence. 
 * Detects press -> waits for release -> jumps to bootloader.
 */
static inline void check_bootsel_reboot() {
    if (get_bootsel_button()) {
        // Confirm with a small debounce
        sleep_ms(20);
        if (!get_bootsel_button()) return;

        // USER IS HOLDING THE BUTTON
        // Stay in this RAM-based loop until they let go.
        // This prevents the ROM bootloader from starting while CS is still disconnected.
        while (get_bootsel_button()) {
            tight_loop_contents(); 
        }

        // Give the Flash chip and CS line 100ms to stabilize after physical release
        sleep_ms(100);

        // Jump to USB Bootloader (0,0 = No specific interface constraints)
        reset_usb_boot(0, 0);
    }
}

#endif // REBOOT_HELPER_H