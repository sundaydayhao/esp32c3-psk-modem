#include "button_gesture.h"

button_event_t button_gesture_update(button_gesture_t *b, bool down, uint32_t now)
{
    if (down) {
        if (!b->down) { b->down = true; b->pressed_ms = now; }
        b->releasing = false;
    } else if (b->down) {
        if (!b->releasing) { b->releasing = true; b->released_ms = now; }
        if ((uint32_t)(now - b->released_ms) >= 80) {
            uint32_t duration = b->released_ms - b->pressed_ms;
            b->down = b->releasing = false;
            return duration >= 2000 ? BUTTON_LONG : duration >= 50 ? BUTTON_SHORT : BUTTON_NONE;
        }
    }
    return BUTTON_NONE;
}
