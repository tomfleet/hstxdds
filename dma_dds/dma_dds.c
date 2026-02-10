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

// Use the HSTX DREQ index for RP2350
#ifndef DREQ_HSTX
#define DREQ_HSTX 52
#endif


// // State Management
// dds_config_t current_config = {
//     .waveform_buffer = waveform_buffer,
//     .buffer_len = 0,
//     .fstart = 0,
//     .fend = 0,
//     .duration_ms = 0,
//     .mode = DDS_MODE_SINGLE,
//     .repeats = 0,
//     .delay_ms = 0
// };

// dds_config_t current_config = {
//     .waveform_buffer = waveform_buffer,
//     .buffer_len = 0,
//     .fstart = 0,
//     .fend = 0,
//     .duration_ms = 0
// };

// RTT Buffers
static uint8_t rtt_bin_up_buf[1024];   
static uint8_t rtt_bin_down_buf[16384 + 64]; // Enough for Wave + Header
static uint8_t rtt_ch0_down_buf[256];
static uint8_t debug_dma_buf[32];
static const uint32_t test_pattern_freq_hz = 2000000;
static const uint32_t test_pattern_samples_per_step =1;

static int dds_dma_chan = -1;
static volatile bool dma_busy = false;
static volatile uint32_t dma_irq_count = 0;
static volatile uint32_t last_dma_done_ms = 0;
static volatile bool dma_done_flag = false;
static bool dma_done_verbose = false;
static uint32_t repeats_remaining = 0;
static uint32_t next_trigger_time = 0;
static bool waiting_for_delay = false;
static uint dds_slice;


dds_config_t current_config = {
    .waveform_buffer = waveform_buffer,
    .buffer_len = 0,
    .fstart = 1000,
    .fend = 1000,
    .duration_ms = 0,
    .mode = DDS_MODE_SINGLE,
    .repeats = 0,
    .delay_ms = 0
};

// State Machine Variables
typedef enum { IDLE, READ_HEADER, READ_DATA } mbox_state_t;
static mbox_state_t current_state = IDLE;
static dds_header_t incoming_header;
#define DDS_HEADER_SIZE (sizeof(dds_header_t))
static uint8_t header_work_buf[DDS_HEADER_SIZE];
static uint32_t header_pos = 0;
static uint32_t data_pos = 0;
static uint32_t last_byte_time_ms = 0;


// --- HARDWARE CONFIGURATION ---
// Stock Pico 2 HSTX Pins are usually GP12 - GP19
#define HSTX_START_PIN 12 

static void log_dma_status(const char *tag) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t csr = hstx_ctrl_hw->csr;
    uint32_t ctrl = dma_hw->ch[dds_dma_chan].ctrl_trig;
    uint32_t count = dma_hw->ch[dds_dma_chan].transfer_count;
    uint32_t rd = (uint32_t)dma_hw->ch[dds_dma_chan].read_addr;
    uint32_t wr = (uint32_t)dma_hw->ch[dds_dma_chan].write_addr;
    SEGGER_RTT_printf(0, "%s t=%u irq=%u CSR=0x%08X DMA=0x%08X cnt=%u rd=0x%08X wr=0x%08X\n",
                      tag, now, dma_irq_count, csr, ctrl, count, rd, wr);
}

static void hstx_bus_enable(void) {
    for (int i = 12; i <= 19; i++) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
    }
    // RP2350 HSTX lane order appears to be nibble-swapped (4..7,0..3)
    for (int i = 0; i < 8; i++) {
#if DDS_HSTX_NIBBLE_SWAP
        hstx_ctrl_hw->bit[i] = 12 + ((i + 4) & 7);
#else
        hstx_ctrl_hw->bit[i] = 12 + i;
#endif
    }
}

static void hstx_bus_disable_and_low(void) {
    hstx_ctrl_hw->csr = 0;
    for (int i = 12; i <= 19; i++) {
        gpio_set_function(i, GPIO_FUNC_SIO);
        gpio_set_dir(i, GPIO_OUT);
        gpio_put(i, 0);
    }
}




// --- RESTORED: Standard apply_dds_config with 3 args ---
void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len) {
    if (dds_dma_chan < 0) return;
    
    dma_channel_abort(dds_dma_chan);

    // Program HSTX clocking for the requested rate
    if (freq_hz > 0) {
        uint32_t sys_clk = clock_get_hz(clk_sys);
        uint32_t div = sys_clk / freq_hz;
        if (div > 4095) div = 4095;
        if (div < 1) div = 1;
        hstx_bus_enable();
        hstx_ctrl_hw->csr = (div << HSTX_CTRL_CSR_CLKDIV_LSB) |
                           (8 << HSTX_CTRL_CSR_SHIFT_LSB) |
                           HSTX_CTRL_CSR_EN_BITS;
    }

    // Update global state for looping logic
    current_config.fstart = freq_hz;
    current_config.buffer_len = len;

    // Debug marker: idle high, 5 pulses, then low for transfer
    gpio_put(DEBUG_PULSE_PIN, 1);
    for (int i = 0; i < 5; i++) {
        gpio_put(DEBUG_PULSE_PIN, 0);
        busy_wait_us_32(10);
        gpio_put(DEBUG_PULSE_PIN, 1);
        busy_wait_us_32(10);
    }
    gpio_put(DEBUG_PULSE_PIN, 0);

    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX); // PACE: Prevents Bus Fault crash

    dma_channel_configure(
        dds_dma_chan, &c,
        &hstx_fifo_hw->fifo,
        buf,
        len,
        true // Start immediately
    );

    dma_busy = true;
}

// --- Main Task (Reconciled with your RTT state machine) ---
void dds_update335(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // 1. Handle Looping / Repeat Triggering
    if (waiting_for_delay && now >= next_trigger_time && !dma_busy) {
        bool should_trigger = false;
        if (current_config.mode == DDS_MODE_LOOP) {
            should_trigger = true;
        } else if (current_config.mode == DDS_MODE_REPEAT && repeats_remaining > 0) {
            repeats_remaining--;
            should_trigger = true;
        }

        if (should_trigger) {
            apply_dds_config(current_config.fstart, current_config.waveform_buffer, current_config.buffer_len);
            waiting_for_delay = false;
        } else {
            waiting_for_delay = false;
        }
    }

    // 2. Protocol Handling (FIXED: Removed SEGGER_RTT_Peek)
    unsigned avail = SEGGER_RTT_HasData(0);
    if (avail >= sizeof(dds_header_t)) {
        dds_header_t h;
        // Read header if available
        if (SEGGER_RTT_Read(0, &h, sizeof(h)) == sizeof(h)) {
            if (h.sync == 0xDEADBEEF) {
                current_config.fstart = h.fstart;
                current_config.buffer_len = h.len;
                current_config.mode = h.mode;
                current_config.repeats = h.repeats;
                current_config.delay_ms = h.delay_ms;
                repeats_remaining = h.repeats;
                // Wait for data on Channel 1 (your existing state machine handles this)
            }
        }
    } else if (avail > 0) {
        // Handle single-char commands like 's' or 'x'
        char c;
        SEGGER_RTT_Read(0, &c, 1);
        if (c == 's') {
            current_config.mode = DDS_MODE_SINGLE; // manual 's' resets to single
            apply_dds_config(current_config.fstart, current_config.waveform_buffer, current_config.buffer_len);
        } else if (c == 'x') {
            dma_channel_abort(dds_dma_chan);
            dma_busy = false;
            waiting_for_delay = false;
        }
    }
}
void dds_update_config(dds_config_t *config) {
    if (!config || config->buffer_len == 0) return;
    apply_dds_config(config->fstart, config->waveform_buffer, config->buffer_len);
}


void dds_update_config11111111111(dds_config_t *config) {
    if (dds_dma_chan < 0) return;
    
    dma_channel_abort(dds_dma_chan);

    dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX); 

    dma_channel_configure(
        dds_dma_chan,
        &c,
        &hstx_fifo_hw->fifo,
        config->waveform_buffer,
        config->buffer_len,
        true // Start
    );

    dma_busy = true;
}


// --- Main Update Loop ---
void dds_update(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // 1. Handle Looping
    if (waiting_for_delay && now >= next_trigger_time && !dma_busy) {
        bool trigger = false;
        if (current_config.mode == DDS_MODE_LOOP) trigger = true;
        else if (current_config.mode == DDS_MODE_REPEAT && repeats_remaining > 0) {
            repeats_remaining--;
            trigger = true;
        }

        if (trigger) {
            dds_update_config(&current_config);
            waiting_for_delay = false;
        } else {
            waiting_for_delay = false;
        }
    }

    // 2. Protocol Handling (Ch 0)
    unsigned data_avail = SEGGER_RTT_HasData(0);
    
    if (data_avail > 0) {
        // Safe Assumption: If we see >= 32 bytes, it's a Header. 
        // If we see 1 byte, it's a command.
        
        if (data_avail >= sizeof(dds_header_t)) {
            dds_header_t h;
            if (SEGGER_RTT_Read(0, &h, sizeof(h)) == sizeof(h)) {
                if (h.sync == 0xDEADBEEF) {
                    current_config.fstart = h.fstart;
                    current_config.fend = h.fend;
                    current_config.duration_ms = h.duration;
                    current_config.buffer_len = h.len;
                    current_config.mode = h.mode;
                    current_config.repeats = h.repeats;
                    current_config.delay_ms = h.delay_ms;
                    
                    repeats_remaining = h.repeats;
                    
                    SEGGER_RTT_printf(0, "RX Config: %u Hz, Mode %u\n", h.fstart, h.mode);
                }
            }
        } 
        else {
             // Likely a single char command
             char c = 0;
             SEGGER_RTT_Read(0, &c, 1);
             if (c == 's') {
                 SEGGER_RTT_WriteString(0, "CMD: Start\n");
                 current_config.mode = DDS_MODE_SINGLE; 
                 dds_update_config(&current_config);
             }
             else if (c == 'x') {
                 SEGGER_RTT_WriteString(0, "CMD: Stop\n");
                 dma_channel_abort(dds_dma_chan);
                 dma_busy = false;
                 waiting_for_delay = false;
                 current_config.mode = DDS_MODE_SINGLE;
             }
        }
    }

    // 3. Read Data on Channel 1
    if (SEGGER_RTT_HasData(1)) {
        static uint32_t data_pos = 0;
        unsigned num = SEGGER_RTT_Read(1, waveform_buffer + data_pos, 
                                     current_config.buffer_len - data_pos);
        data_pos += num;

        if (data_pos >= current_config.buffer_len && current_config.buffer_len > 0) {
            SEGGER_RTT_printf(0, "Data RX: %u bytes\n", data_pos);
            data_pos = 0;
        }
    }
}

// void dds_update4(void) {
//     uint32_t now = to_ms_since_boot(get_absolute_time());

//     // 1. Handle Looping
//     if (waiting_for_delay && now >= next_trigger_time && !dma_busy) {
//         bool trigger = false;
//         if (current_config.mode == DDS_MODE_LOOP) trigger = true;
//         else if (current_config.mode == DDS_MODE_REPEAT && repeats_remaining > 0) {
//             repeats_remaining--;
//             trigger = true;
//         }

//         if (trigger) {
//             //apply_dds_config(&current_config);
//             waiting_for_delay = false;
//         } else {
//             waiting_for_delay = false;
//         }
//     }

//     // 2. Protocol Handling (Single Unified Header)
//     if (SEGGER_RTT_HasData(0)) {
//         // Peek to see if it's a command 's'/'x' or a binary header
//         uint8_t peek_buf[4];
//         //unsigned peek_len = SEGGER_RTT_Peek(0, peek_buf, 4);

//         // if ((1)) >= 4 && *(uint32_t*)peek_buf == 0xDEADBEEF) {
//         //     // It's a Header
//         //      if (SEGGER_RTT_HasData(0) >= sizeof(dds_header_t)) {
//         //         dds_header_t h;
//         //         SEGGER_RTT_Read(0, &h, sizeof(h));
                
//         //         current_config.fstart = h.fstart;
//         //         current_config.fend = h.fend;
//         //         current_config.duration_ms = h.duration;
//         //         current_config.buffer_len = h.len;
//         //         current_config.mode = h.mode;
//         //         current_config.repeats = h.repeats;
//         //         current_config.delay_ms = h.delay_ms;
                
//         //         repeats_remaining = h.repeats;
                
//         //         SEGGER_RTT_printf(0, "RX Config: %u Hz, Mode %u\n", h.fstart, h.mode);
//         //     }
//         // } 
//         // else if (peek_len > 0) {
//         //      // It's a text command (legacy single char)
//         //      char c = 0;
//         //      SEGGER_RTT_Read(0, &c, 1);
//         //      if (c == 's') {
//         //          SEGGER_RTT_WriteString(0, "CMD: Start\n");
//         //          current_config.mode = DDS_MODE_SINGLE; 
//         //          //apply_dds_config(&current_config);
//         //      }
//         //      else if (c == 'x') {
//         //          SEGGER_RTT_WriteString(0, "CMD: Stop\n");
//         //          dma_channel_abort(dds_dma_chan);
//         //          dma_busy = false;
//         //          waiting_for_delay = false;
//         //          current_config.mode = DDS_MODE_SINGLE;
//         //      }
//         // }
//     }

//     // 3. Read Data on Channel 1
//     if (SEGGER_RTT_HasData(1)) {
//         static uint32_t data_pos = 0;
//         unsigned num = SEGGER_RTT_Read(1, waveform_buffer + data_pos, 
//                                      current_config.buffer_len - data_pos);
//         data_pos += num;

//         if (data_pos >= current_config.buffer_len && current_config.buffer_len > 0) {
//             SEGGER_RTT_printf(0, "Data RX: %u bytes\n", data_pos);
//             //apply_dds_config(&current_config);
//             data_pos = 0;
//         }
//     }
// }


// void dds_update22(void) {
//     uint32_t now = to_ms_since_boot(get_absolute_time());

//     // 1. Handle Looping / Repeats
//     if (waiting_for_delay && now >= next_trigger_time && !dma_busy) {
//         bool trigger = false;
        
//         if (current_config.mode == DDS_MODE_LOOP) {
//             trigger = true;
//         } 
//         else if (current_config.mode == DDS_MODE_REPEAT) {
//             if (repeats_remaining > 0) {
//                 repeats_remaining--;
//                 trigger = true;
//             }
//         }

//         if (trigger) {
//             //apply_dds_config(current_config.fend, current_config.waveform_buffer, current_config.buffer_len);
//             waiting_for_delay = false; // Reset flag, ISR will set it again when done
//         } else {
//             waiting_for_delay = false; // Done repeating
//         }
//     }

//     // 2. Handle Text Commands (Channel 0)
//     if (SEGGER_RTT_HasKey()) {
//         int key = SEGGER_RTT_GetKey();
//         if (key == 's') {
//             SEGGER_RTT_WriteString(0, "CMD: Start Single\n");
//             current_config.mode = DDS_MODE_SINGLE;
//            // apply_dds_config(current_config.fend, current_config.waveform_buffer, current_config.buffer_len);
//         }
//         else if (key == 'x') {
//             SEGGER_RTT_WriteString(0, "CMD: Stop\n");
//             dma_channel_abort(dds_dma_chan);
//             dma_busy = false;
//             waiting_for_delay = false;
//             current_config.mode = DDS_MODE_SINGLE;
//         }
//     }

//     // 3. Handle Binary Configuration (Channel 1)
//     if (SEGGER_RTT_HasData(1)) {
//         static dds_header_t header;
//         static uint32_t bytes_read = 0;
//         static bool reading_header = true;

//         if (reading_header) {
//              // Peek or accumulate header
//              unsigned num = SEGGER_RTT_Read(1, ((uint8_t*)&header) + bytes_read, sizeof(dds_header_t) - bytes_read);
//              bytes_read += num;
             
//              if (bytes_read >= sizeof(dds_header_t)) {
//                  if (header.sync == 0xDEADBEEF) {
//                      // Header valid, prep for data
//                      reading_header = false;
//                      bytes_read = 0; // Recycle counter for data
                     
//                      // Debug Print
//                      SEGGER_RTT_printf(0, "New Config: %u Hz, Mode=%u, Reps=%u, Dly=%u\n", 
//                          header.fstart, header.mode, header.repeats, header.delay_ms);
//                  } else {
//                      // Sync fail, flush
//                      SEGGER_RTT_Read(1, NULL, 1024); 
//                      bytes_read = 0;
//                  }
//              }
//         } else {
//             // Reading Data
//             unsigned num = SEGGER_RTT_Read(1, waveform_buffer + bytes_read, header.len - bytes_read);
//             bytes_read += num;

//             if (bytes_read >= header.len) {
//                 // Transfer Complete
//                 current_config.fstart = header.fstart;
//                 current_config.fend = header.fend;
//                 current_config.duration_ms = header.duration;
//                 current_config.buffer_len = header.len;
//                 current_config.mode = header.mode;
//                 current_config.repeats = header.repeats;
//                 current_config.delay_ms = header.delay_ms;

//                 // Reset state variables
//                 repeats_remaining = header.repeats;
//                 waiting_for_delay = false;
                
//                 // Auto-start
//             //apply_dds_config(current_config.fend, current_config.waveform_buffer, current_config.buffer_len);
                
//                 // Reset parser
//                 reading_header = true;
//                 bytes_read = 0;
//             }
//         }
//     }
// }


// --- Interrupt Handler ---
void __isr_dma_handler(void) {
    if (dma_hw->ints0 & (1u << dds_dma_chan)) {
        dma_hw->ints0 = 1u << dds_dma_chan; // Clear IRQ
        dma_busy = false;
        dma_irq_count++;
        last_dma_done_ms = to_ms_since_boot(get_absolute_time());
        dma_done_flag = true;
        gpio_put(DEBUG_PULSE_PIN, 1);

        if (current_config.mode == DDS_MODE_SINGLE) {
            hstx_bus_disable_and_low();
        }

        // Schedule next loop/repeat
        if (current_config.mode != DDS_MODE_SINGLE) {
            waiting_for_delay = true;
            next_trigger_time = to_ms_since_boot(get_absolute_time()) + current_config.delay_ms;
        }
    }
}

// void __isr_dma_handler(void) {
//     if (dma_hw->ints0 & (1u << dds_dma_chan)) {
//         dma_hw->ints0 = 1u << dds_dma_chan; // Clear interrupt
//         dma_busy = false;
//         //SEGGER_RTT_WriteString(0, "DMA complete\n");
//     }
// }

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

void generate_waveform_count(void) {
    // Cumulative bit pattern: 0x00, 0x01, 0x03, 0x07, ..., 0xFF
    int steps = 9;
    int len = steps * (int)test_pattern_samples_per_step;
    for (int i = 0; i < steps; i++) {
        uint8_t value = (uint8_t)(((1u << i) - 1u));
    #if DDS_TEST_PATTERN_INVERT
        value = (uint8_t)(~value);
    #endif
        int base = i * (int)test_pattern_samples_per_step;
        for (int j = 0; j < (int)test_pattern_samples_per_step; j++) {
            waveform_buffer[base + j] = value;
        }
    }
    current_config.buffer_len = len;
}

// Generate waveform AND start DMA transmission
void generate_standalone_sine(void) {
    // Only generate and start if no user waveform is present
    if (current_config.buffer_len == 0) {
        generate_waveform_sine();
        // Start at slow speed for testing (1 kHz sample rate, not 10 MHz)
        dds_update_config(&current_config);//, waveform_buffer, current_config.buffer_len);
    } else {
        // If user waveform is present, just start DMA with it
        dds_update_config(&current_config);
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
    // 4. Enable the HSTX Peripheral Block
    // (Must be done before writing to FIFO or CSR)
    if (hstx_ctrl_hw) {
         hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EN_BITS;
    }
    // 3. Configure GPIO pins 12-19 for HSTX
    // (Without this, signals don't leave the chip)
    hstx_bus_enable();

    // Debug pulse pin (idle high)
    gpio_init(DEBUG_PULSE_PIN);
    gpio_set_dir(DEBUG_PULSE_PIN, GPIO_OUT);
    gpio_disable_pulls(DEBUG_PULSE_PIN);
    gpio_set_drive_strength(DEBUG_PULSE_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_put(DEBUG_PULSE_PIN, 1);

    // --------------------------------------------------

    // --- PWM Setup (Preserved) ---
    // gpio_set_function(0, GPIO_FUNC_PWM);
    // uint slice_num = pwm_gpio_to_slice_num(0);
    // pwm_config config = pwm_get_default_config();
    // pwm_config_set_clkdiv(&config, 4.f);
    // pwm_init(slice_num, &config, true);
    // pwm_set_gpio_level(0, 0); 



    // --- DMA Setup (Preserved) ---
    dds_dma_chan = dma_claim_unused_channel(true);
    dma_channel_set_irq0_enabled(dds_dma_chan, true);

    SEGGER_RTT_printf(0, "Init: DMA channel %d claimed\n", dds_dma_chan);

    irq_set_exclusive_handler(DMA_IRQ_0, __isr_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    SEGGER_RTT_WriteString(0, "Init: COMPLETE - Ready for RTT commands\n");
    // Load default
    generate_waveform_sine(); // uses internal helper
    SEGGER_RTT_WriteString(0, "Init: default waveform loaded\n");


// // PWM Setup
//     gpio_set_function(3, GPIO_FUNC_PWM); 
//     uint slice_num = pwm_gpio_to_slice_num(3);
//     pwm_config config = pwm_get_default_config();
//     pwm_config_set_clkdiv(&config, 4.f);
//     pwm_init(slice_num, &config, true);
//     pwm_set_gpio_level(3, 0);
//     SEGGER_RTT_WriteString(0, "Init: - PWM Init\n");

    
 
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
    //SEGGER_RTT_Init();
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

// void dds_init(uint gpio_pin) {
//     // Initialize standard IO just in case
//     //stdio_init_all();

//     // 1. Set Pins 12-19 to HSTX
//     for (int i = 0; i < 8; i++) {
//         gpio_set_function(HSTX_START_PIN + i, GPIO_FUNC_HSTX);
//     }

//     // 2. Claim DMA
//     if (dds_dma_chan < 0) dds_dma_chan = dma_claim_unused_channel(true);

//     // 3. AUTO-START (Bypassing RTT)
//     generate_standalone_sine();
// }

void process_mailbox() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (dma_done_flag) {
        dma_done_flag = false;
        if (dma_done_verbose || current_config.mode == DDS_MODE_SINGLE) {
            SEGGER_RTT_printf(0, "DONE t=%u irq=%u\n", last_dma_done_ms, dma_irq_count);
        }
    }

    if (waiting_for_delay && now >= next_trigger_time && !dma_busy) {
        bool should_trigger = false;
        if (current_config.mode == DDS_MODE_LOOP) {
            should_trigger = true;
        } else if (current_config.mode == DDS_MODE_REPEAT && repeats_remaining > 0) {
            repeats_remaining--;
            should_trigger = true;
        }

        if (should_trigger) {
            dds_update_config(&current_config);
        } else if (current_config.mode == DDS_MODE_REPEAT) {
            hstx_bus_disable_and_low();
        }
        waiting_for_delay = false;
    }

    // Timeout: Reset if stuck mid-transfer for > 500ms
    if (current_state != IDLE && (now - last_byte_time_ms > 500)) {
        current_state = IDLE;
        header_pos = 0;
        data_pos = 0;
        SEGGER_RTT_WriteString(0, "Mbox: Timeout Reset\n");
    }

    if (current_state == IDLE && header_pos > 0 && (now - last_byte_time_ms > 500)) {
        header_pos = 0;
    }

    switch (current_state) {
        case IDLE:
            while (SEGGER_RTT_HasData(0)) {
                uint8_t c;
                SEGGER_RTT_Read(0, &c, 1);
                last_byte_time_ms = now;

                if (header_pos == 0) {
                    if (c == 'r') {
                        SEGGER_RTT_Write(1, waveform_buffer, current_config.buffer_len);
                        continue;
                    }
                    if (c == 's') {
                        if (!dma_busy) {
                            SEGGER_RTT_WriteString(0, "Starting waveform...\n");
                            log_dma_status("START");
                            current_config.mode = DDS_MODE_SINGLE;
                            current_config.repeats = 0;
                            current_config.delay_ms = 0;
                            repeats_remaining = 0;
                            waiting_for_delay = false;
                            generate_standalone_sine();
                            SEGGER_RTT_WriteString(0, "Waveform started\n");
                        } else {
                            SEGGER_RTT_WriteString(0, "DMA busy, cannot start new transfer\n");
                        }
                        continue;
                    }
                    if (c == 't') {
                        if (!dma_busy) {
                            SEGGER_RTT_WriteString(0, "Starting test pattern...\n");
                            dma_irq_count = 0;
                            dma_done_flag = false;
                            current_config.mode = DDS_MODE_SINGLE;
                            current_config.fstart = test_pattern_freq_hz;
                            current_config.repeats = 0;
                            current_config.delay_ms = 0;
                            repeats_remaining = 0;
                            waiting_for_delay = false;
                            generate_waveform_count();
                            log_dma_status("TSTART");
                            dds_update_config(&current_config);
                            SEGGER_RTT_WriteString(0, "Test pattern started\n");
                        } else {
                            SEGGER_RTT_WriteString(0, "DMA busy, cannot start test pattern\n");
                        }
                        continue;
                    }
                    if (c == 'T') {
                        if (!dma_busy) {
                            SEGGER_RTT_WriteString(0, "Sequence: test pattern one-shot\n");
                            dma_irq_count = 0;
                            dma_done_flag = false;
                            current_config.mode = DDS_MODE_SINGLE;
                            current_config.fstart = test_pattern_freq_hz;
                            current_config.repeats = 0;
                            current_config.delay_ms = 0;
                            repeats_remaining = 0;
                            waiting_for_delay = false;
                            generate_waveform_count();
                            log_dma_status("TSEQ_START");
                            dds_update_config(&current_config);
                        } else {
                            SEGGER_RTT_WriteString(0, "DMA busy, cannot run sequence\n");
                        }
                        continue;
                    }
                    if (c == 'p') {
                        uint32_t csr = hstx_ctrl_hw->csr;
                        SEGGER_RTT_printf(0, "HSTX CSR=0x%08X\n", csr);
                        for (int i = 0; i < 64; i++) {
                            hstx_fifo_hw->fifo = (uint8_t)(0xAA ^ i);
                        }
                        SEGGER_RTT_WriteString(0, "Poke: FIFO pattern written\n");
                        continue;
                    }
                    if (c == 'd') {
                        uint32_t csr = hstx_ctrl_hw->csr;
                        uint32_t ctrl = dma_hw->ch[dds_dma_chan].ctrl_trig;
                        uint32_t count = dma_hw->ch[dds_dma_chan].transfer_count;
                        uint32_t rd = (uint32_t)dma_hw->ch[dds_dma_chan].read_addr;
                        uint32_t wr = (uint32_t)dma_hw->ch[dds_dma_chan].write_addr;
                        SEGGER_RTT_printf(0, "HSTX CSR=0x%08X\n", csr);
                        SEGGER_RTT_printf(0, "DMA: ctrl=0x%08X cnt=%u rd=0x%08X wr=0x%08X\n",
                                          ctrl, count, rd, wr);
                        continue;
                    }
                    if (c == 'u') {
                        for (int i = 0; i < (int)sizeof(debug_dma_buf); i++) {
                            debug_dma_buf[i] = (uint8_t)(0x55 ^ i);
                        }
                        dma_channel_abort(dds_dma_chan);
                        dma_channel_config c = dma_channel_get_default_config(dds_dma_chan);
                        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
                        channel_config_set_read_increment(&c, true);
                        channel_config_set_write_increment(&c, false);
                        dma_channel_configure(
                            dds_dma_chan,
                            &c,
                            &hstx_fifo_hw->fifo,
                            debug_dma_buf,
                            sizeof(debug_dma_buf),
                            true
                        );
                        SEGGER_RTT_WriteString(0, "Unpaced DMA burst started\n");
                        continue;
                    }
                    if (c == 'g') {
                        SEGGER_RTT_WriteString(0, "Trigger pulse 10ms\n");
                        gpio_put(DEBUG_PULSE_PIN, 1);
                        busy_wait_ms(10);
                        gpio_put(DEBUG_PULSE_PIN, 0);
                        continue;
                    }
                    if (c == 'h') {
                        hstx_ctrl_hw->csr = 0;
                        SEGGER_RTT_WriteString(0, "HSTX disabled\n");
                        continue;
                    }
                    if (c == 'H') {
                        hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EN_BITS;
                        SEGGER_RTT_WriteString(0, "HSTX enabled\n");
                        continue;
                    }
                    if (c == 'v') {
                        dma_done_verbose = !dma_done_verbose;
                        SEGGER_RTT_printf(0, "DONE verbose %s\n", dma_done_verbose ? "on" : "off");
                        continue;
                    }
                    if (c == 'x' || c == 'X') {
                        SEGGER_RTT_WriteString(0, "Stopping DMA...\n");
                        dma_channel_abort(dds_dma_chan);
                        dma_busy = false;
                        waiting_for_delay = false;
                        current_config.mode = DDS_MODE_SINGLE;
                        SEGGER_RTT_WriteString(0, "DMA stopped\n");
                        continue;
                    }
                    if (c == '?') {
                        SEGGER_RTT_WriteString(0, "Commands: s=start, t=test, T=seq, x=stop, r=read, p=poke, d=dma, u=unpaced, g=pulse, h=disable, H=enable, v=verbose, ?=help\n");
                        continue;
                    }
                }

                header_work_buf[header_pos++] = c;
                if (header_pos >= sizeof(uint32_t)) {
                    uint32_t sync = 0;
                    memcpy(&sync, header_work_buf, sizeof(sync));
                    if (sync == 0xDEADBEEF) {
                        current_state = READ_HEADER;
                        break;
                    }
                    memmove(header_work_buf, header_work_buf + 1, sizeof(uint32_t) - 1);
                    header_pos = sizeof(uint32_t) - 1;
                }
            }
            break;

        case READ_HEADER:
            // Pull bytes one-by-one to handle trickling J-Link data
            while (SEGGER_RTT_HasData(0) && header_pos < DDS_HEADER_SIZE) {
                SEGGER_RTT_Read(0, &header_work_buf[header_pos++], 1);
                last_byte_time_ms = now;
            }

            if (header_pos == DDS_HEADER_SIZE) {
                memcpy(&incoming_header, header_work_buf, DDS_HEADER_SIZE);
                if (incoming_header.sync == 0xDEADBEEF && incoming_header.len <= MAX_WAVEFORM_SIZE) {
                    SEGGER_RTT_printf(0, "Header OK: %u->%u Hz, %u ms, len=%u\n", 
                                    incoming_header.fstart, incoming_header.fend, 
                                    incoming_header.duration, incoming_header.len);
                    current_config.fstart = incoming_header.fstart;
                    current_config.fend = incoming_header.fend;
                    current_config.duration_ms = incoming_header.duration;
                    current_config.buffer_len = incoming_header.len;
                    current_config.mode = incoming_header.mode;
                    current_config.repeats = incoming_header.repeats;
                    current_config.delay_ms = incoming_header.delay_ms;
                    repeats_remaining = incoming_header.repeats;
                    waiting_for_delay = false;
                    data_pos = 0;
                    current_state = READ_DATA;
                } else {
                    SEGGER_RTT_printf(0, "Bad header: sync=0x%08X len=%u\n", incoming_header.sync, incoming_header.len);
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
                log_dma_status("RXDONE");
                dds_update_config(&current_config);

                SEGGER_RTT_printf(0, "DDS: Success %u bytes @ %u Hz\n", data_pos, current_config.fstart);
                current_state = IDLE;
                header_pos = 0;
                data_pos = 0;
            }
            break;
    }
}


