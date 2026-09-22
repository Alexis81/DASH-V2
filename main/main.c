#include "board.h"
#include "can.h"
#include "toyota_logo.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "dash";

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_touch_handle_t s_touch;
static lv_obj_t *s_coord_label;
static lv_point_t s_touch_point;
static bool s_touch_pressed;

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
        s_touch_point = data->point;
        s_touch_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_touch_pressed = false;
    }
}

static void coord_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *label = lv_timer_get_user_data(timer);
    if (s_touch_pressed) {
        lv_label_set_text_fmt(label, "x %d    y %d", (int)s_touch_point.x, (int)s_touch_point.y);
    } else {
        lv_label_set_text(label, "Aucun toucher");
    }
}

static void splash_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *logo = lv_timer_get_user_data(timer);
    lv_obj_delete(logo);
    lv_obj_remove_flag(s_coord_label, LV_OBJ_FLAG_HIDDEN);
}

static void lvgl_ui_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    lv_obj_t *logo = lv_image_create(screen);
    lv_image_set_src(logo, &toyota_logo);
    lv_obj_center(logo);

    s_coord_label = lv_label_create(screen);
    lv_label_set_text(s_coord_label, "Aucun toucher");
    lv_obj_align(s_coord_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_flag(s_coord_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(coord_timer_cb, 50, s_coord_label);

    lv_timer_t *splash = lv_timer_create(splash_timer_cb, 10000, logo);
    lv_timer_set_repeat_count(splash, 1);
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
    ESP_LOGI(TAG, "RGB panel");
    esp_lcd_panel_handle_t panel = display_init();

    ESP_LOGI(TAG, "I2C");
    ESP_ERROR_CHECK(i2c_bus_init());
    board_reset_touch();

    ESP_LOGI(TAG, "GT911");
    s_touch = touch_init();

    ESP_LOGI(TAG, "LVGL");
    lvgl_init_display(panel, s_touch);

    ESP_LOGI(TAG, "CAN");
    ESP_ERROR_CHECK(can_start());
}
