#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_SCANNING,
    LED_STATE_CONNECTED,
} leds_state_t;

esp_err_t leds_init(void);
void leds_set_state(leds_state_t state);
void leds_notify_activity(void);
bool leds_is_available(void);

#ifdef __cplusplus
}
#endif
