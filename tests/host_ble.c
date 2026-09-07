#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_protocol.h"
#include "button_gesture.h"
#include "rx_history.h"

static ble_command_t parsed[16];
static unsigned parsed_count;
static void received(const char *text, void *user)
{
    (void)user;
    assert(parsed_count < 16);
    parsed[parsed_count++] = ble_command_parse(text);
}
static void commands(void)
{
    const char *bad[] = {NULL, "", "WIFI!", "SET QPSK31 1500", "SET BPSK500 -1500",
        "SET BPSK500 +1500", "SET BPSK500 4294968796", "SET BPSK500 18446744073709553116",
        "SET BPSK500 1500x", "SET BPSK500 1500 extra", "SET BPSK500 ", "SET  1500",
        "SET BPSK1000 1099", "SET BPSK1000 2901", "SET BPSK500 599", "SET BPSK500 3401"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        assert(ble_command_parse(bad[i]).type == BLE_COMMAND_INVALID);
    assert(ble_command_parse("SET BPSK1000 1100").type == BLE_COMMAND_SET);
    assert(ble_command_parse("SET BPSK1000 2900").type == BLE_COMMAND_SET);
    for (unsigned mode = 0; mode < MODEM_MODE_COUNT; ++mode) {
        if (!modem_mode_supported((modem_mode_t)mode)) continue;
        char text[40];
        snprintf(text, sizeof(text), "SET %s 1500", modem_mode_name((modem_mode_t)mode));
        ble_command_t command = ble_command_parse(text);
        assert(command.type == BLE_COMMAND_SET && command.mode == (modem_mode_t)mode && command.tone_hz == 1500);
    }
    ble_line_t line = {0};
    const char *text = "SET BPSK500 1500\r\nREPLAY\nWIFI\n";
    for (size_t n = 0; n < strlen(text); ++n)
        ble_line_feed(&line, (const uint8_t *)text + n, 1, received, NULL);
    assert(parsed_count == 3 && parsed[0].type == BLE_COMMAND_SET &&
           parsed[1].type == BLE_COMMAND_REPLAY && parsed[2].type == BLE_COMMAND_WIFI);
    uint8_t oversized[90]; memset(oversized, 'X', sizeof(oversized));
    ble_line_feed(&line, oversized, sizeof(oversized), received, NULL);
    const uint8_t recovery[] = "\nWIFI\nSET\0BPSK500 1500\nREPLAY\n";
    ble_line_feed(&line, recovery, sizeof(recovery)-1, received, NULL);
    assert(parsed_count == 7 && parsed[3].type == BLE_COMMAND_INVALID &&
           parsed[4].type == BLE_COMMAND_WIFI && parsed[5].type == BLE_COMMAND_INVALID &&
           parsed[6].type == BLE_COMMAND_REPLAY);
}
static void gestures(void)
{
    button_gesture_t b = {0};
    assert(button_gesture_update(&b, true, 100) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 110) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 190) == BUTTON_NONE); /* bounce */
    assert(button_gesture_update(&b, true, 200) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 400) == BUTTON_NONE);
    assert(button_gesture_update(&b, true, 405) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 410) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 489) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 490) == BUTTON_SHORT);
    assert(button_gesture_update(&b, false, 500) == BUTTON_NONE);
    assert(button_gesture_update(&b, true, 1000) == BUTTON_NONE);
    assert(button_gesture_update(&b, true, 9000) == BUTTON_NONE); /* no BOOT-held reset */
    assert(button_gesture_update(&b, false, 9010) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 9090) == BUTTON_LONG);
    assert(button_gesture_update(&b, false, 9200) == BUTTON_NONE);
    assert(button_gesture_update(&b, true, UINT32_MAX-1000) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 999) == BUTTON_NONE);
    assert(button_gesture_update(&b, false, 1079) == BUTTON_LONG);
}
static void payloads(void)
{
    assert(ble_payload_limit(0) == 0 && ble_payload_limit(22) == 0);
    assert(ble_payload_limit(23) == 20 && ble_payload_limit(185) == 180);
    assert(ble_payload_limit(517) == 180);
    rx_history_t h = {0};
    char input[4300];
    for (size_t i=0; i<sizeof(input); ++i) input[i] = (char)(i%251);
    rx_history_append(&h, input, sizeof(input));
    for (unsigned mtu=23; mtu<=185; mtu+=162) {
        uint64_t cursor=0, next, dropped;
        uint8_t bytes[180], retry[180];
        size_t total=0;
        while (total<RX_HISTORY_CAPACITY) {
            size_t count=rx_history_read(&h, cursor, bytes, ble_payload_limit(mtu), &next, &dropped);
            assert(dropped == (total ? 0 : sizeof(input)-RX_HISTORY_CAPACITY));
            cursor=next-count; /* immediate notify failure retains unsent bytes */
            uint64_t retry_next, retry_dropped;
            assert(rx_history_read(&h,cursor,retry,ble_payload_limit(mtu),&retry_next,&retry_dropped)==count);
            assert(retry_next==next && retry_dropped==0 && !memcmp(bytes,retry,count));
            assert(!memcmp(bytes,input+sizeof(input)-RX_HISTORY_CAPACITY+total,count));
            total+=count; cursor=next;
        }
        assert(cursor==sizeof(input));
    }
}
int main(void)
{
    commands(); gestures(); payloads();
    puts("PASS BLE command framing/validation, button gestures, MTU/history/retry");
    return 0;
}
