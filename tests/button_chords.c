#include "chromatic_buttons.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 const uint16_t a=1<<3,b=1<<2;chromatic_buttons_t s={0};
 assert(!chromatic_button_actions(&s,a,0));assert(chromatic_button_actions(&s,a|b,10)==CH_BUTTON_VOICE);
 assert(!chromatic_button_actions(&s,a|b,20));assert(!chromatic_button_actions(&s,b,30));assert(!chromatic_button_actions(&s,0,40));
 assert(!chromatic_button_actions(&s,b,50));assert(chromatic_button_actions(&s,a|b,60)==CH_BUTTON_VOICE);
 assert(!chromatic_button_actions(&s,a,70));assert(!chromatic_button_actions(&s,0,80));
 assert(!chromatic_button_actions(&s,a,90));assert(chromatic_button_actions(&s,0,100)==CH_BUTTON_A);
 assert(!chromatic_button_actions(&s,b,110));assert(chromatic_button_actions(&s,0,120)==CH_BUTTON_B);
 assert(chromatic_button_actions(&s,a|b,130)==CH_BUTTON_VOICE);assert(!chromatic_button_actions(&s,0,140));
 assert(!chromatic_button_actions(&s,a,150));assert(!chromatic_button_actions(&s,0,5000200));
 puts("A+B chord is single-shot and consumes individual actions for both release orders");return 0;
}
