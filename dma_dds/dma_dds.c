#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/resets.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>
#include "pico/time.h"
#include "hardware/sync.h"
#include "board/pins.h"
#include "math.h"
#include "dma_dds/dma_dds.h"

// Memory Allocation
uint8_t waveform_buffer[MAX_WAVEFORM_SIZE] __attribute__((aligned(4)));

dds_config_t current_config = {
    .waveform_buffer = waveform_buffer,
    .buffer_len = 0,
    .fstart = 0,
    .fend = 0,
    .duration_ms = 0
};

// Internal RTT Buffers for Channel 1
static uint8_t rtt_bin_up_buf[10248];   
static uint8_t rtt_bin_down_buf[10248]; 
static uint8_t rtt_ch0_down_buf[256];
static int dds_dma_chan = -1;
static volatile bool dma_busy = false;
static uint dds_slice;

// State Machine Variables
typedef enum { IDLE, READ_HEADER, READ_DATA } mbox_state_t;
static mbox_state_t current_state = IDLE;
static dds_header_t incoming_header;
static uint8_t header_work_buf[20];
static uint32_t header_pos = 0;
static uint32_t data_pos = 0;
static uint32_t last_byte_time_ms = 0;


// --- HARDWARE CONFIGURATION ---
// Stock Pico 2 HSTX Pins are usually GP12 - GP19
#define HSTX_START_PIN 12 

void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    SEGGER_RTT_printf(0, "hstx_fifo_hw @ 0x%08X\n", (uint32_t)hstx_fifo_hw);
    SEGGER_RTT_printf(0, "hstx_fifo_hw->fifo @ 0x%08X\n", (uint32_t)&hstx_fifo_hw->fifo);
    SEGGER_RTT_WriteString(0, "HALT before DMA\n");
    //while (1) { __breakpoint(); } // halt for inspection

    SEGGER_RTT_printf(0, "apply_dds_config called: freq=%u, len=%u, chan=%d\n", freq_hz, len, dds_dma_chan);
    if (dma_busy) {
        SEGGER_RTT_WriteString(0, "DMA busy, skipping new transfer\n");
        return;
    }
    if (len == 0 || freq_hz == 0 || dds_dma_chan < 0) {
        SEGGER_RTT_printf(0, "Early return: len=%u, freq=%u, chan=%d\n", len, freq_hz, dds_dma_chan);
        return;
    }
    // Safety: clamp length to buffer size
    if (len > MAX_WAVEFORM_SIZE) len = MAX_WAVEFORM_SIZE;
    
    // Validate hardware pointers before use
    if (!hstx_ctrl_hw || !hstx_fifo_hw) {
        SEGGER_RTT_printf(0, "ERROR: hstx_ctrl_hw=0x%08X, hstx_fifo_hw=0x%08X\n", 
                         (uint32_t)hstx_ctrl_hw, (uint32_t)hstx_fifo_hw);
        return;
    }
    
    // 1. Clock Divider Calculation
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div < 1) div = 1;
    if (div > 4095) div = 4095;

    SEGGER_RTT_printf(0, "DMA Config: buf=0x%08X, len=%u, freq=%u Hz\n", 
                     (uint32_t)buf, len, freq_hz);

    // 2. HSTX Configuration (with proper memory barriers)
    uint32_t save_irq = save_and_disable_interrupts();
    
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) | 
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) | 
                       HSTX_CTRL_CSR_EN_BITS;
    
    // 3. Bit Mapping (1:1)
    for (int i = 0; i < 8; i++) {
        hstx_ctrl_hw->bit[i] = i; 
    }

    // 4. DMA Configuration
    dma_channel_abort(dds_dma_chan);
    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);

    // 5. Memory Barrier & Launch
    __dmb();
    dma_busy = true;
    // IMPORTANT: DO NOT auto-start - set to false and start manually
    dma_channel_configure(dds_dma_chan, &c, (void *)&hstx_fifo_hw->fifo, buf, len, false);
    
    // Now manually start the transfer
    dma_channel_set_irq0_enabled(dds_dma_chan, true);
    dma_channel_start(dds_dma_chan);
    
    restore_interrupts(save_irq);
    
    SEGGER_RTT_WriteString(0, "DMA started\n");
}

void __isr_dma_handler(void) {
    if (dma_hw->ints0 & (1u << dds_dma_chan)) {
        dma_hw->ints0 = 1u << dds_dma_chan; // Clear interrupt
        dma_busy = false;
        //SEGGER_RTT_WriteString(0, "DMA complete\n");
    }
}

// Generate a simple Sine Wave into the buffer (no DMA start)
void generate_waveform_sine(void) {
    // Generate 1024 points of a sine wave (0..255)
    int len = 1024;
    for (int i = 0; i < len; i++) {
        float angle = (2.0f * 3.14159f * i) / len;
        // Scale -1..1 to 0..255
        float val = (sinf(angle) + 1.0f) * 127.5f;
        waveform_buffer[i] = (uint8_t)val;
    }
    current_config.buffer_len = len;
}

// Generate waveform AND start DMA transmission
void generate_standalone_sine(void) {
    // Only generate and start if no user waveform is present
    if (current_config.buffer_len == 0) {
        generate_waveform_sine();
        // Start at slow speed for testing (1 kHz sample rate, not 10 MHz)
        apply_dds_config(1000000, waveform_buffer, current_config.buffer_len);
    } else {
        // If user waveform is present, just start DMA with it
        apply_dds_config(current_config.fstart ? current_config.fstart : 1000000, waveform_buffer, current_config.buffer_len);
    }
}

void dds_init_rtt(uint gpio_pin) {
    // --- RTT & Buffer Setup ---
    SEGGER_RTT_Init();
    SEGGER_RTT_ConfigUpBuffer(1, "DataOut", rtt_bin_up_buf, sizeof(rtt_bin_up_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(1, "DataIn", rtt_bin_down_buf, sizeof(rtt_bin_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    // Ensure Channel 0 is configured
    SEGGER_RTT_ConfigDownBuffer(0, "Terminal", rtt_ch0_down_buf, sizeof(rtt_ch0_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);

    SEGGER_RTT_WriteString(0, "Init: RTT initialized\n");

    // --- CRITICAL FIX: HSTX Hardware Initialization ---
    // 1. Reset the HSTX block (Fixes Bus Fault on register access)
    reset_block(RESETS_RESET_HSTX_BITS);
    unreset_block_wait(RESETS_RESET_HSTX_BITS);

    // 2. Configure HSTX Clock to System Clock (125 MHz)
    clock_configure(
        clk_hstx,
        0,
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
        125000000, 
        125000000
    );

    // 3. Configure GPIO pins 12-19 for HSTX
    // (Without this, signals don't leave the chip)
    for (int i = 12; i <= 19; i++) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }

    // 4. Enable the HSTX Peripheral Block
    // (Must be done before writing to FIFO or CSR)
    if (hstx_ctrl_hw) {
         hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EN_BITS;
    }
    // --------------------------------------------------

    // --- PWM Setup (Preserved) ---
    gpio_set_function(0, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(0);
    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 4.f);
    pwm_init(slice_num, &config, true);
    pwm_set_gpio_level(0, 0); 

    // --- DMA Setup (Preserved) ---
    dds_dma_chan = dma_claim_unused_channel(true);
    SEGGER_RTT_printf(0, "Init: DMA channel %d claimed\n", dds_dma_chan);

    dma_channel_set_irq0_enabled(dds_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, __isr_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Load default
    generate_waveform_sine(); // uses internal helper
    SEGGER_RTT_WriteString(0, "Init: default waveform loaded\n");


    // // Configure PWM
    // gpio_set_function(3, GPIO_FUNC_PWM);
    // dds_slice = pwm_gpio_to_slice_num(3);
    // pwm_config cfg = pwm_get_default_config();
    // pwm_config_set_wrap(&cfg, 255);
    // pwm_init(dds_slice, &cfg, true);
    // SEGGER_RTT_WriteString(0, "Init: PWM done\n");

    
    SEGGER_RTT_WriteString(0, "Init: COMPLETE - Ready for RTT commands\n");
}

void dds_init_rtt2(uint gpio_pin) {
    // --- HSTX Peripheral Initialization ---
    // 1. De-assert reset and enable clock for HSTX peripheral
    // These macros/constants are typical for RP2xxx SDKs; adjust if needed for RP2350
    // HSTX_CTRL and HSTX_FIFO are separate blocks, both must be released from reset and clocked
    // See: https://datasheets.raspberrypi.com/bbrp2350/rp2350-datasheet.pdf
    // If not defined, you may need to define RESETS_RESET_HSTX_CTRL, RESETS_RESET_HSTX_FIFO, etc.
    reset_block(RESETS_RESET_HSTX_BITS);
    unreset_block_wait(RESETS_RESET_HSTX_BITS);

    clock_configure(
        clk_hstx,
        0,
        CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
        125 * 1000 * 1000, // Input frequency (assuming 125MHz sys clock)
        125 * 1000 * 1000  // Output frequency
        );

    SEGGER_RTT_WriteString(0, "Init: HSTX peripheral clocks and resets enabled\n");


    // Set Pins 12-19 to HSTX
    for (int i = 0; i < 8; i++) {
        gpio_set_function(HSTX_START_PIN + i, GPIO_FUNC_HSTX);
    }
    SEGGER_RTT_WriteString(0, "Init: GPIO HSTX done\n");

    // Initialize RTT FIRST
    SEGGER_RTT_Init();
    SEGGER_RTT_WriteString(0, "Init: RTT initialized\n");
    
    // Config Channel 1 for Binary (Index 1)
    SEGGER_RTT_ConfigUpBuffer(1, "DataOut", rtt_bin_up_buf, sizeof(rtt_bin_up_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(1, "DataIn", rtt_bin_down_buf, sizeof(rtt_bin_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(0, "Terminal", rtt_ch0_down_buf, sizeof(rtt_ch0_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_WriteString(0, "Init: buffers done\n");

    // Claim DMA channel FIRST (needed by apply_dds_config)
    if (dds_dma_chan < 0) {
        dds_dma_chan = dma_claim_unused_channel(true);
        SEGGER_RTT_printf(0, "Init: DMA channel %d claimed\n", dds_dma_chan);
        irq_set_exclusive_handler(DMA_IRQ_0, __isr_dma_handler);
        irq_set_enabled(DMA_IRQ_0, true);
    }

    // Populate waveform buffer (but don't start DMA yet)
    generate_waveform_sine();
    SEGGER_RTT_WriteString(0, "Init: default waveform loaded\n");


    // Configure PWM
    gpio_set_function(3, GPIO_FUNC_PWM);
    dds_slice = pwm_gpio_to_slice_num(gpio_pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(dds_slice, &cfg, true);
    SEGGER_RTT_WriteString(0, "Init: PWM done\n");

    SEGGER_RTT_WriteString(0, "Init: COMPLETE - Ready for RTT commands\n");
}

void dds_init(uint gpio_pin) {
    // Initialize standard IO just in case
    //stdio_init_all();

    // 1. Set Pins 12-19 to HSTX
    for (int i = 0; i < 8; i++) {
        gpio_set_function(HSTX_START_PIN + i, GPIO_FUNC_HSTX);
    }

    // 2. Claim DMA
    if (dds_dma_chan < 0) dds_dma_chan = dma_claim_unused_channel(true);

    // 3. AUTO-START (Bypassing RTT)
    generate_standalone_sine();
}

void process_mailbox() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Timeout: Reset if stuck mid-transfer for > 500ms
    if (current_state != IDLE && (now - last_byte_time_ms > 500)) {
        current_state = IDLE;
        header_pos = 0;
        data_pos = 0;
        SEGGER_RTT_WriteString(0, "Mbox: Timeout Reset\n");
    }

    switch (current_state) {
        case IDLE:
            if (SEGGER_RTT_HasData(0)) {
                uint8_t c;
                SEGGER_RTT_Read(0, &c, 1);
                if (c == 0xEF) { 
                    header_work_buf[0] = 0xEF;
                    header_pos = 1;
                    last_byte_time_ms = now;
                    current_state = READ_HEADER;
                } else if (c == 'r') {
                    SEGGER_RTT_Write(1, waveform_buffer, current_config.buffer_len);
                } else if (c == 's') {
                    if (!dma_busy) {
                        SEGGER_RTT_WriteString(0, "Starting waveform...\n");
                        generate_standalone_sine();
                        SEGGER_RTT_WriteString(0, "Waveform started\n");
                    } else {
                        SEGGER_RTT_WriteString(0, "DMA busy, cannot start new transfer\n");
                    }
                } else if (c == 'x' || c == 'X') {
                    SEGGER_RTT_WriteString(0, "Stopping DMA...\n");
                    dma_channel_abort(dds_dma_chan);
                    dma_busy = false;
                    SEGGER_RTT_WriteString(0, "DMA stopped\n");
                } else if (c == '?') {
                    SEGGER_RTT_WriteString(0, "Commands: s=start, x=stop, r=read, ?=help\n");
                }
            }
            break;

        case READ_HEADER:
            // Pull bytes one-by-one to handle trickling J-Link data
            while (SEGGER_RTT_HasData(0) && header_pos < 20) {
                SEGGER_RTT_Read(0, &header_work_buf[header_pos++], 1);
                last_byte_time_ms = now;
            }

            if (header_pos == 20) {
                memcpy(&incoming_header, header_work_buf, 20);
                if (incoming_header.sync == 0xDEADBEEF) {
                    SEGGER_RTT_printf(0, "Header OK: %u->%u Hz, %u ms, len=%u\n", 
                                    incoming_header.fstart, incoming_header.fend, 
                                    incoming_header.duration, incoming_header.len);
                    data_pos = 0;
                    current_state = READ_DATA;
                } else {
                    SEGGER_RTT_printf(0, "Bad sync: 0x%08X\n", incoming_header.sync);
                    current_state = IDLE;
                    header_pos = 0;
                }
            }
            break;

        case READ_DATA:
            while (SEGGER_RTT_HasData(1) && data_pos < incoming_header.len) {
                uint32_t r = SEGGER_RTT_Read(1, &waveform_buffer[data_pos], 
                                            (incoming_header.len - data_pos));
                data_pos += r;
                last_byte_time_ms = now;
            }

            if (data_pos >= incoming_header.len) {
                current_config.buffer_len = data_pos;
                current_config.fstart = incoming_header.fstart;
                current_config.fend = incoming_header.fend;
                current_config.duration_ms = incoming_header.duration;
                apply_dds_config(current_config.fstart, waveform_buffer, data_pos);
                SEGGER_RTT_printf(0, "DDS: Success %u bytes @ %u Hz\n", data_pos, incoming_header.fstart);
                current_state = IDLE;
                header_pos = 0;
            }
            break;
    }
}


