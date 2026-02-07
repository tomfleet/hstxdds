
#include "dma_dds.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"

static int dma_chan;
static uint dds_slice;

void dds_init(uint gpio_pin) {
    gpio_set_function(gpio_pin, GPIO_FUNC_PWM);
    dds_slice = pwm_gpio_to_slice_num(gpio_pin);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255); // 8-bit resolution
    pwm_init(dds_slice, &cfg, true);

    dma_chan = dma_claim_unused_channel(true);
    // ... insert the DMA configuration code from our previous turn here ...
}

void dds_update_config(dds_config_t *new_cfg) {
    // Math to translate fstart/fend into PWM frequency or DMA pacing
    // This is where you'll handle the "Sweep" logic
}