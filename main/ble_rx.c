#include "ble_rx.h"
#include "ble_protocol.h"
#include "output_mode.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Nordic UART UUIDs, little-endian: phone RX=...0002, board TX=...0003.
 * ...0004 is our read-only status, separate from the received text stream. */
#define UART_UUID(id) BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,id,0x00,0x40,0x6e)
static const ble_uuid128_t service_uuid = UART_UUID(1);
static const ble_uuid128_t rx_uuid = UART_UUID(2);
static const ble_uuid128_t tx_uuid = UART_UUID(3);
static const ble_uuid128_t status_uuid = UART_UUID(4);
static const char *TAG = "ble-rx";
static char name[32];
static QueueHandle_t commands;
/* All BLE cursor/connection state belongs to the NimBLE host event queue. */
static struct ble_npl_callout pump;
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE, tx_handle;
static uint8_t address_type;
static bool subscribed, synced;
static uint64_t cursor, overwritten;
static uint32_t notifications, notify_errors;
static ble_line_t line;
static const char *last_command = "NONE";
static int gap_event(struct ble_gap_event *event, void *arg);

const char *ble_rx_name(void) { return name; }

static void advertise(void)
{
    if (!synced || connection != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    struct ble_hs_adv_fields scan = {0};
    scan.name = (uint8_t *)name;
    scan.name_len = strlen(name);
    scan.name_is_complete = 1;
    if (!rc) rc = ble_gap_adv_rsp_set_fields(&scan);
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = 160, .itvl_max = 240, /* 100..150 ms, name in scan response. */
    };
    if (!rc) rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "Advertising failed: %d; retrying", rc);
}
static void received_command(const char *text, void *arg)
{
    (void)arg;
    ble_command_t cmd = ble_command_parse(text);
    switch (cmd.type) {
    case BLE_COMMAND_REPLAY:
        cursor = 0;
        last_command = "REPLAY";
        break;
    case BLE_COMMAND_WIFI:
        last_command = output_mode_request(OUTPUT_WIFI) == ESP_OK ? "WIFI_REBOOT" : "BUSY";
        break;
    case BLE_COMMAND_SET: {
        receiver_status_t state;
        receiver_get_status(&state);
        modem_command_t request = {.mode = cmd.mode, .tone_hz = cmd.tone_hz};
        last_command = !state.audio_ready ? "AUDIO_NOT_READY" :
            xQueueSend(commands, &request, 0) == pdTRUE ? "QUEUED" : "BUSY";
        break;
    }
    default: last_command = "INVALID"; break;
    }
}
static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctx->chr->uuid, &rx_uuid.u) == 0) {
        uint8_t bytes[BLE_TEXT_MAX_PAYLOAD];
        uint16_t count = 0;
        if (OS_MBUF_PKTLEN(ctx->om) > sizeof(bytes)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &count)) return BLE_ATT_ERR_UNLIKELY;
        ble_line_feed(&line, bytes, count, received_command, NULL);
        return 0;
    }
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctx->chr->uuid, &status_uuid.u) == 0) {
        receiver_status_t s;
        receiver_get_status(&s);
        char value[320];
        int count = snprintf(value, sizeof(value),
            "mode=%s tone=%d ready=%d signal=%d quality=%u rev=%" PRIu32
            " dsp_us=%" PRIu32 " max_us=%" PRIu32 " dma_ovf=%" PRIu32
            " cursor=%" PRIu64 " history_lost=%" PRIu64 " notify=%" PRIu32
            " notify_err=%" PRIu32 " heap=%" PRIu32 " cmd=%s",
            modem_mode_name(s.mode), s.tone_hz, s.audio_ready, s.signal, s.quality, s.control_revision,
            s.dsp_us, s.max_dsp_us, s.dma_overflows, cursor, overwritten, notifications,
            notify_errors, esp_get_free_heap_size(), last_command);
        if (count < 0 || count >= sizeof(value)) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctx->om, value, count) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid = &rx_uuid.u, .access_cb = access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
        {.uuid = &tx_uuid.u, .access_cb = access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle},
        {.uuid = &status_uuid.u, .access_cb = access, .flags = BLE_GATT_CHR_F_READ},
        {0},
     }}, {0},
};
static void send_text(struct ble_npl_event *event)
{
    (void)event;
    if (synced && connection == BLE_HS_CONN_HANDLE_NONE) advertise();
    if (subscribed && connection != BLE_HS_CONN_HANDLE_NONE) {
        uint8_t bytes[BLE_TEXT_MAX_PAYLOAD];
        uint64_t next, dropped;
        size_t capacity = ble_payload_limit(ble_att_mtu(connection));
        size_t count = receiver_read(cursor, bytes, capacity, &next, &dropped);
        /* Move past overwritten history once, even if a notify needs retry. */
        cursor = next - count;
        overwritten += dropped;
        if (count) {
            struct os_mbuf *buffer = ble_hs_mbuf_from_flat(bytes, count);
            /* NimBLE consumes the mbuf on both success and failure. */
            int rc = buffer ? ble_gatts_notify_custom(connection, tx_handle, buffer) : BLE_HS_ENOMEM;
            if (rc == 0) { cursor = next; ++notifications; }
            else ++notify_errors; /* Retain cursor: bounded retry next tick. */
        }
    }
    ble_npl_callout_reset(&pump, ble_npl_time_ms_to_ticks32(25));
}
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connection = event->connect.conn_handle;
            subscribed = false;
            cursor = 0; /* New client gets currently retained history after subscription. */
            memset(&line, 0, sizeof(line));
            last_command = "NONE";
            const struct ble_gap_upd_params parameters = {
                .itvl_min = 24, .itvl_max = 40, .latency = 0, .supervision_timeout = 400,
            };
            (void)ble_gap_update_params(connection, &parameters);
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        subscribed = false;
        connection = BLE_HS_CONN_HANDLE_NONE;
        memset(&line, 0, sizeof(line));
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == connection && event->subscribe.attr_handle == tx_handle)
            subscribed = event->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: advertise(); break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status != 0) ++notify_errors;
        break;
    default: break;
    }
    return 0;
}
static void on_reset(int reason)
{
    synced = subscribed = false;
    connection = BLE_HS_CONN_HANDLE_NONE;
    memset(&line, 0, sizeof(line));
    ESP_LOGW(TAG, "Host reset: %d", reason);
}
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc) { ESP_LOGE(TAG, "Address init failed: %d", rc); return; }
    synced = true;
    advertise();
    ESP_LOGI(TAG, "%s ready; subscribe to UART TX notifications", name);
}
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
esp_err_t ble_rx_start(QueueHandle_t controls)
{
    commands = controls;
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "BT MAC");
    snprintf(name, sizeof(name), "ESP32C3-PSK-BLE-%02X%02X", mac[4], mac[5]);
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "NimBLE init");
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set(name) || ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_att_set_preferred_mtu(185);
    ble_npl_callout_init(&pump, nimble_port_get_dflt_eventq(), send_text, NULL);
    ble_npl_callout_reset(&pump, ble_npl_time_ms_to_ticks32(25));
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
