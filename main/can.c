#include "can.h"
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "can";

#define CAN_ID_ENGINE 0x3E8
#define CAN_ID_STATUS 0x3E9
#define CAN_ID_LAMBDA 0x3EA
#define CAN_RX_QUEUE_LEN 32

typedef struct {
    uint32_t id;
    uint8_t data[8];
} can_frame_t;

#define CAN_ALIVE_US 1000000

static twai_node_handle_t s_node;
static QueueHandle_t s_rx_queue;
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static can_data_t s_data;
static int64_t s_last_rx_us;
static volatile bool s_bus_off;

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

/* Brut CAN uint8 − 50 °C, en arithmétique signée (brut 20 → −30, pas 226). */
static int16_t temp_c_from_raw(uint8_t raw)
{
    return (int16_t)((int)raw - 50);
}

static void decode_frame(const can_frame_t *frame)
{
    can_data_t next;

    taskENTER_CRITICAL(&s_data_lock);
    next = s_data;
    taskEXIT_CRITICAL(&s_data_lock);

    switch (frame->id) {
    case CAN_ID_ENGINE:
        next.rpm = read_be16(&frame->data[0]);
        next.manifold_kpa = (int32_t)read_be16(&frame->data[2]) - 100;
        next.ect_c = temp_c_from_raw(frame->data[4]);
        next.iat_c = temp_c_from_raw(frame->data[5]);
        next.ecu_volts = frame->data[6] * 0.1f;
        next.oil_temp_c = temp_c_from_raw(frame->data[7]);
        next.engine_valid = true;
        break;
    case CAN_ID_STATUS:
        next.tps_pct = read_be16(&frame->data[0]) * 0.1f;
        next.ignition_deg = (read_be16(&frame->data[2]) * 0.1f) - 100.0f;
        next.speed = frame->data[4];
        next.oil_pressure = frame->data[5];
        next.fuel_pressure = frame->data[6];
        next.ecu_temp_c = temp_c_from_raw(frame->data[7]);
        next.status_valid = true;
        break;
    case CAN_ID_LAMBDA:
        next.lambda1 = read_be16(&frame->data[0]) * 0.001f;
        next.lambda2 = read_be16(&frame->data[2]) * 0.001f;
        next.steering = ((int32_t)read_be16(&frame->data[4]) - 30000) / 10.0f;
        next.atmosphere_kpa = read_be16(&frame->data[6]) * 0.1f;
        next.lambda_valid = true;
        break;
    default:
        return;
    }

    taskENTER_CRITICAL(&s_data_lock);
    s_data = next;
    s_last_rx_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_data_lock);
}

static void can_task(void *arg)
{
    can_frame_t frame;
    TickType_t last_log = 0;

    while (1) {
        if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(200)) == pdTRUE) {
            decode_frame(&frame);
        }

        if (s_bus_off) {
            s_bus_off = false;
            const esp_err_t err = twai_node_recover(s_node);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reprise bus-off : %s", esp_err_to_name(err));
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(2000)) {
            can_data_t sample;
            can_get_data(&sample);
            if (sample.engine_valid || sample.status_valid || sample.lambda_valid) {
                ESP_LOGI(TAG, "rpm %u  kPa %ld  ect %d  tps %.1f  lambda %.3f",
                         sample.rpm, (long)sample.manifold_kpa, sample.ect_c,
                         sample.tps_pct, sample.lambda1);
            }
            last_log = now;
        }
    }
}

static bool can_on_rx(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    uint8_t data[8];
    twai_frame_t rx = {
        .buffer = data,
        .buffer_len = sizeof(data),
    };
    BaseType_t woken = pdFALSE;

    (void)edata;
    (void)user_ctx;

    if (twai_node_receive_from_isr(handle, &rx) != ESP_OK) {
        return false;
    }
    if (rx.header.rtr || rx.header.ide || twaifd_dlc2len(rx.header.dlc) != 8) {
        return false;
    }

    const can_frame_t frame = {
        .id = rx.header.id,
        .data = {data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]},
    };
    xQueueSendFromISR(s_rx_queue, &frame, &woken);
    return woken == pdTRUE;
}

static bool can_on_state(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    s_bus_off = edata->new_sta == TWAI_ERROR_BUS_OFF;
    return false;
}

void can_get_data(can_data_t *out)
{
    taskENTER_CRITICAL(&s_data_lock);
    *out = s_data;
    taskEXIT_CRITICAL(&s_data_lock);
}

bool can_is_alive(void)
{
    int64_t last;

    taskENTER_CRITICAL(&s_data_lock);
    last = s_last_rx_us;
    taskEXIT_CRITICAL(&s_data_lock);
    if (last == 0) {
        return false;
    }
    return (esp_timer_get_time() - last) < CAN_ALIVE_US;
}

esp_err_t can_start(void)
{
    s_rx_queue = xQueueCreate(CAN_RX_QUEUE_LEN, sizeof(can_frame_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = BOARD_PIN_CAN_TX,
            .rx = BOARD_PIN_CAN_RX,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = BOARD_CAN_BITRATE,
        .fail_retry_cnt = 0,
        .tx_queue_depth = 1,
    };

    esp_err_t err = twai_new_node_onchip(&node_config, &s_node);
    if (err != ESP_OK) {
        return err;
    }

    const twai_mask_filter_config_t filter = {
        .id = CAN_ID_ENGINE,
        .mask = 0x7FC,
        .is_ext = false,
        .no_fd = true,
    };
    err = twai_node_config_mask_filter(s_node, 0, &filter);
    if (err != ESP_OK) {
        return err;
    }

    const twai_event_callbacks_t callbacks = {
        .on_rx_done = can_on_rx,
        .on_state_change = can_on_state,
    };
    err = twai_node_register_event_callbacks(s_node, &callbacks, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(can_task, "can", BOARD_CAN_TASK_STACK, NULL, BOARD_CAN_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RX GPIO %d, TX GPIO %d, 1 Mbit/s", BOARD_PIN_CAN_RX, BOARD_PIN_CAN_TX);
    return ESP_OK;
}
