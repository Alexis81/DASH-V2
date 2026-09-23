#include "board.h"
#include "can.h"
#include "toyota_logo.h"

#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

extern const lv_font_t rpm_font;
extern const lv_font_t stop_font;

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_touch_handle_t s_touch;

#define RPM_GREEN_DEF 4500
#define RPM_YELLOW_DEF 6000
#define RPM_MAX_DEF 7500
#define RPM_MAX_CAP 8000
#define ECT_COLD_C 60
#define ECT_HOT_C 95
#define ECT_STOP_DEF 110
#define LAMBDA_RICH 0.95f
#define LAMBDA_LEAN 1.2f
#define STOP_BLINK_MS 500
#define BOTTOM_BAND_H 120
#define TITLE_BAND_Y ((BOARD_LCD_V_RES / 2) + 122)
#define TITLE_BAND_H 39
#define SEP_LINE_COUNT 5
#define SET_ROW_COUNT 4

typedef struct {
    uint16_t *val;
    uint16_t lo;
    uint16_t hi;
    uint16_t step;
    lv_obj_t *lbl;
} set_row_t;

static lv_obj_t *s_rpm_green;
static lv_obj_t *s_rpm_yellow;
static lv_obj_t *s_rpm_red;
static lv_obj_t *s_rpm_label;
static lv_obj_t *s_bottom_band;
static lv_obj_t *s_title_band;
static lv_obj_t *s_sep_lines[SEP_LINE_COUNT];
static lv_obj_t *s_lambda_title;
static lv_obj_t *s_lambda_value;
static lv_obj_t *s_air_title;
static lv_obj_t *s_air_value;
static lv_obj_t *s_tps_title;
static lv_obj_t *s_tps_value;
static lv_obj_t *s_ect_title;
static lv_obj_t *s_ect_value;
static lv_obj_t *s_stop_overlay;
static lv_obj_t *s_stop_text;
static lv_obj_t *s_stop_label;
static lv_obj_t *s_water_label;
static lv_obj_t *s_settings;
static bool s_stop_active;
static bool s_stop_lit = true;
static bool s_settings_open;
static uint32_t s_stop_elapsed;
static uint16_t s_rpm_drawn;
static int16_t s_ect_drawn = INT16_MIN;
static int16_t s_iat_drawn = INT16_MIN;
static float s_lambda_drawn = -1.0f;
static float s_tps_drawn = -1.0f;
static bool s_lambda_valid_drawn;
static bool s_engine_valid_drawn;
static int32_t s_rpm_green_px;
static int32_t s_rpm_yellow_px;
static int32_t s_rpm_red_px;
static uint16_t s_rpm_shown;
static bool s_dash_ready;
static uint16_t s_rpm_end_g = RPM_GREEN_DEF;
static uint16_t s_rpm_end_y = RPM_YELLOW_DEF;
static uint16_t s_rpm_end_r = RPM_MAX_DEF;
static uint16_t s_ect_stop = ECT_STOP_DEF;
static set_row_t s_set_rows[SET_ROW_COUNT];

static int32_t rpm_span_px(uint16_t rpm, uint16_t rpm_start, uint16_t rpm_end, int32_t width)
{
    if (rpm <= rpm_start || width <= 0 || rpm_end <= rpm_start) {
        return 0;
    }
    if (rpm >= rpm_end) {
        return width;
    }
    return (int32_t)(((uint32_t)(rpm - rpm_start) * (uint32_t)width) / (uint32_t)(rpm_end - rpm_start));
}

static lv_obj_t *rpm_band_create(lv_obj_t *parent, lv_color_t color, int32_t x)
{
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_set_pos(band, x, 0);
    lv_obj_set_size(band, 0, BOARD_LCD_V_RES - BOTTOM_BAND_H);
    lv_obj_set_style_bg_color(band, color, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_radius(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_set_style_min_width(band, 0, 0);
    lv_obj_set_scrollable(band, false);
    lv_obj_set_clickable(band, false);
    return band;
}

static void rpm_curtain_apply(uint16_t rpm)
{
    if (rpm > s_rpm_end_r) {
        rpm = s_rpm_end_r;
    }

    const int32_t green = rpm_span_px(rpm, 0, s_rpm_end_g, s_rpm_green_px);
    const int32_t yellow = rpm_span_px(rpm, s_rpm_end_g, s_rpm_end_y, s_rpm_yellow_px);
    const int32_t red = rpm_span_px(rpm, s_rpm_end_y, s_rpm_end_r, s_rpm_red_px);

    lv_obj_set_width(s_rpm_green, green);
    lv_obj_set_width(s_rpm_yellow, yellow);
    lv_obj_set_width(s_rpm_red, red);

    if (s_rpm_label != NULL && rpm != s_rpm_drawn) {
        s_rpm_drawn = rpm;
        lv_label_set_text_fmt(s_rpm_label, "%u", rpm);
        lv_obj_align(s_rpm_label, LV_ALIGN_CENTER, -1, -64);
        lv_obj_move_foreground(s_rpm_label);
    }
}

static lv_color_t ect_color(int16_t ect)
{
    if (ect < ECT_COLD_C) {
        return lv_color_make(0, 0, 255);
    }
    if (ect < ECT_HOT_C) {
        return lv_color_white();
    }
    return lv_color_make(255, 0, 0);
}

static lv_color_t lambda_color(float lambda)
{
    if (lambda < LAMBDA_RICH) {
        return lv_color_make(255, 0, 0);
    }
    if (lambda > LAMBDA_LEAN) {
        return lv_color_make(0, 0, 255);
    }
    return lv_color_white();
}

static void bottom_values_raise(void)
{
    int i;

    lv_obj_move_foreground(s_bottom_band);
    lv_obj_move_foreground(s_title_band);
    for (i = 0; i < SEP_LINE_COUNT; i++) {
        lv_obj_move_foreground(s_sep_lines[i]);
    }
    lv_obj_move_foreground(s_lambda_title);
    lv_obj_move_foreground(s_lambda_value);
    lv_obj_move_foreground(s_air_title);
    lv_obj_move_foreground(s_air_value);
    lv_obj_move_foreground(s_tps_title);
    lv_obj_move_foreground(s_tps_value);
    lv_obj_move_foreground(s_ect_title);
    lv_obj_move_foreground(s_ect_value);
}

static void bottom_apply(const can_data_t *data)
{
    char buf[16];
    const bool engine_changed = data->engine_valid != s_engine_valid_drawn;

    if (s_ect_value == NULL) {
        return;
    }

    if (data->lambda_valid != s_lambda_valid_drawn ||
        (data->lambda_valid && data->lambda1 != s_lambda_drawn)) {
        s_lambda_valid_drawn = data->lambda_valid;
        s_lambda_drawn = data->lambda_valid ? data->lambda1 : -1.0f;
        if (!data->lambda_valid) {
            lv_label_set_text(s_lambda_value, "-.--");
            lv_obj_set_style_text_color(s_lambda_value, lv_color_white(), 0);
        } else {
            snprintf(buf, sizeof(buf), "%.2f", data->lambda1);
            lv_label_set_text(s_lambda_value, buf);
            lv_obj_set_style_text_color(s_lambda_value, lambda_color(data->lambda1), 0);
        }
    }

    if (engine_changed || (data->engine_valid && data->iat_c != s_iat_drawn)) {
        if (!data->engine_valid) {
            s_iat_drawn = INT16_MIN;
            lv_label_set_text(s_air_value, "--");
        } else {
            s_iat_drawn = data->iat_c;
            lv_label_set_text_fmt(s_air_value, "%d", data->iat_c);
        }
        lv_obj_set_style_text_color(s_air_value, lv_color_white(), 0);
    }

    if (engine_changed || (data->engine_valid && data->tps_pct != s_tps_drawn)) {
        if (!data->engine_valid) {
            s_tps_drawn = -1.0f;
            lv_label_set_text(s_tps_value, "---");
        } else {
            s_tps_drawn = data->tps_pct;
            snprintf(buf, sizeof(buf), "%.1f%%", data->tps_pct);
            lv_label_set_text(s_tps_value, buf);
        }
        lv_obj_set_style_text_color(s_tps_value, lv_color_white(), 0);
    }

    if (engine_changed || (data->engine_valid && data->ect_c != s_ect_drawn)) {
        if (!data->engine_valid) {
            s_ect_drawn = INT16_MIN;
            lv_label_set_text(s_ect_value, "---");
            lv_obj_set_style_text_color(s_ect_value, lv_color_white(), 0);
        } else {
            s_ect_drawn = data->ect_c;
            lv_label_set_text_fmt(s_ect_value, "%d", data->ect_c);
            lv_obj_set_style_text_color(s_ect_value, ect_color(data->ect_c), 0);
        }
    }

    s_engine_valid_drawn = data->engine_valid;
    bottom_values_raise();
}

static void dash_set_hidden(bool hidden)
{
    int i;

    lv_obj_set_hidden(s_rpm_green, hidden);
    lv_obj_set_hidden(s_rpm_yellow, hidden);
    lv_obj_set_hidden(s_rpm_red, hidden);
    lv_obj_set_hidden(s_rpm_label, hidden);
    lv_obj_set_hidden(s_bottom_band, hidden);
    lv_obj_set_hidden(s_title_band, hidden);
    for (i = 0; i < SEP_LINE_COUNT; i++) {
        lv_obj_set_hidden(s_sep_lines[i], hidden);
    }
    lv_obj_set_hidden(s_lambda_title, hidden);
    lv_obj_set_hidden(s_lambda_value, hidden);
    lv_obj_set_hidden(s_air_title, hidden);
    lv_obj_set_hidden(s_air_value, hidden);
    lv_obj_set_hidden(s_tps_title, hidden);
    lv_obj_set_hidden(s_tps_value, hidden);
    lv_obj_set_hidden(s_ect_title, hidden);
    lv_obj_set_hidden(s_ect_value, hidden);
}

static void stop_engine_set(bool active)
{
    if (active == s_stop_active || s_stop_overlay == NULL) {
        return;
    }
    s_stop_active = active;
    s_stop_elapsed = 0;
    s_stop_lit = true;
    lv_obj_set_hidden(s_stop_text, false);
    lv_obj_set_hidden(s_stop_overlay, !active);
    dash_set_hidden(active);
    if (active) {
        lv_obj_move_foreground(s_stop_overlay);
    }
}

static void stop_engine_blink(void)
{
    bool on;

    s_stop_elapsed += 40;
    on = (s_stop_elapsed / STOP_BLINK_MS) % 2 == 0;
    if (on == s_stop_lit) {
        return;
    }
    s_stop_lit = on;
    lv_obj_set_hidden(s_stop_text, !on);
}

static bool stop_engine_needed(const can_data_t *data)
{
    return data->engine_valid && data->ect_c > (int16_t)s_ect_stop;
}

static void settings_set_open(bool open)
{
    can_data_t data;

    if (open == s_settings_open || s_settings == NULL) {
        return;
    }
    s_settings_open = open;
    lv_obj_set_hidden(s_settings, !open);
    if (open) {
        lv_obj_move_foreground(s_settings);
        return;
    }
    can_get_data(&data);
    if (stop_engine_needed(&data)) {
        if (s_stop_active) {
            lv_obj_move_foreground(s_stop_overlay);
        } else {
            stop_engine_set(true);
        }
        return;
    }
    stop_engine_set(false);
    dash_set_hidden(false);
    rpm_curtain_apply(s_rpm_shown);
    bottom_apply(&data);
    lv_obj_move_foreground(s_rpm_label);
}

static void settings_row_refresh(set_row_t *row)
{
    lv_label_set_text_fmt(row->lbl, "%u", *row->val);
}

static void settings_rows_refresh(void)
{
    int i;
    for (i = 0; i < SET_ROW_COUNT; i++) {
        if (s_set_rows[i].lbl) {
            settings_row_refresh(&s_set_rows[i]);
        }
    }
}

static void settings_clamp_ordered(void)
{
    if (s_rpm_end_r > RPM_MAX_CAP) {
        s_rpm_end_r = RPM_MAX_CAP;
    }
    if (s_rpm_end_y + 100 > s_rpm_end_r) {
        s_rpm_end_y = (uint16_t)(s_rpm_end_r - 100);
    }
    if (s_rpm_end_g + 100 > s_rpm_end_y) {
        s_rpm_end_g = (uint16_t)(s_rpm_end_y - 100);
    }
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("dash", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u16(h, "g", s_rpm_end_g);
    nvs_set_u16(h, "y", s_rpm_end_y);
    nvs_set_u16(h, "r", s_rpm_end_r);
    nvs_set_u16(h, "e", s_ect_stop);
    nvs_commit(h);
    nvs_close(h);
}

static void settings_load(void)
{
    nvs_handle_t h;
    uint16_t v;
    if (nvs_open("dash", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_u16(h, "g", &v) == ESP_OK) {
        s_rpm_end_g = v;
    }
    if (nvs_get_u16(h, "y", &v) == ESP_OK) {
        s_rpm_end_y = v;
    }
    if (nvs_get_u16(h, "r", &v) == ESP_OK) {
        s_rpm_end_r = v;
    }
    if (nvs_get_u16(h, "e", &v) == ESP_OK) {
        s_ect_stop = v;
    }
    nvs_close(h);
    settings_clamp_ordered();
}

static void settings_btn_cb(lv_event_t *e)
{
    set_row_t *row = lv_event_get_user_data(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const bool plus = (bool)(uintptr_t)lv_obj_get_user_data(btn);
    int32_t v = *row->val;

    if (plus) {
        v += row->step;
    } else {
        v -= row->step;
    }
    if (v < row->lo) {
        v = row->lo;
    }
    if (v > row->hi) {
        v = row->hi;
    }
    *row->val = (uint16_t)v;
    settings_clamp_ordered();
    settings_rows_refresh();
    settings_save();
    rpm_curtain_apply(s_rpm_shown);
}

static lv_obj_t *settings_mk_btn(lv_obj_t *parent, const char *txt, bool plus, set_row_t *row, int32_t x, int32_t y)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lab;

    lv_obj_set_size(btn, 72, 56);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(50, 50, 50), 0);
    lv_obj_set_user_data(btn, (void *)(uintptr_t)plus);
    lv_obj_add_event_cb(btn, settings_btn_cb, LV_EVENT_CLICKED, row);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    lv_obj_center(lab);
    return btn;
}

static void settings_mk_row(lv_obj_t *parent, int idx, const char *title, uint16_t *val, uint16_t lo, uint16_t hi,
                            uint16_t step, int32_t y)
{
    lv_obj_t *t = lv_label_create(parent);
    set_row_t *row = &s_set_rows[idx];

    row->val = val;
    row->lo = lo;
    row->hi = hi;
    row->step = step;
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_set_pos(t, 24, y + 12);
    row->lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(row->lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(row->lbl, lv_color_white(), 0);
    lv_obj_set_pos(row->lbl, 320, y);
    settings_row_refresh(row);
    settings_mk_btn(parent, "-", false, row, 520, y);
    settings_mk_btn(parent, "+", true, row, 620, y);
}

static void settings_build(lv_obj_t *screen)
{
    lv_obj_t *title;

    s_settings = lv_obj_create(screen);
    lv_obj_set_size(s_settings, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(s_settings, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings, 0, 0);
    lv_obj_set_style_radius(s_settings, 0, 0);
    lv_obj_set_style_pad_all(s_settings, 0, 0);
    lv_obj_set_style_text_color(s_settings, lv_color_white(), 0);
    lv_obj_set_scrollable(s_settings, false);
    lv_obj_set_hidden(s_settings, true);

    title = lv_label_create(s_settings);
    lv_label_set_text(title, "Reglages");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    settings_mk_row(s_settings, 0, "Fin vert", &s_rpm_end_g, 1000, 7000, 100, 100);
    settings_mk_row(s_settings, 1, "Fin jaune", &s_rpm_end_y, 2000, 7500, 100, 190);
    settings_mk_row(s_settings, 2, "Regime max", &s_rpm_end_r, 3000, RPM_MAX_CAP, 100, 280);
    settings_mk_row(s_settings, 3, "Eau max C", &s_ect_stop, 80, 130, 1, 370);
}

static void dash_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir;

    (void)e;
    if (!s_dash_ready) {
        return;
    }
    dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT && !s_settings_open) {
        settings_set_open(true);
    } else if (dir == LV_DIR_RIGHT && s_settings_open) {
        settings_set_open(false);
    }
    lv_indev_wait_release(lv_indev_active());
}

static void rpm_curtain_timer_cb(lv_timer_t *timer)
{
    can_data_t data;
    uint16_t target;
    uint16_t step;

    (void)timer;
    if (!s_dash_ready || s_settings_open) {
        return;
    }

    can_get_data(&data);
    if (stop_engine_needed(&data)) {
        stop_engine_set(true);
        stop_engine_blink();
        return;
    }
    stop_engine_set(false);
    bottom_apply(&data);
    target = data.engine_valid ? data.rpm : 0;

    if (s_rpm_shown < target) {
        step = (uint16_t)((target - s_rpm_shown) / 3);
        if (step < 30) {
            step = 30;
        }
        s_rpm_shown = (uint16_t)((s_rpm_shown + step > target) ? target : s_rpm_shown + step);
    } else if (s_rpm_shown > target) {
        step = (uint16_t)((s_rpm_shown - target) / 3);
        if (step < 30) {
            step = 30;
        }
        s_rpm_shown = (s_rpm_shown < target + step) ? target : (uint16_t)(s_rpm_shown - step);
    }

    rpm_curtain_apply(s_rpm_shown);
}

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t i2c_write_byte(uint8_t device_address, uint8_t value)
{
    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t device = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &device);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit(device, &value, 1, BOARD_I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(device);
    return err;
}

static void board_reset_touch(void)
{
    const gpio_config_t rst_config = {
        .pin_bit_mask = 1ULL << BOARD_PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_config));

    ESP_ERROR_CHECK(i2c_write_byte(BOARD_IO_EXPANDER_ADDR, 0x01));
    ESP_ERROR_CHECK(i2c_write_byte(BOARD_TOUCH_RESET_ADDR, 0x2C));
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(BOARD_PIN_TOUCH_RST, 0);
    esp_rom_delay_us(100 * 1000);
    ESP_ERROR_CHECK(i2c_write_byte(BOARD_TOUCH_RESET_ADDR, 0x2E));
    esp_rom_delay_us(200 * 1000);
}

static esp_lcd_panel_handle_t display_init(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
        .dma_burst_size = 64,
        .num_fbs = BOARD_LCD_NUM_FB,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = BOARD_PIN_DISP_EN,
        .pclk_gpio_num = BOARD_PIN_PCLK,
        .vsync_gpio_num = BOARD_PIN_VSYNC,
        .hsync_gpio_num = BOARD_PIN_HSYNC,
        .de_gpio_num = BOARD_PIN_DE,
        .data_gpio_nums = {
            BOARD_PIN_DATA0, BOARD_PIN_DATA1, BOARD_PIN_DATA2, BOARD_PIN_DATA3,
            BOARD_PIN_DATA4, BOARD_PIN_DATA5, BOARD_PIN_DATA6, BOARD_PIN_DATA7,
            BOARD_PIN_DATA8, BOARD_PIN_DATA9, BOARD_PIN_DATA10, BOARD_PIN_DATA11,
            BOARD_PIN_DATA12, BOARD_PIN_DATA13, BOARD_PIN_DATA14, BOARD_PIN_DATA15,
        },
        .timings = {
            .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .hsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = true,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static esp_lcd_touch_handle_t touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.scl_speed_hz = BOARD_I2C_FREQ_HZ;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io));

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(io, &touch_config, &touch));
    return touch;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(display);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = lv_indev_get_user_data(indev);
    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;

    esp_lcd_touch_read_data(touch);
    const esp_err_t err = esp_lcd_touch_get_data(touch, &point, &count, 1);

    if (err == ESP_OK && count > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_obj_t *sep_line_create(lv_obj_t *parent, int32_t w, int32_t h, int32_t x_ofs, int32_t y_ofs)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, w, h);
    lv_obj_align(line, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_bg_color(line, lv_color_make(205, 206, 205), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_scrollable(line, false);
    lv_obj_set_clickable(line, false);
    lv_obj_set_hidden(line, true);
    return line;
}

static lv_obj_t *metric_title_create(lv_obj_t *parent, const char *text, int32_t x_ofs)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_TRANSP, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, x_ofs, 141);
    lv_obj_set_hidden(title, true);
    return title;
}

static lv_obj_t *metric_value_create(lv_obj_t *parent, const char *text, int32_t x_ofs)
{
    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, text);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_opa(value, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(value, LV_OPA_TRANSP, 0);
    lv_obj_align(value, LV_ALIGN_CENTER, x_ofs, 203);
    lv_obj_set_hidden(value, true);
    return value;
}

static void splash_timer_cb(lv_timer_t *timer)
{
    can_data_t data;
    lv_obj_t *logo = lv_timer_get_user_data(timer);

    lv_obj_delete(logo);
    can_get_data(&data);
    s_rpm_shown = data.engine_valid ? data.rpm : 0;
    s_dash_ready = true;
    if (stop_engine_needed(&data)) {
        stop_engine_set(true);
        return;
    }
    dash_set_hidden(false);
    rpm_curtain_apply(s_rpm_shown);
    bottom_apply(&data);
    lv_obj_move_foreground(s_rpm_label);
}

static void lvgl_ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    s_rpm_green_px = (BOARD_LCD_H_RES * 2) / 15;
    s_rpm_yellow_px = s_rpm_green_px;
    s_rpm_red_px = BOARD_LCD_H_RES - s_rpm_green_px - s_rpm_yellow_px;
    s_rpm_green = rpm_band_create(screen, lv_color_make(0, 180, 40), 0);
    s_rpm_yellow = rpm_band_create(screen, lv_color_make(240, 190, 0), s_rpm_green_px);
    s_rpm_red = rpm_band_create(screen, lv_color_make(210, 20, 20), s_rpm_green_px + s_rpm_yellow_px);
    lv_timer_create(rpm_curtain_timer_cb, 40, NULL);

    lv_obj_t *logo = lv_image_create(screen);
    lv_image_set_src(logo, &toyota_logo);
    lv_obj_center(logo);

    lv_timer_t *splash = lv_timer_create(splash_timer_cb, 3000, logo);
    lv_timer_set_repeat_count(splash, 1);

    s_rpm_label = lv_label_create(screen);
    lv_label_set_text(s_rpm_label, "0");
    lv_obj_set_style_text_color(s_rpm_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_rpm_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_rpm_label, &rpm_font, 0);
    lv_obj_set_style_bg_opa(s_rpm_label, LV_OPA_TRANSP, 0);
    lv_obj_align(s_rpm_label, LV_ALIGN_CENTER, -1, -64);
    lv_obj_set_hidden(s_rpm_label, true);

    s_bottom_band = lv_obj_create(screen);
    lv_obj_set_size(s_bottom_band, BOARD_LCD_H_RES, BOTTOM_BAND_H);
    lv_obj_set_pos(s_bottom_band, 0, BOARD_LCD_V_RES - BOTTOM_BAND_H);
    lv_obj_set_style_bg_color(s_bottom_band, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bottom_band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bottom_band, 0, 0);
    lv_obj_set_style_radius(s_bottom_band, 0, 0);
    lv_obj_set_style_pad_all(s_bottom_band, 0, 0);
    lv_obj_set_scrollable(s_bottom_band, false);
    lv_obj_set_clickable(s_bottom_band, false);
    lv_obj_set_hidden(s_bottom_band, true);

    /* Bande blanche titres : entre Horizontal4 (centre+121) et Horizontal3 (centre+162). */
    s_title_band = lv_obj_create(screen);
    lv_obj_set_size(s_title_band, BOARD_LCD_H_RES, TITLE_BAND_H);
    lv_obj_set_pos(s_title_band, 0, TITLE_BAND_Y);
    lv_obj_set_style_bg_color(s_title_band, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_title_band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_title_band, 0, 0);
    lv_obj_set_style_radius(s_title_band, 0, 0);
    lv_obj_set_style_pad_all(s_title_band, 0, 0);
    lv_obj_set_scrollable(s_title_band, false);
    lv_obj_set_clickable(s_title_band, false);
    lv_obj_set_hidden(s_title_band, true);

    /* Separators from Dash-S-bastien Screen2 (Horizontal4/3, Vertical4/5/6). */
    s_sep_lines[0] = sep_line_create(screen, 797, 2, -1, 121);
    s_sep_lines[1] = sep_line_create(screen, 797, 2, -1, 162);
    s_sep_lines[2] = sep_line_create(screen, 2, 119, -205, 179);
    s_sep_lines[3] = sep_line_create(screen, 2, 119, -3, 180);
    s_sep_lines[4] = sep_line_create(screen, 2, 119, 201, 179);

    s_lambda_title = metric_title_create(screen, "Lambda", -306);
    s_lambda_value = metric_value_create(screen, "-.--", -309);
    s_air_title = metric_title_create(screen, "Air Motor", -102);
    s_air_value = metric_value_create(screen, "--", -105);
    s_tps_title = metric_title_create(screen, "Tps", 98);
    s_tps_value = metric_value_create(screen, "---", 102);
    s_ect_title = metric_title_create(screen, "Water", 303);
    s_ect_value = metric_value_create(screen, "---", 308);

    s_stop_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_stop_overlay, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_pos(s_stop_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_stop_overlay, lv_color_make(210, 20, 20), 0);
    lv_obj_set_style_bg_opa(s_stop_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_stop_overlay, 0, 0);
    lv_obj_set_style_radius(s_stop_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_stop_overlay, 0, 0);
    lv_obj_set_scrollable(s_stop_overlay, false);
    lv_obj_set_hidden(s_stop_overlay, true);

    s_stop_text = lv_obj_create(s_stop_overlay);
    lv_obj_set_size(s_stop_text, BOARD_LCD_H_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_stop_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_stop_text, 0, 0);
    lv_obj_set_style_pad_all(s_stop_text, 0, 0);
    lv_obj_set_style_pad_row(s_stop_text, 16, 0);
    lv_obj_set_layout(s_stop_text, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_stop_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_stop_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_stop_text, false);
    lv_obj_center(s_stop_text);

    s_stop_label = lv_label_create(s_stop_text);
    lv_label_set_text(s_stop_label, "STOP ENGINE");
    lv_obj_set_style_text_font(s_stop_label, &stop_font, 0);
    lv_obj_set_style_text_color(s_stop_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_stop_label, LV_OPA_COVER, 0);

    s_water_label = lv_label_create(s_stop_text);
    lv_label_set_text(s_water_label, "WATER HIGH");
    lv_obj_set_style_text_font(s_water_label, &stop_font, 0);
    lv_obj_set_style_text_color(s_water_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_water_label, LV_OPA_COVER, 0);

    settings_build(screen);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(BOARD_LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (1) {
        const uint32_t delay_ms = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
    }
}

static void lvgl_init_display(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    void *buf1 = NULL;
    void *buf2 = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &buf1, &buf2));

    lv_init();

    lv_display_t *display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
    lv_display_set_buffers(display, buf1, buf2, BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(display, panel);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    lv_indev_set_user_data(indev, touch);
    lv_indev_add_event_cb(indev, dash_gesture_cb, LV_EVENT_GESTURE, NULL);

    lvgl_ui_init();

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, BOARD_LVGL_TICK_PERIOD_MS * 1000));

    xTaskCreate(lvgl_task, "lvgl", BOARD_LVGL_TASK_STACK, NULL, BOARD_LVGL_TASK_PRIORITY, NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();

    esp_lcd_panel_handle_t panel = display_init();
    ESP_ERROR_CHECK(i2c_bus_init());
    board_reset_touch();
    s_touch = touch_init();
    lvgl_init_display(panel, s_touch);
    ESP_ERROR_CHECK(can_start());
}
