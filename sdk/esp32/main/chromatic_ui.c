/* Pixel UI for the ModRetro Chromatic's 160 x 144 RGB565 OSD. */
#include "chromatic.h"
#include "pixel_font.h"
#include <string.h>

#define W 160
#define H 144
static uint16_t *fb;
static int first_row, last_row;
static uint16_t color(unsigned r, unsigned g, unsigned b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}
static void box(int x, int y, int w, int h, uint16_t c) {
    int left=x<0?0:x, right=x+w>W?W:x+w;
    int top=y<first_row?first_row:y, bottom=y+h>last_row?last_row:y+h;
    for (int j=top; j<bottom; j++) for (int i=left; i<right; i++)
        fb[(j-first_row)*W+i]=c;
}
static void text(int x, int y, const char *s, uint16_t c) {
    for (; *s && x+5<W; s++, x+=6) {
        unsigned ch=(unsigned char)*s;
        if (ch < 32 || ch > 126) ch='?';
        for(int a=0;a<5;a++) for(int b=0;b<8;b++)
            if(pixel_font[ch-32][a] & (1<<b)) box(x+a,y+b,1,1,c);
    }
}
/* One wrapping routine is shared by drawing and page counting, so the last
 * page is reachable for long replies, newlines and unbroken words alike. */
static const char *next_line(const char *s, char line[CHROMATIC_TEXT_COLUMNS+1]) {
    while(*s==' ' || *s=='\n' || *s=='\r') s++;
    if(!*s) { line[0]=0; return s; }
    int n=0,brk=0;
    while(s[n] && s[n]!='\n' && s[n]!='\r' && n<CHROMATIC_TEXT_COLUMNS) {
        if(s[n]==' ') brk=n;
        n++;
    }
    if(n==CHROMATIC_TEXT_COLUMNS && s[n] && s[n]!=' ' && brk) n=brk;
    memcpy(line,s,n);line[n]=0;return s+n;
}
int chromatic_message_pages(const char *s) {
    int rows=0;char line[CHROMATIC_TEXT_COLUMNS+1];
    if(!s) return 1;
    while(*s) { s=next_line(s,line);if(line[0]) rows++; }
    return rows ? (rows+CHROMATIC_REPLY_ROWS-1)/CHROMATIC_REPLY_ROWS : 1;
}
static void paragraph(const char *s, int page, uint16_t c) {
    int row=0,skip=(page<0?0:page)*CHROMATIC_REPLY_ROWS;
    while(*s && row<skip+CHROMATIC_REPLY_ROWS) {
        char line[CHROMATIC_TEXT_COLUMNS+1];s=next_line(s,line);
        if(!line[0]) break;
        if(row>=skip) text(4,32+(row-skip)*9,line,c);
        row++;
    }
}
void chromatic_render(uint16_t *pixels, const chromatic_view_t *v) {
    chromatic_render_rows(pixels,v,0,H);
}
void chromatic_render_rows(uint16_t *pixels, const chromatic_view_t *v,
                           int start, int count) {
    if(!pixels || !v || start<0 || start>=H || count<=0 || count>H-start) return;
    fb=pixels;
    first_row=start; last_row=start+count;
    const uint16_t ink=color(224,243,228), dark=color(9,22,26);
    const uint16_t mint=color(111,247,172), dim=color(114,151,148);
    const uint16_t amber=color(248,183,83), blue=color(115,170,250);
    box(0,0,W,H,dark);
    box(0,0,W,28,color(19,47,49));
    const char *states[]={"BOOTING", "PAIR IN MUSE APP", "FIND ME IN MUSE", "PHONE CONNECTED",
        "PRESS A TO PAIR", "JOINING WIFI", "WIFI READY", "SIGNED IN", "SWITCHING MUSE",
        "MUSE READY", "MUSE CONNECTED", "RECONNECTING", "PAIR IN MUSE APP", "CHECK CONNECTION"};
    const char *status=v->listening ? "LISTENING" : v->busy ? "THINKING" : states[v->state>=0 && v->state<14 ? v->state : 0];
    text(36,5,status,v->state==13 ? amber : dim);
    /* A tiny cartridge creature: blinking eyes, moving antenna. */
    int bob=(v->frame/8)%2;
    box(5,8+bob,24,15,blue); box(9,5+bob,16,3,blue);
    box(16,2+bob,3,3,mint);
    int eye=(v->frame%90)>84 ? 1 : 4;
    box(10,11+bob,3,eye,dark); box(22,11+bob,3,eye,dark);
    box(15,19+bob,5,2,dark); box(6,24,6,2,mint); box(22,24,6,2,mint);
    text(36,16,v->title[0]?v->title:"YOUR POCKET MUSE",ink);
    if(v->message[0]) paragraph(v->message,v->page,ink);
    else {
        const char *items[]={"QUICK CHECK-IN", "NEXT BEST ACTION", "CREATIVE SPARK", "SURPRISE ME"};
        for(int i=0;i<4;i++) {
            if(i==v->selected) box(5,38+i*21,150,17,color(33,75,63));
            text(9,42+i*21,i==v->selected?">":" ",mint);
            text(21,42+i*21,items[i],ink);
        }
    }
    box(0,132,W,12,color(19,47,49));
    text(6,135,v->listening?"A+B: SEND  B: CANCEL":v->mic_ready?"A+B: TALK   A: QUICK":v->message[0]?"UP/DOWN: PAGE  B: BACK":"D-PAD: PICK   A: SEND",mint);
}
