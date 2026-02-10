#ifndef DMA_DDS_H
#define DMA_DDS_H

#include <stdint.h>
#include "pico/stdlib.h"

//#define MAX_WAVEFORM_SIZE 8192

#define MAX_WAVEFORM_SIZE 16384
// Playback Modes
#define DDS_MODE_SINGLE    0
#define DDS_MODE_LOOP      1
#define DDS_MODE_REPEAT    2

// Optional debug toggles
#ifndef DDS_TEST_PATTERN_INVERT
#define DDS_TEST_PATTERN_INVERT 1
#endif

#ifndef DDS_HSTX_NIBBLE_SWAP
#define DDS_HSTX_NIBBLE_SWAP 0
#endif

// HSTX pad tuning (GPIO pad settings)
#ifndef DDS_HSTX_DRIVE_STRENGTH
#define DDS_HSTX_DRIVE_STRENGTH GPIO_DRIVE_STRENGTH_2MA
#endif

#ifndef DDS_HSTX_SLEW_RATE
#define DDS_HSTX_SLEW_RATE GPIO_SLEW_RATE_SLOW
#endif

// 32-byte header matching Python struct.pack
typedef struct __attribute__((packed)) {
    uint32_t sync;       // 0xDEADBEEF
    uint32_t fstart;
    uint32_t fend;
    uint32_t duration;   // Duration of calculation (ms)
    uint32_t len;        // Byte length of data
    uint32_t mode;       // 0=Single, 1=Loop, 2=Repeat
    uint32_t repeats;    // Number of repeats (if mode=2)
    uint32_t delay_ms;   // Delay between triggers (ms)
} dds_header_t;
// // 20-byte header matching Python: struct.pack("<IIIII", 0xDEADBEEF, fstart, fend, duration, len)
// typedef struct __attribute__((packed)) {
//     uint32_t sync;      // 0xDEADBEEF
//     uint32_t fstart;
//     uint32_t fend;
//     uint32_t duration;
//     uint32_t len;
// } dds_header_t;

typedef struct {
    uint32_t fstart;
    uint32_t fend;
    uint32_t duration_ms;
    uint32_t buffer_len;
    uint8_t *waveform_buffer;
    
    // New Fields
    uint32_t mode;
    uint32_t repeats;
    uint32_t delay_ms;
} dds_config_t;

// typedef struct {
//     uint32_t fstart;
//     uint32_t fend;
//     uint32_t duration_ms;
//     uint32_t buffer_len;
//     uint8_t *waveform_buffer;
// } dds_config_t;

// Expose globals for external visibility
extern dds_config_t current_config;
extern uint8_t waveform_buffer[MAX_WAVEFORM_SIZE];

// Function Prototypes
void dds_update(void);
void dds_init(uint gpio_pin);
void dds_init_rtt(uint gpio_pin);
void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len);
void dds_update_config(dds_config_t *config);
void process_mailbox(void);

#endif