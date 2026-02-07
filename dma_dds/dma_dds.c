#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>
#include "pico/time.h"
#include "hardware/sync.h"
#include "board/pins.h"
#include <math.h> // Needed for sin()
#include "dma_dds/dma_dds.h"

// Memory Allocation
uint8_t waveform_buffer[MAX_WAVEFORM_SIZE] __attribute__((aligned(4)));

dds_config_t current_config = {
    .waveform_buffer = waveform_buffer,
    .buffer_len = 0,
    .fstart = 0
};

// Internal RTT Buffers for Channel 1
static uint8_t rtt_bin_up_buf[10248];   
static uint8_t rtt_bin_down_buf[10248]; 
static uint8_t rtt_ch0_down_buf[256];
static int dds_dma_chan = -1;
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
    if (len == 0 || freq_hz == 0) return;
    
    // 1. Clock Divider Calculation
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div < 1) div = 1;
    if (div > 4095) div = 4095;

    // 2. HSTX Configuration
    // SHIFT=8 (8 bits per cycle), EN_BITS=1 (Enable)
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) | 
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) | 
                       HSTX_CTRL_CSR_EN_BITS;
    
    // 3. Bit Mapping (1:1)
    // Map Data Bits 0-7 to Output Pins 0-7 (relative to HSTX start)
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

    // 5. Memory Barrier
    __dmb();

    // 6. Launch
    dma_channel_configure(dds_dma_chan, &c, (void *)&hstx_fifo_hw->fifo, buf, len, true);
}



// Generate a simple Sine Wave into the buffer
void generate_standalone_sine(void) {
    // Generate 1024 points of a sine wave (0..255)
    int len = 1024;
    for (int i = 0; i < len; i++) {
        float angle = (2.0f * 3.14159f * i) / len;
        // Scale -1..1 to 0..255
        float val = (sinf(angle) + 1.0f) * 127.5f;
        waveform_buffer[i] = (uint8_t)val;
    }
    
    // Alternative: Sawtooth (good for bit check)
    // for (int i = 0; i < len; i++) waveform_buffer[i] = (uint8_t)(i % 256);

    // Blast it out at ~1 MHz sample rate (or whatever freq you want)
    // If you want a 100Hz tone with 1024 samples, sample rate = 102.4 kHz.
    SEGGER_RTT_printf(0, "Generated standalone sine wave with %d samples\n", len);
    apply_dds_config(100000, waveform_buffer, len);
}

void apply_dds_config3(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    if (len == 0 || freq_hz == 0) return;
    
    // 1. Calculate Clock Divider
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div < 1) div = 1;
    if (div > 4095) div = 4095; // CSR field limit

    // 2. Configure HSTX Control Register
    // SHIFT = 8: Shift out 8 bits per cycle (consume 1 byte from FIFO effectively)
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) | 
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) | 
                       HSTX_CTRL_CSR_EN_BITS;
    
    // 3. [FIX] Configure Bit Mapping (1:1 mapping)
    // Map Data Bit 0 -> Pin 0, Data Bit 1 -> Pin 1, etc.
    for (int i = 0; i < 8; i++) {
        hstx_ctrl_hw->bit[i] = 12 + i; 
    }

    // 4. Configure DMA to feed the beast
    dma_channel_abort(dds_dma_chan);
    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false); // FIFO is at fixed address
    channel_config_set_dreq(&c, DREQ_HSTX);        // Pace transfer by HSTX FIFO

    dma_channel_configure(dds_dma_chan, &c, 
                          (void *)&hstx_fifo_hw->fifo, // Destination: HSTX FIFO
                          buf,                         // Source: Your waveform buffer
                          len,                         // Transfer count
                          true);                       // Start immediately
}

void apply_dds_co2nfig(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    if (len == 0 || freq_hz == 0) return;
    
    // 1. Calculate Clock Divider
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div < 1) div = 1;
    if (div > 4095) div = 4095; // CSR field limit

    // 2. Configure HSTX Control Register
    // SHIFT = 8: Shift out 8 bits per cycle (consume 1 byte from FIFO effectively)
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) | 
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) | 
                       HSTX_CTRL_CSR_EN_BITS;
    
    // 3. [FIX] Configure Bit Mapping (1:1 mapping)
    // Map Data Bit 0 -> Pin 0, Data Bit 1 -> Pin 1, etc.
    for (int i = 0; i < 8; i++) {
        hstx_ctrl_hw->bit[i] = 12 + i; 
    }

    // 4. Configure DMA to feed the beast
    dma_channel_abort(dds_dma_chan);
    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false); // FIFO is at fixed address
    channel_config_set_dreq(&c, DREQ_HSTX);        // Pace transfer by HSTX FIFO
    __dmb(); // Ensure all config writes are complete before starting DMA   
    dma_channel_configure(dds_dma_chan, &c, 
                          (void *)&hstx_fifo_hw->fifo, // Destination: HSTX FIFO
                          buf,                         // Source: Your waveform buffer
                          len,                         // Transfer count
                          true);                       // Start immediately
}

void dds_init_rtt(uint gpio_pin) {
    SEGGER_RTT_Init();
    // Config Channel 1 for Binary (Index 1)
    SEGGER_RTT_ConfigUpBuffer(1, "DataOut", rtt_bin_up_buf, sizeof(rtt_bin_up_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(1, "DataIn", rtt_bin_down_buf, sizeof(rtt_bin_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(0, "Terminal", rtt_ch0_down_buf, sizeof(rtt_ch0_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    for (int i = 12; i < 20; i++) gpio_set_function(i, GPIO_FUNC_HSTX);

    gpio_set_function(gpio_pin, GPIO_FUNC_PWM);
    dds_slice = pwm_gpio_to_slice_num(gpio_pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(dds_slice, &cfg, true);

    if (dds_dma_chan < 0) dds_dma_chan = dma_claim_unused_channel(true);
}

void dds_init(uint gpio_pin) {
    // Initialize standard IO just in case
    stdio_init_all();

    // 1. Set Pins 12-19 to HSTX
    for (int i = 0; i < 8; i++) {
        gpio_set_function(HSTX_START_PIN + i, GPIO_FUNC_HSTX);
    }

    // 2. Claim DMA
    if (dds_dma_chan < 0) dds_dma_chan = dma_claim_unused_channel(true);

    // 3. AUTO-START (Bypassing RTT)
    //generate_standalone_sine();
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
                    data_pos = 0;
                    current_state = READ_DATA;
                } else {
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
                apply_dds_config(current_config.fstart, waveform_buffer, data_pos);
                SEGGER_RTT_printf(0, "DDS: Success %u bytes\n", data_pos);
                current_state = IDLE;
                header_pos = 0;
            }
            break;
    }
}


