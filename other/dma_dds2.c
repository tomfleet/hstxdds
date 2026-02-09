#include "dma_dds.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h" // Assuming you have this

// Use the standard HSTX DREQ (30 on RP2350)
#ifndef DREQ_HSTX
#define DREQ_HSTX 30 
#endif

static int dma_chan = -1;

// Interrupt Handler: KEEP IT SHORT. NO PRINTF.
void dma_handler() {
    // Clear the interrupt request for this channel
    dma_hw->ints0 = 1u << dma_chan;
    // Do NOT print here. It corrupts RTT.
    // Set a flag if you need to notify the main loop.
}

void dds_init_rtt() {
    // 1. Reset HSTX (Mandatory)
    reset_block(RESETS_RESET_HSTX_BITS);
    unreset_block_wait(RESETS_RESET_HSTX_BITS);

    // 2. Clock HSTX (Mandatory)
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, 
                    125000000, 125000000);

    // 3. Configure Pins (GPIO 12-19 for HSTX)
    for (int i = 12; i <= 19; i++) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }

    // 4. Enable HSTX Peripheral 
    // (This was likely missing or done implicitly before)
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EN_BITS;

    // 5. Claim DMA Channel
    dma_chan = dma_claim_unused_channel(true);
    
    // Setup Interrupts
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    SEGGER_RTT_WriteString(0, "Init: HSTX & DMA Configured\n");
}

void apply_dds_config(dds_config_t *config) {
    if (dma_chan < 0) return;

    dma_channel_config c = dma_channel_get_default_config(dma_chan);

    // CRITICAL: Pace the data transfer!
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false); // FIFO is fixed address
    channel_config_set_dreq(&c, DREQ_HSTX);        // <--- THIS PREVENTS THE CRASH

    SEGGER_RTT_printf(0, "DMA Config: buf=0x%p, len=%d\n", config->buffer_address, config->length_words);

    dma_channel_configure(
        dma_chan,
        &c,
        &hstx_fifo_hw->fifo,    // Destination: HSTX FIFO
        config->buffer_address, // Source: Your Waveform in RAM
        config->length_words,   // Count
        true                    // Start immediately
    );
}