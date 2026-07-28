#include "display.h"

#include <assert.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
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
static lv_obj_t *s_text;
static lv_obj_t *s_status;
static lv_obj_t *s_menu;
static modem_mode_t s_pending_mode;
static int s_has_pending_mode;
static int s_pending_tone_hz;
static int s_has_pending_tone;
static esp_lcd_touch_handle_t s_touch;
static i2c_master_bus_handle_t s_i2c_bus;

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

static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t conf = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&conf, &s_i2c_bus);
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

static void create_ui(lv_disp_t *disp)
{
    lv_obj_t *screen = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8ecef), 0);

    s_status = lv_label_create(screen);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 4);

    s_spectrum = lv_canvas_create(screen);
    lv_obj_set_size(s_spectrum, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT / 2 - 20);
    lv_obj_align(s_spectrum, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_add_flag(s_spectrum, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_spectrum, spectrum_touch_cb, LV_EVENT_CLICKED, NULL);

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
    ESP_ERROR_CHECK(i2c_master_init());

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
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    gpio_set_level(BOARD_LCD_PIN_BL, BOARD_LCD_BL_ON_LEVEL);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle));
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
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

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
    static uint8_t canvas_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(BOARD_LCD_WIDTH, 120)];
    static int canvas_ready;
    if (!canvas_ready) {
        lv_canvas_set_buffer(s_spectrum, canvas_buf, BOARD_LCD_WIDTH, 120, LV_IMG_CF_TRUE_COLOR);
        canvas_ready = 1;
    }

    static int frame_div;
    if (++frame_div % 5 != 0) {
        return;
    }

    lv_canvas_fill_bg(s_spectrum, lv_color_hex(0x101418), LV_OPA_COVER);
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = lv_color_hex(0x2bd4bd);
    bar.bg_opa = LV_OPA_COVER;
    const int bar_w = BOARD_LCD_WIDTH / SPECTRUM_BINS;
    for (int i = 0; i < SPECTRUM_BINS; ++i) {
        const int h = 4 + (spectrum->bins[i] * 110) / 255;
        lv_canvas_draw_rect(s_spectrum, i * bar_w, 120 - h, bar_w > 1 ? bar_w - 1 : 1, h, &bar);
    }

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
