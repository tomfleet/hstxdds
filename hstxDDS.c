#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/interp.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "board/pins.h"
#include "dma_dds/dma_dds.h"
#include "blink.pio.h"
#include "hardware/structs/pll.h"
#include "hardware/regs/pll.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>
//#include "reboot_helper.h"

// This function MUST run from RAM because the Flash chip (XIP) 
// will be temporarily unreachable or unstable during the PLL transition.
void __no_inline_not_in_flash_func(configure_clocks_safe)() {
    // 1. Set voltage to stock 1.1V for 150MHz
    vreg_set_voltage(VREG_VOLTAGE_1_10);
    
    // Give the voltage regulator time to settle without using Flash-based sleep
    for (volatile int i = 0; i < 2000; i++) __asm("nop");

    // 2. Attempt 150MHz PLL configuration
    // This is the line where your debugger currently hangs.
    //set_sys_clock_khz(150000, true);
    set_sys_clock_pll(900 * MHZ, 6, 1);
// Manually wait for the hardware lock bit instead of relying on a return value
    while (!(pll_sys_hw->cs & PLL_CS_LOCK_BITS)) {
        __asm("nop");
    }
   // Update peripheral clock so UART/RTT work at the new speed
    
        clock_configure(clk_peri,
                        0,
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                        150 * MHZ,
                        150 * MHZ);
    }

// Data will be copied from src to dst
const char src[] = "Hello, world! (from DMA)";
char dst[count_of(src)];


void blink_pin_forever(PIO pio, uint sm, uint offset, uint pin, uint freq) {
    blink_program_init(pio, sm, offset, pin);
    pio_sm_set_enabled(pio, sm, true);

    printf("Blinking pin %d at %d Hz\n", pin, freq);

    // PIO counter program takes 3 more cycles in total than we pass as
    // input (wait for n + 1; mov; jmp)
    pio->txf[sm] = (125000000 / (2 * freq)) - 3;
}


int64_t alarm_callback(alarm_id_t id, void *user_data) {
    // Put your timeout handler code in here
    return 0;
}



int main() {

    // 1.1V is default; 1.2V is recommended for 200MHz+
    //vreg_set_voltage(VREG_VOLTAGE_1_10);
    //sleep_ms(10);
    //set_sys_clock_khz(150000, true);    
    // [FIX] Switch system clock to run directly from the 12 MHz XOSC.
    // // This bypasses the PLL constraints.
    // clock_configure(clk_sys,
    //                 CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
    //                 CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
    //                 12 * MHZ,
    //                 12 * MHZ);

    // Re-init standard IO (UART/USB) now that clock changed
   
   configure_clocks_safe();
    //stdio_init_all(); 
    
    
    
    //stdio_init_all();
    //set_sys_clock_khz(20000, true);
    // Initialize hardware and RTT channels 0 and 1
//SEGGER_RTT_Init();

    dds_init(LED_PIN); 
    uint32_t speed_hz = clock_get_hz(clk_sys);
    SEGGER_RTT_printf(0, "System Clock is now %u MHz (%u Hz)\n", speed_hz / 1000000, speed_hz);
    //SEGGER_RTT_printf(0, "System Clock is now 12 MHz\n");

    // Enable watchdog
    //watchdog_enable(100, 1);

    while (1) {
        // High-level mailbox now handles all RTT traffic exclusively
        process_mailbox();
        //check_bootsel_reboot();
        //watchdog_update();
    }
}

    