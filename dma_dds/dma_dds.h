#ifndef DMA_DDS_H
#define DMA_DDS_H

#include <stdint.h>
#include "pico/stdlib.h"

#define MAX_WAVEFORM_SIZE 8192

// 20-byte header matching Python: struct.pack("<IIIII", 0xDEADBEEF, fstart, fend, duration, len)
typedef struct __attribute__((packed)) {
    uint32_t sync;      // 0xDEADBEEF
    uint32_t fstart;
    uint32_t fend;
    uint32_t duration;
    uint32_t len;
} dds_header_t;

typedef struct {
    uint32_t fstart;
    uint32_t fend;
    uint32_t duration_ms;
    uint32_t buffer_len;
    uint8_t *waveform_buffer;
} dds_config_t;

// Expose globals for external visibility
extern dds_config_t current_config;
extern uint8_t waveform_buffer[MAX_WAVEFORM_SIZE];

// Function Prototypes
void dds_init(uint gpio_pin);
void dds_init_rtt(uint gpio_pin);
void apply_dds_config(uint32_t freq_hz, uint8_t *buf, uint32_t len);
void dds_update_config(dds_config_t *config);
void process_mailbox(void);

#endif