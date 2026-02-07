#include "dma_dds.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>

// 1. Memory Allocation
uint8_t waveform_buffer[MAX_WAVEFORM_SIZE] __attribute__((aligned(4)));

dds_config_t current_config = {
    .waveform_buffer = waveform_buffer,
    .buffer_len = 0,
    .fstart = 0
};

// Internal RTT Buffers for Channel 1 (Binary Data)
static uint8_t rtt_bin_up_buf[8192];   
static uint8_t rtt_bin_down_buf[8192]; 

static int dds_dma_chan = -1;
static uint dds_slice;

// State machine variables
typedef enum { IDLE, READ_HEADER, READ_DATA } mbox_state_t;
static mbox_state_t current_state = IDLE;
static dds_header_t incoming_header;
static uint8_t header_work_buf[20];

void dds_init(uint gpio_pin) {
    SEGGER_RTT_Init();
    
    // Config Channel 1 for Binary (Index 1)
    SEGGER_RTT_ConfigUpBuffer(1, "DataOut", rtt_bin_up_buf, sizeof(rtt_bin_up_buf), SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);
    SEGGER_RTT_ConfigDownBuffer(1, "DataIn", rtt_bin_down_buf, sizeof(rtt_bin_down_buf), SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);

    // Parallel HSTX Pin Setup
    for (int i = 0; i < 8; i++) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }

    // PWM Reference Setup
    gpio_set_function(gpio_pin, GPIO_FUNC_PWM);
    dds_slice = pwm_gpio_to_slice_num(gpio_pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(dds_slice, &cfg, true);

    // Initial DMA claim
    if (dds_dma_chan < 0) {
        dds_dma_chan = dma_claim_unused_channel(true);
    }
}

void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    if (len == 0 || freq_hz == 0) return;

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div > 4095) div = 4095;
    if (div < 1) div = 1;

    // RP2350 HSTX CSR configuration
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) |
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) |
                       HSTX_CTRL_CSR_EN_BITS;

    // Connect shifter slots to pins 0-7
    for (int i = 0; i < 8; i++) {
        hstx_ctrl_hw->bit[i] = 0;
    }

    dma_channel_abort(dds_dma_chan);
    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);

    dma_channel_configure(
        dds_dma_chan, &c,
        (void *)&hstx_fifo_hw->fifo,
        buf, len, true
    );
}

void dds_update_config(dds_config_t *config) {
    if (!config || config->buffer_len == 0) return;
    apply_dds_config(config->fstart, config->waveform_buffer, config->buffer_len);
    SEGGER_RTT_WriteString(0, "DDS: Hardware Updated.\n");
}

void process_mailbox() {
    switch (current_state) {
        case IDLE:
            if (SEGGER_RTT_HasData(0)) {
                uint8_t c;
                SEGGER_RTT_Read(0, &c, 1);
                if (c == 0xEF) { // Start of 0xDEADBEEF
                    header_work_buf[0] = 0xEF;
                    current_state = READ_HEADER;
                } else if (c == '1') { gpio_put(25, 1); }
                  else if (c == '0') { gpio_put(25, 0); }
                  else if (c == 'r') {
                      SEGGER_RTT_Write(1, waveform_buffer, current_config.buffer_len);
                  }
            }
            break;

        case READ_HEADER:
            if (SEGGER_RTT_GetBytesInBuffer(0) >= 19) {
                SEGGER_RTT_Read(0, &header_work_buf[1], 19);
                memcpy(&incoming_header, header_work_buf, 20);
                if (incoming_header.sync == 0xDEADBEEF) {
                    current_state = READ_DATA;
                } else {
                    current_state = IDLE;
                }
            }
            break;

        case READ_DATA:
            if (SEGGER_RTT_GetBytesInBuffer(1) >= incoming_header.len) {
                uint32_t r = SEGGER_RTT_Read(1, waveform_buffer, incoming_header.len);
                current_config.buffer_len = r;
                current_config.fstart = incoming_header.fstart;
                dds_update_config(&current_config);
                SEGGER_RTT_printf(0, "DDS: Loaded %u bytes\n", r);
                current_state = IDLE;
            }
            break;
    }
}