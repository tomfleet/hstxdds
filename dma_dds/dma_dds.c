#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "other/SEGGER_RTT/RTT/SEGGER_RTT.h"
#include <string.h>
#include "pico/time.h"
#include "board/pins.h"
#include "dma_dds/dma_dds.h"

// Memory Allocation
uint8_t waveform_buffer[MAX_WAVEFORM_SIZE] __attribute__((aligned(4)));

dds_config_t current_config = {
    .waveform_buffer = waveform_buffer,
    .buffer_len = 0,
    .fstart = 0
};

// Internal RTT Buffers for Channel 1
static uint8_t rtt_bin_up_buf[8192];   
static uint8_t rtt_bin_down_buf[8192]; 

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

// FIX: Added missing implementation to resolve linker error
void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    if (len == 0 || freq_hz == 0) return;
    
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / freq_hz;
    if (div < 1) div = 1;
    if (div > 4095) div = 4095;

    // Register-level HSTX configuration using your existing includes
    hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) | 
                       (8 << HSTX_CTRL_CSR_SHIFT_LSB) | 
                       HSTX_CTRL_CSR_EN_BITS;
    
    for (int i = 0; i < 8; i++) hstx_ctrl_hw->bit[i] = 0;

    dma_channel_abort(dds_dma_chan);
    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);

    dma_channel_configure(dds_dma_chan, &c, (void *)&hstx_fifo_hw->fifo, buf, len, true);
}

void dds_init(uint gpio_pin) {
    SEGGER_RTT_Init();
    // Config Channel 1 for Binary (Index 1)
    SEGGER_RTT_ConfigUpBuffer(1, "DataOut", rtt_bin_up_buf, sizeof(rtt_bin_up_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(1, "DataIn", rtt_bin_down_buf, sizeof(rtt_bin_down_buf), SEGGER_RTT_MODE_NO_BLOCK_SKIP);

    for (int i = 0; i < 8; i++) gpio_set_function(i, GPIO_FUNC_HSTX);

    gpio_set_function(gpio_pin, GPIO_FUNC_PWM);
    dds_slice = pwm_gpio_to_slice_num(gpio_pin);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(dds_slice, &cfg, true);

    if (dds_dma_chan < 0) dds_dma_chan = dma_claim_unused_channel(true);
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


void process_mailbox3() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Transfer Timeout: Reset to IDLE if no data for 500ms mid-transfer
    if (current_state != IDLE && (now - last_byte_time_ms > 500)) {
        current_state = IDLE;
        header_pos = 0;
        data_pos = 0;
        SEGGER_RTT_WriteString(0, "Mbox Timeout: Resetting\n");
    }

    switch (current_state) {
        case IDLE:
            if (SEGGER_RTT_HasData(0)) {
                uint8_t c;
                SEGGER_RTT_Read(0, &c, 1);
                if (c == 0xEF) { // Start of DEADBEEF (Little Endian)
                    header_work_buf[0] = 0xEF;
                    header_pos = 1;
                    last_byte_time_ms = now;
                    current_state = READ_HEADER;
                } else if (c == '1') { gpio_put(LED_PIN, 1); }
                  else if (c == '0') { gpio_put(LED_PIN, 0); }
                  else if (c == 'r') {
                      SEGGER_RTT_Write(1, waveform_buffer, current_config.buffer_len);
                  }
            }
            break;

        case READ_HEADER:
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
                uint32_t avail = SEGGER_RTT_GetBytesInBuffer(1);
                uint32_t remaining = incoming_header.len - data_pos;
                uint32_t to_read = (avail < remaining) ? avail : remaining;

                uint32_t r = SEGGER_RTT_Read(1, &waveform_buffer[data_pos], to_read);
                data_pos += r;
                last_byte_time_ms = now;
            }

            if (data_pos >= incoming_header.len) {
                current_config.buffer_len = data_pos;
                current_config.fstart = incoming_header.fstart;
                apply_dds_config(current_config.fstart, waveform_buffer, data_pos);
                SEGGER_RTT_printf(0, "DDS: Sync Complete (%u bytes)\n", data_pos);
                current_state = IDLE;
                header_pos = 0;
                data_pos = 0;
            }
            break;
    }
}