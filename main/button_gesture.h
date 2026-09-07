#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { BUTTON_NONE, BUTTON_SHORT, BUTTON_LONG } button_event_t;
typedef struct { bool down, releasing; uint32_t pressed_ms, released_ms; } button_gesture_t;
/* Events occur only after a stable release, so GPIO9 is high before reboot. */
button_event_t button_gesture_update(button_gesture_t *button, bool down, uint32_t now_ms);
