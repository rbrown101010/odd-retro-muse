#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { CH_BUTTON_A=1, CH_BUTTON_B=2, CH_BUTTON_VOICE=4 };
typedef struct { uint16_t previous; int64_t a_down; bool chord_consumed; } chromatic_buttons_t;
/* A+B acts once per complete press/release and consumes both individual taps,
 * regardless of which button is pressed or released first. */
static inline unsigned chromatic_button_actions(chromatic_buttons_t *s, uint16_t b, int64_t now) {
    const uint16_t a=1u<<3, bb=1u<<2, both=a|bb;
    uint16_t rising=b & ~s->previous, falling=s->previous & ~b;
    unsigned actions=0;
    if(rising&a) s->a_down=now;
    if((b&both)==both && !s->chord_consumed) {
        s->chord_consumed=true; actions|=CH_BUTTON_VOICE;
    }
    if(!s->chord_consumed) {
        if((falling&a) && now-s->a_down<5000000) actions|=CH_BUTTON_A;
        if(falling&bb) actions|=CH_BUTTON_B;
    }
    if(!(b&both)) s->chord_consumed=false;
    s->previous=b;
    return actions;
}
