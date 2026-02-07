#ifndef DDS_ENGINE_H
#define DDS_ENGINE_H

#include "pico/stdlib.h"

// The same struct your Python 'config.py' sends
typedef struct {
    uint32_t fstart;
    uint32_t fend;
    uint16_t ramp_ms;
    uint8_t  mode;
} dds_config_t;

// Public API
void dds_init(uint gpio_pin);
void dds_update_config(dds_config_t *new_cfg);
void dds_load_wavetable(const int16_t *data, uint16_t length);

#endif