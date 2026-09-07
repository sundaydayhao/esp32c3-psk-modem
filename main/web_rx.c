#include "web_rx.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "output_mode.h"

static const char *TAG = "web-rx";
static QueueHandle_t commands;
static uint32_t session;
static char ap_ssid[33];

extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[] asm("_binary_index_html_end");

static void response_headers(httpd_req_t *req, const char *type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}
static esp_err_t page_get(httpd_req_t *req)
{
    response_headers(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start - 1);
}
static esp_err_t status_get(httpd_req_t *req)
{
    receiver_status_t s;
    receiver_get_status(&s);
    wifi_sta_list_t clients = {0};
    esp_wifi_ap_get_sta_list(&clients);
    char body[1400];
    int length = snprintf(body, sizeof(body),
        "{\"mode\":%d,\"name\":\"%s\",\"baud\":%.2f,\"tone_hz\":%d,"
        "\"signal\":%s,\"quality\":%u,\"audio_ready\":%s,\"rms\":%" PRIu32 ","
        "\"samples\":%" PRIu32 ",\"blocks\":%" PRIu32 ",\"dsp_us\":%" PRIu32 ","
        "\"max_dsp_us\":%" PRIu32 ",\"deadline_misses\":%" PRIu32 ","
        "\"dma_overflows\":%" PRIu32 ",\"read_errors\":%" PRIu32 ","
        "\"clipped_samples\":%" PRIu32 ",\"output_drops\":%" PRIu32 ",\"lcd_drops\":%" PRIu32 ","
        "\"invalid_codes\":%" PRIu32 ",\"control_revision\":%" PRIu32 ","
        "\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
        "\"uptime_ms\":%" PRId64 ",\"clients\":%d,\"ssid\":\"%s\",\"session\":%" PRIu32 "}",
        s.mode, modem_mode_name(s.mode), 8000.0 / modem_samples_per_symbol(s.mode), s.tone_hz,
        s.signal ? "true" : "false", s.quality, s.audio_ready ? "true" : "false", s.rms,
        s.samples, s.blocks, s.dsp_us, s.max_dsp_us, s.deadline_misses,
        s.dma_overflows, s.read_errors, s.clipped_samples, s.output_drops, s.lcd_drops,
        s.invalid_codes, s.control_revision, esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
        esp_timer_get_time() / 1000, clients.num, ap_ssid, session);
    if (length < 0 || length >= sizeof(body)) return ESP_FAIL;
    response_headers(req, "application/json");
    return httpd_resp_send(req, body, length);
}
static bool unsigned_number(const char *value, uint64_t *out)
{
    if (!value[0]) return false;
    for (const char *p = value; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    unsigned long long number = strtoull(value, &end, 10);
    if (errno || *end) return false;
    *out = number;
    return true;
}
static esp_err_t rx_get(httpd_req_t *req)
{
    char query[64], number[32];
    uint64_t since = 0;
    if (httpd_req_get_url_query_len(req)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "since", number, sizeof(number)) != ESP_OK ||
            !unsigned_number(number, &since)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid cursor");
        }
    }
    uint8_t bytes[1024];
    uint64_t next, dropped;
    size_t length = receiver_read(since, bytes, sizeof(bytes), &next, &dropped);
    char next_text[24], dropped_text[24], session_text[16];
    snprintf(next_text, sizeof(next_text), "%" PRIu64, next);
    snprintf(dropped_text, sizeof(dropped_text), "%" PRIu64, dropped);
    snprintf(session_text, sizeof(session_text), "%" PRIu32, session);
    response_headers(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-RX-Next", next_text);
    httpd_resp_set_hdr(req, "X-RX-Dropped", dropped_text);
    httpd_resp_set_hdr(req, "X-RX-Session", session_text);
    return httpd_resp_send(req, (const char *)bytes, length);
}
static esp_err_t config_post(httpd_req_t *req)
{
    /* A custom header prevents a different website from changing the radio
     * with a cross-origin form. This AP does not enable CORS. */
    char client[16];
    if (httpd_req_get_hdr_value_str(req, "X-PSK-Client", client, sizeof(client)) != ESP_OK ||
        strcmp(client, "receiver") != 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Receiver page required");
    }
    char body[64], mode_text[16], tone_text[16];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid configuration");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += (size_t)n;
    }
    body[received] = 0;
    uint64_t mode, tone;
    if (httpd_query_key_value(body, "mode", mode_text, sizeof(mode_text)) != ESP_OK ||
        httpd_query_key_value(body, "tone", tone_text, sizeof(tone_text)) != ESP_OK ||
        !unsigned_number(mode_text, &mode) || !unsigned_number(tone_text, &tone) ||
        mode >= MODEM_MODE_COUNT || !modem_mode_supported((modem_mode_t)mode) || tone > 3500) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported mode or frequency");
    }
    unsigned margin = 8000 / modem_samples_per_symbol((modem_mode_t)mode) + 100;
    if (tone < margin || tone > 4000 - margin) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Frequency too close to audio band edge");
    }
    modem_command_t command = {.mode = (int)mode, .tone_hz = (int)tone};
    receiver_status_t current;
    receiver_get_status(&current);
    if (!current.audio_ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Audio receiver is not ready");
    }
    if (xQueueSend(commands, &command, 0) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Control queue full; retry");
    }
    response_headers(req, "application/json");
    httpd_resp_set_status(req, "202 Accepted");
    char reply[80];
    snprintf(reply, sizeof(reply), "{\"queued\":true,\"revision\":%" PRIu32 "}", current.control_revision);
    return httpd_resp_sendstr(req, reply);
}
esp_err_t web_rx_start(QueueHandle_t controls)
{
    _Static_assert(sizeof(BOARD_AP_PASSWORD) >= 9 && sizeof(BOARD_AP_PASSWORD) <= 64, "WPA2 password must be 8..63 characters");
    _Static_assert(sizeof(BOARD_AP_SSID) <= 27, "SSID needs room for MAC suffix");
    commands = controls;
    session = esp_random();
    /* Do not erase existing NVS on a version/storage error. Report it. */
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "network init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap, ESP_ERR_NO_MEM, TAG, "AP interface");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "AP MAC");
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X", BOARD_AP_SSID, mac[4], mac[5]);
    wifi_config_t config = { .ap = {
        .channel = BOARD_AP_CHANNEL, .max_connection = BOARD_AP_MAX_CLIENTS,
        .authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = {.required = false},
    }};
    strlcpy((char *)config.ap.ssid, ap_ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen((char *)config.ap.ssid);
    strlcpy((char *)config.ap.password, BOARD_AP_PASSWORD, sizeof(config.ap.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.task_priority = 3;
    server_config.stack_size = 6144;
    server_config.max_open_sockets = 5;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 2;
    server_config.send_wait_timeout = 2;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &server_config), TAG, "HTTP server");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/rx", .method = HTTP_GET, .handler = rx_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "HTTP route");
    }
    ESP_LOGI(TAG, "AP %s ready; open http://192.168.4.1", ap_ssid);
    return ESP_OK;
}
