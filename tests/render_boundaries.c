#include "chromatic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    uint16_t *p=malloc(160*144*2);
    uint16_t *striped=malloc(160*144*2);
    uint16_t stripe[160*8+2];
    chromatic_view_t v={0};
    for(int state=-3;state<18;state++) for(int n=0;n<=CHROMATIC_REPLY_BYTES;n+=17) {
        v.state=state; v.page=n%10; v.frame=n; v.selected=n%4;
        v.mic_ready=n%2; v.listening=n%3==0;
        memset(v.message,'Q',n);v.message[n]=0;
        for(int i=0;i<n;i+=23) v.message[i]=' ';
        chromatic_render(p,&v);
        for(int row=0;row<144;row+=8) {
            stripe[0]=0x1234;stripe[160*8+1]=0xabcd;
            chromatic_render_rows(stripe+1,&v,row,8);
            if(stripe[0]!=0x1234 || stripe[160*8+1]!=0xabcd) abort();
            memcpy(striped+row*160,stripe+1,160*8*2);
        }
        if(memcmp(p,striped,160*144*2)) abort();
    }
    v.choices.count=20;
    for(int i=0;i<20;i++) { memset(v.choices.items[i],'A'+i,44);v.choices.items[i][44]=0; }
    for(int selected=-1;selected<=20;selected++) {
        v.choice_selected=selected;v.page=chromatic_message_pages(v.message);
        chromatic_render(p,&v);
        for(int row=0;row<144;row+=8) {
            stripe[0]=0x1234;stripe[160*8+1]=0xabcd;
            chromatic_render_rows(stripe+1,&v,row,8);
            if(stripe[0]!=0x1234 || stripe[160*8+1]!=0xabcd) abort();
            memcpy(striped+row*160,stripe+1,160*8*2);
        }
        if(memcmp(p,striped,160*144*2)) abort();
    }
    if(chromatic_message_pages("")!=1 || chromatic_message_pages(NULL)!=1) abort();
    memset(v.message,'X',275);v.message[275]=0;
    if(chromatic_message_pages(v.message)!=1) abort();
    v.message[275]='X';v.message[276]=0;
    if(chromatic_message_pages(v.message)!=2) abort();
    memset(v.message,'X',CHROMATIC_REPLY_BYTES);v.message[CHROMATIC_REPLY_BYTES]=0;
    if(chromatic_message_pages(v.message)!=9) abort();
    stripe[0]=0x1234;
    chromatic_render_rows(stripe,&v,-1,8);
    chromatic_render_rows(stripe,&v,144,8);
    chromatic_render_rows(stripe,&v,143,8);
    chromatic_render_rows(stripe,&v,0,0);
    if(stripe[0]!=0x1234) abort();
    free(striped);free(p);
    puts("Full-frame and striped rendering match; boundaries passed under ASan and UBSan");return 0;
}
