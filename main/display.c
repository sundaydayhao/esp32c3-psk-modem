#include "display.h"

#include <assert.h>
#include <string.h>

#include "board_i2c.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "display";

static lv_obj_t *s_spectrum;
static lv_obj_t *s_tone_marker;
static lv_obj_t *s_text;
static lv_obj_t *s_status;
static lv_obj_t *s_menu;
static modem_mode_t s_pending_mode;
static int s_has_pending_mode;
static int s_pending_tone_hz;
static int s_has_pending_tone;
static esp_lcd_touch_handle_t s_touch;

#define WATERFALL_HEIGHT 120
#define WATERFALL_ROWS_PER_FRAME 4

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    (void)panel_io;
    (void)edata;
    return false;
}

static void update_lcd_and_touch_rotation(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;

    switch (drv->rotated) {
    case LV_DISP_ROT_NONE:
        break;
    case LV_DISP_ROT_90:
        swap_xy = true;
        mirror_y = true;
        break;
    case LV_DISP_ROT_180:
        mirror_x = true;
        mirror_y = true;
        break;
    case LV_DISP_ROT_270:
        swap_xy = true;
        mirror_x = true;
        break;
    }

    esp_lcd_panel_swap_xy(panel_handle, swap_xy);
    esp_lcd_panel_mirror(panel_handle, mirror_x, mirror_y);

    if (s_touch) {
        esp_lcd_touch_set_swap_xy(s_touch, swap_xy);
        esp_lcd_touch_set_mirror_x(s_touch, mirror_x);
        esp_lcd_touch_set_mirror_y(s_touch, mirror_y);
    }
}

static void lvgl_port_update_cb(lv_disp_drv_t *drv)
{
    update_lcd_and_touch_rotation(drv);
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_read_data(drv->user_data);
    if (esp_lcd_touch_get_coordinates(drv->user_data, x, y, NULL, &count, 1) && count > 0) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(2);
    (void)arg;
}

static void mode_button_cb(lv_event_t *event)
{
    s_pending_mode = (modem_mode_t)(uintptr_t)lv_event_get_user_data(event);
    s_has_pending_mode = 1;
}

static void spectrum_touch_cb(lv_event_t *event)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    const lv_area_t *coords = &s_spectrum->coords;
    const int width = coords->x2 - coords->x1 + 1;
    int rel_x = p.x - coords->x1;
    if (rel_x < 0) {
        rel_x = 0;
    } else if (rel_x >= width) {
        rel_x = width - 1;
    }
    s_pending_tone_hz = 100 + (rel_x * 3400) / width;
    s_has_pending_tone = 1;
    (void)event;
}

static lv_color_t waterfall_color(uint8_t power)
{
    if (power < 12) {
        return lv_color_hex(0x000000);
    }
    if (power < 96) {
        const uint8_t b = 48 + (power * 160) / 96;
        return lv_color_make(0, 0, b);
    }
    if (power < 170) {
        const uint8_t t = (uint8_t)(((power - 96) * 255) / 74);
        return lv_color_make(t / 4, t / 4, 210);
    }
    if (power < 230) {
        const uint8_t t = (uint8_t)(((power - 170) * 255) / 60);
        return lv_color_make(40 + t, 40 + t, 210 - (t * 170) / 255);
    }
    return lv_color_make(255, 245, 160);
}

static uint8_t waterfall_power_at_x(const spectrum_t *spectrum, int x)
{
    const int scaled = x * (SPECTRUM_BINS - 1) * 256 / (BOARD_LCD_WIDTH - 1);
    const int bin = scaled / 256;
    const int frac = scaled % 256;
    const int next = bin + 1 < SPECTRUM_BINS ? bin + 1 : bin;
    const int a = spectrum->bins[bin];
    const int b = spectrum->bins[next];
    return (uint8_t)((a * (256 - frac) + b * frac) / 256);
}

static void waterfall_render_row(lv_color_t *buf, const spectrum_t *spectrum)
{
    memmove(buf + BOARD_LCD_WIDTH * WATERFALL_ROWS_PER_FRAME, buf,
            BOARD_LCD_WIDTH * (WATERFALL_HEIGHT - WATERFALL_ROWS_PER_FRAME) * sizeof(buf[0]));

    for (int row = 0; row < WATERFALL_ROWS_PER_FRAME; ++row) {
        lv_color_t *line = buf + row * BOARD_LCD_WIDTH;
        for (int x = 0; x < BOARD_LCD_WIDTH; ++x) {
            line[x] = waterfall_color(waterfall_power_at_x(spectrum, x));
        }
    }
}

static void update_tone_marker(int rx_tone_hz)
{
    int marker_x = ((rx_tone_hz - 100) * BOARD_LCD_WIDTH) / 3400;
    if (marker_x < 0) {
        marker_x = 0;
    } else if (marker_x >= BOARD_LCD_WIDTH) {
        marker_x = BOARD_LCD_WIDTH - 1;
    }

    if (s_tone_marker) {
        lv_obj_set_pos(s_tone_marker, s_spectrum->coords.x1 + marker_x, s_spectrum->coords.y1);
    }
}

static void create_ui(lv_disp_t *disp)
{
    lv_obj_t *screen = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8ecef), 0);

    s_status = lv_label_create(screen);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 4);

    s_spectrum = lv_canvas_create(screen);
    lv_obj_set_size(s_spectrum, BOARD_LCD_WIDTH, WATERFALL_HEIGHT);
    lv_obj_align(s_spectrum, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_add_flag(s_spectrum, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_spectrum, spectrum_touch_cb, LV_EVENT_CLICKED, NULL);

    s_tone_marker = lv_obj_create(screen);
    lv_obj_set_size(s_tone_marker, 2, WATERFALL_HEIGHT);
    lv_obj_set_style_bg_color(s_tone_marker, lv_color_hex(0xff1b1b), 0);
    lv_obj_set_style_bg_opa(s_tone_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tone_marker, 0, 0);
    lv_obj_set_style_radius(s_tone_marker, 0, 0);
    lv_obj_clear_flag(s_tone_marker, LV_OBJ_FLAG_CLICKABLE);

    s_text = lv_label_create(screen);
    lv_obj_set_width(s_text, BOARD_LCD_WIDTH - 12);
    lv_obj_set_height(s_text, BOARD_LCD_HEIGHT / 2 - 8);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_text, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_text, "");

    s_menu = lv_obj_create(screen);
    lv_obj_set_size(s_menu, BOARD_LCD_WIDTH - 20, BOARD_LCD_HEIGHT - 40);
    lv_obj_center(s_menu);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_menu, 8, 0);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);

    for (modem_mode_t mode = 0; mode < MODEM_MODE_COUNT; ++mode) {
        lv_obj_t *btn = lv_btn_create(s_menu);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 30);
        lv_obj_add_event_cb(btn, mode_button_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)mode);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, modem_mode_name(mode));
        lv_obj_center(label);
    }
}

esp_err_t display_init(void)
{
    i2c_master_bus_handle_t touch_i2c_bus = NULL;
    ESP_ERROR_CHECK(board_i2c_get_bus(&touch_i2c_bus));

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = BOARD_LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    static lv_disp_drv_t disp_drv;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    gpio_set_level(BOARD_LCD_PIN_BL, BOARD_LCD_BL_ON_LEVEL);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = BOARD_I2C_SPEED_HZ;
    tp_io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(touch_i2c_bus, &tp_io_config, &tp_io_handle));
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_WIDTH,
        .y_max = BOARD_LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BOARD_TOUCH_PIN_INT,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch));

    lv_init();
    lv_color_t *buf1 = heap_caps_malloc(BOARD_LCD_WIDTH * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(BOARD_LCD_WIDTH * 20 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 && buf2);
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, BOARD_LCD_WIDTH * 20);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BOARD_LCD_WIDTH;
    disp_drv.ver_res = BOARD_LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.drv_update_cb = lvgl_port_update_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    update_lcd_and_touch_rotation(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = s_touch;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2000));

    create_ui(disp);
    ESP_LOGI(TAG, "LCD/touch backend ready");
    return ESP_OK;
}

void display_append_text(display_state_t *state, const char *text)
{
    const size_t add = strlen(text);
    if (add == 0) {
        return;
    }
    if (add >= sizeof(state->text)) {
        text += add - sizeof(state->text) + 1;
    }

    size_t keep = state->text_len;
    const size_t incoming = strlen(text);
    if (keep + incoming >= sizeof(state->text)) {
        const size_t drop = keep + incoming - sizeof(state->text) + 1;
        memmove(state->text, state->text + drop, keep - drop);
        keep -= drop;
    }
    memcpy(state->text + keep, text, incoming + 1);
    state->text_len = keep + incoming;
}

void display_render(const display_state_t *state, const spectrum_t *spectrum)
{
    static lv_color_t canvas_buf[BOARD_LCD_WIDTH * WATERFALL_HEIGHT];
    static int canvas_ready;
    if (!canvas_ready) {
        lv_canvas_set_buffer(s_spectrum, canvas_buf, BOARD_LCD_WIDTH, WATERFALL_HEIGHT, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(s_spectrum, lv_color_hex(0x000000), LV_OPA_COVER);
        canvas_ready = 1;
    }

    waterfall_render_row(canvas_buf, spectrum);
    lv_obj_invalidate(s_spectrum);
    update_tone_marker(state->rx_tone_hz);

    char status[64];
    snprintf(status, sizeof(status), "%s  RX %d Hz  Peak %d Hz",
             modem_mode_name(state->selected_mode), state->rx_tone_hz, spectrum->peak_hz);
    lv_label_set_text(s_status, status);
    lv_label_set_text(s_text, state->text);
    display_set_menu_open(state->menu_open);
}

void display_set_menu_open(int open)
{
    if (!s_menu) {
        return;
    }
    if (open) {
        lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

int display_take_mode_selection(modem_mode_t *mode)
{
    if (!s_has_pending_mode) {
        return 0;
    }
    *mode = s_pending_mode;
    s_has_pending_mode = 0;
    return 1;
}

int display_take_tone_selection(int *tone_hz)
{
    if (!s_has_pending_tone) {
        return 0;
    }
    *tone_hz = s_pending_tone_hz;
    s_has_pending_tone = 0;
    return 1;
}

void display_task_handler(void)
{
    lv_timer_handler();
}
