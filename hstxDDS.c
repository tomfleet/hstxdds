#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/interp.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "board/pins.h"
#include "dma_dds/dma_dds.h"

#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>
#include "reboot_helper.h"



// Data will be copied from src to dst
const char src[] = "Hello, world! (from DMA)";
char dst[count_of(src)];

#include "blink.pio.h"

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
    //set_sys_clock_khz(20000, true);
    
    // [FIX] Switch system clock to run directly from the 12 MHz XOSC.
    // This bypasses the PLL constraints.
    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLK_REF,
                    CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                    12 * MHZ,
                    12 * MHZ);

    // Re-init standard IO (UART/USB) now that clock changed
    stdio_init_all(); 
    
    
    
    //stdio_init_all();
    //set_sys_clock_khz(20000, true);
    // Initialize hardware and RTT channels 0 and 1
    dds_init(LED_PIN); 
    SEGGER_RTT_printf(0, "System Clock is now 12 MHz\n");

    // Enable watchdog
    watchdog_enable(100, 1);

    while (1) {
        // High-level mailbox now handles all RTT traffic exclusively
        process_mailbox();
        //check_bootsel_reboot();
        watchdog_update();
    }
}

    