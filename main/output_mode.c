#include "output_mode.h"
#include "board_config.h"
#include "button_gesture.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "output";
static output_mode_t current;
static QueueHandle_t requests;

output_mode_t output_mode_get(void) { return current; }
esp_err_t output_mode_request(output_mode_t mode)
{
    if (mode != OUTPUT_WIFI && mode != OUTPUT_BLE) return ESP_ERR_INVALID_ARG;
    if (mode == current) return ESP_OK;
    return requests && xQueueSend(requests, &mode, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static esp_err_t save(output_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("psk_output", NVS_READWRITE, &handle);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_u8(handle, "mode", (uint8_t)mode);
    if (rc == ESP_OK) rc = nvs_commit(handle);
    nvs_close(handle);
    return rc;
}
static void mode_task(void *arg)
{
    (void)arg;
    button_gesture_t button = {0};
    output_mode_t next = current;
    bool pending = false;
    int64_t release_since = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        bool pressed = gpio_get_level(BOARD_SW1_GPIO) == 0;
        if (button_gesture_update(&button, pressed, (uint32_t)(now / 1000)) == BUTTON_LONG) {
            next = current == OUTPUT_WIFI ? OUTPUT_BLE : OUTPUT_WIFI;
            pending = true;
        }
        if (xQueueReceive(requests, &next, 0) == pdTRUE) pending = next != current;
        if (pending && !pressed) {
            if (!release_since) release_since = now;
            /* Allow request response to leave, and never reset with BOOT held. */
            if (now - release_since >= 750000) {
                esp_err_t rc = save(next);
                if (rc == ESP_OK) {
                    ESP_LOGI(TAG, "Switching to %s; rebooting", next == OUTPUT_BLE ? "BLE" : "Wi-Fi AP");
                    esp_restart();
                }
                ESP_LOGE(TAG, "Cannot save output mode: %s", esp_err_to_name(rc));
                pending = false;
                release_since = 0;
            }
        } else release_since = 0;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t output_mode_init(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init");
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("psk_output", NVS_READONLY, &handle);
    if (rc == ESP_OK) {
        uint8_t stored = OUTPUT_WIFI;
        rc = nvs_get_u8(handle, "mode", &stored);
        nvs_close(handle);
        if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) return rc;
        if (stored == OUTPUT_BLE) current = OUTPUT_BLE;
    } else if (rc != ESP_ERR_NVS_NOT_FOUND) return rc;
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOARD_SW1_GPIO, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "SW1 config");
    requests = xQueueCreate(1, sizeof(output_mode_t));
    ESP_RETURN_ON_FALSE(requests, ESP_ERR_NO_MEM, TAG, "switch queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(mode_task, "psk_output", 4096, NULL, 2, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "switch task");
    ESP_LOGI(TAG, "%s selected; hold SW1 2 s and release to switch", current == OUTPUT_BLE ? "BLE" : "Wi-Fi AP");
    return ESP_OK;
}
