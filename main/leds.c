#include "leds.h"
#include "board.h"

#include <string.h>
#include "esp_log.h"
#include "led_strip.h"

#define LED_BRIGHTNESS_DEF 25
#define LED_TICK_MS BOARD_UI_PERIOD_MS
#define LED_BLINK_MAX_MS 80
#define LED_BLINK_IDLE_MS 350
#define LED_BLINK_TIMEOUT_MS 500
#define COLOR_RED 0xFF0000
#define COLOR_GREEN 0x00FF00
#define COLOR_BLUE 0x0000FF
#define COLOR_ORANGE 0xFFA500
#define COLOR_BLACK 0x000000

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_px_t;

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_TIMEOUT,
    LED_MODE_IDLE,
    LED_MODE_BAR,
    LED_MODE_BLINK,
    LED_MODE_PREVIEW,
} led_mode_t;

static const char *TAG = "leds";
static led_strip_handle_t s_strip;
static bool s_enabled;
static bool s_preview;
static uint8_t s_brightness = LED_BRIGHTNESS_DEF;
static led_mode_t s_mode = LED_MODE_OFF;
static uint32_t s_elapsed;
static bool s_phase = true;
static led_px_t s_frame[BOARD_LED_COUNT];
static uint16_t s_last_rpm;
static bool s_last_can_alive;
static uint16_t s_last_green_end;
static uint16_t s_last_yellow_end;
static uint16_t s_last_red_end;
static bool s_have_last_update;

static void put(led_px_t *px, uint32_t color)
{
    px->r = (uint8_t)(((color >> 16) & 0xFF) * s_brightness / 255);
    px->g = (uint8_t)(((color >> 8) & 0xFF) * s_brightness / 255);
    px->b = (uint8_t)((color & 0xFF) * s_brightness / 255);
}

static void push(const led_px_t *px)
{
    int i;

    if (s_strip == NULL || memcmp(px, s_frame, sizeof(s_frame)) == 0) {
        return;
    }
    memcpy(s_frame, px, sizeof(s_frame));
    for (i = 0; i < BOARD_LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, px[i].r, px[i].g, px[i].b);
    }
    led_strip_refresh(s_strip);
}

static void blank(void)
{
    led_px_t px[BOARD_LED_COUNT];

    memset(px, 0, sizeof(px));
    if (s_strip != NULL) {
        led_strip_clear(s_strip);
    }
    memcpy(s_frame, px, sizeof(s_frame));
    s_mode = LED_MODE_OFF;
    s_elapsed = 0;
    s_phase = true;
}

static void fill_solid(led_px_t *px, uint32_t color)
{
    int i;

    for (i = 0; i < BOARD_LED_COUNT; i++) {
        put(&px[i], color);
    }
}

static void fill_alt(led_px_t *px, uint32_t color, bool even_on)
{
    int i;

    for (i = 0; i < BOARD_LED_COUNT; i++) {
        const bool on = ((i % 2) == 0) == even_on;
        put(&px[i], on ? color : COLOR_BLACK);
    }
}

/* 4 vertes jusqu'à fin vert, 6 rouges jusqu'à fin jaune, 6 bleues jusqu'au régime max. */
static void fill_bar(led_px_t *px, uint16_t rpm, uint16_t green_end, uint16_t yellow_end, uint16_t red_end)
{
    int remaining = rpm;
    int step;
    int i;

    for (i = 0; i < BOARD_LED_COUNT; i++) {
        put(&px[i], COLOR_BLACK);
    }

    step = (int)green_end / 4;
    for (i = 0; i < 4 && remaining > 0; i++) {
        put(&px[i], COLOR_GREEN);
        remaining -= (step > 0) ? step : remaining;
    }

    step = (yellow_end > green_end) ? (int)(yellow_end - green_end) / 6 : 0;
    for (i = 4; i < 10 && remaining > 0; i++) {
        put(&px[i], COLOR_RED);
        remaining -= (step > 0) ? step : remaining;
    }

    step = (red_end > yellow_end) ? (int)(red_end - yellow_end) / 6 : 0;
    for (i = 10; i < BOARD_LED_COUNT && remaining > 0; i++) {
        put(&px[i], COLOR_BLUE);
        remaining -= (step > 0) ? step : remaining;
    }
}

static void phase_tick(uint32_t period_ms)
{
    s_elapsed += LED_TICK_MS;
    if (s_elapsed >= period_ms) {
        s_elapsed = 0;
        s_phase = !s_phase;
    }
}

void leds_update(uint16_t rpm, bool can_alive, uint16_t green_end, uint16_t yellow_end, uint16_t red_end);

esp_err_t leds_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_PIN_LED_STRIP,
        .max_leds = BOARD_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    const esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "barre LED : %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }
    blank();
    return ESP_OK;
}

void leds_set_enabled(bool enabled)
{
    led_px_t px[BOARD_LED_COUNT];

    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    if (s_strip == NULL || s_preview) {
        return;
    }
    if (!enabled) {
        blank();
        return;
    }
    fill_solid(px, COLOR_BLUE);
    s_mode = LED_MODE_OFF;
    push(px);
}

static void show_preview(void)
{
    led_px_t px[BOARD_LED_COUNT];
    int i;

    if (s_strip == NULL) {
        return;
    }
    /* Barre complète : 4 vertes, 6 rouges, 6 bleues (pas de niveau régime). */
    for (i = 0; i < 4; i++) {
        put(&px[i], COLOR_GREEN);
    }
    for (i = 4; i < 10; i++) {
        put(&px[i], COLOR_RED);
    }
    for (i = 10; i < BOARD_LED_COUNT; i++) {
        put(&px[i], COLOR_BLUE);
    }
    s_mode = LED_MODE_PREVIEW;
    push(px);
}

void leds_set_preview(bool preview)
{
    if (preview == s_preview) {
        if (preview) {
            show_preview();
        }
        return;
    }
    s_preview = preview;
    if (s_strip == NULL) {
        return;
    }
    if (preview) {
        show_preview();
        return;
    }
    memset(s_frame, 0xFF, sizeof(s_frame));
    s_mode = LED_MODE_OFF;
    s_elapsed = 0;
    s_phase = true;
    if (!s_enabled) {
        blank();
        return;
    }
    if (s_have_last_update) {
        leds_update(s_last_rpm, s_last_can_alive, s_last_green_end, s_last_yellow_end, s_last_red_end);
    }
}

void leds_set_brightness(uint8_t brightness)
{
    if (brightness < 1) {
        brightness = 1;
    }
    if (brightness == s_brightness) {
        return;
    }
    s_brightness = brightness;
    /* Invalide le cache pour forcer un refresh avec le nouvel échelon. */
    memset(s_frame, 0xFF, sizeof(s_frame));
    if (s_preview) {
        show_preview();
        return;
    }
    if (s_enabled && s_have_last_update) {
        leds_update(s_last_rpm, s_last_can_alive, s_last_green_end, s_last_yellow_end, s_last_red_end);
    }
}

void leds_update(uint16_t rpm, bool can_alive, uint16_t green_end, uint16_t yellow_end, uint16_t red_end)
{
    led_mode_t mode;
    led_px_t px[BOARD_LED_COUNT];
    uint32_t period = 0;

    s_last_rpm = rpm;
    s_last_can_alive = can_alive;
    s_last_green_end = green_end;
    s_last_yellow_end = yellow_end;
    s_last_red_end = red_end;
    s_have_last_update = true;

    if (s_strip == NULL) {
        return;
    }
    if (s_preview) {
        show_preview();
        return;
    }
    if (!s_enabled) {
        return;
    }

    if (!can_alive) {
        mode = LED_MODE_TIMEOUT;
        period = LED_BLINK_TIMEOUT_MS;
    } else if (rpm == 0) {
        mode = LED_MODE_IDLE;
        period = LED_BLINK_IDLE_MS;
    } else if (rpm >= red_end) {
        mode = LED_MODE_BLINK;
        period = LED_BLINK_MAX_MS;
    } else {
        mode = LED_MODE_BAR;
    }

    if (mode != s_mode) {
        s_mode = mode;
        s_elapsed = 0;
        s_phase = true;
    } else if (mode != LED_MODE_BAR) {
        phase_tick(period);
    }

    if (mode == LED_MODE_TIMEOUT) {
        fill_alt(px, COLOR_ORANGE, s_phase);
    } else if (mode == LED_MODE_IDLE) {
        fill_alt(px, COLOR_BLUE, s_phase);
    } else if (mode == LED_MODE_BLINK) {
        fill_solid(px, s_phase ? COLOR_RED : COLOR_BLACK);
    } else {
        fill_bar(px, rpm, green_end, yellow_end, red_end);
    }
    push(px);
}
