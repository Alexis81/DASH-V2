#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t rpm;
    int32_t manifold_kpa;
    int16_t ect_c;
    int16_t iat_c;
    int16_t oil_temp_c;
    float ecu_volts;

    float tps_pct;
    float ignition_deg;
    uint8_t speed;
    uint8_t oil_pressure;
    uint8_t fuel_pressure;
    int16_t ecu_temp_c;

    float lambda1;
    float lambda2;
    float steering;
    float atmosphere_kpa;

    bool engine_valid;
    bool status_valid;
    bool lambda_valid;
} can_data_t;

esp_err_t can_start(void);
void can_get_data(can_data_t *out);
