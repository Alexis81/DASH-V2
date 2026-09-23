#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t leds_init(void);
void leds_set_enabled(bool enabled);

/* Appelé toutes les 40 ms. Les seuils sont ceux des réglages (fin vert, fin jaune, régime max). */
void leds_update(uint16_t rpm, bool can_alive, uint16_t green_end, uint16_t yellow_end, uint16_t red_end);
