/* ModRetro pins and packet/OSD format from oss-chromatic-console-mcu:
 * main/board.h, main/main.c, main/fpga_rx.c, components/button/button.h.
 * ESP32-U4WDH rev 3.1 / 4MB confirmed by esptool on this device.
 * No FPGA firmware or eFuses are modified by this port.
 */
#include "chromatic.h"
#include "chromatic_buttons.h"
#include "led_status.h"
#include "noise_control.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static chromatic_view_t view;
static bool voice_active;
static char voice_state[12];
static SemaphoreHandle_t lock;
static spi_device_handle_t panel;
/* Render eight rows at a time: a full framebuffer consumes 46 KB of scarce
 * internal RAM and prevents the encrypted Muse session from being allocated. */
static DMA_ATTR uint16_t pixels[160*8];
static chromatic_button_cb on_tap, on_twice, on_hold;
static uint16_t buttons;
static chromatic_buttons_t button_state;
static const char *prompts[]={
    "I'm on my Chromatic handheld. Give me a short personal check-in, using what you know about me. Keep it under 60 words.",
    "I'm on my Chromatic handheld. What is one useful next action for me right now? Use my context and keep it under 60 words.",
    "Give me one unexpected, specific creative idea I could make today. I'm reading on a tiny Chromatic screen. Under 60 words.",
    "Surprise me with something delightful and personal. I'm reading on a tiny Chromatic screen. Under 60 words."
};
static uint8_t crc(const uint8_t *p, size_t n) {
    uint8_t c=255;
    while(n--) { c^=*p++; for(int b=0;b<8;b++) c=(c&128)?(c<<1)^0x1d:c<<1; }
    return c;
}
void chromatic_bind_pairing(chromatic_button_cb tap, chromatic_button_cb twice,
                            chromatic_button_cb hold) {
    on_tap=tap; on_twice=twice; on_hold=hold;
}
void chromatic_message(const char *s) {
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY);
    strlcpy(view.message,s?s:"",sizeof(view.message)); view.page=0; view.choices.count=0; view.choice_selected=0;
    xSemaphoreGive(lock);
}
void chromatic_reply(const char *s,const chromatic_suggestions_t *choices) {
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY);
    strlcpy(view.message,s,sizeof(view.message)); view.page=0; view.choice_selected=0;
    view.choices=*choices;
    xSemaphoreGive(lock);
}
void chromatic_set_voice(const char *state) {
    if(!lock || !state) return;
    bool active=!strcmp(state,"listening") || !strcmp(state,"stopping");
    bool busy=chromatic_chat_busy();
    xSemaphoreTake(lock,portMAX_DELAY);
    if(!strcmp(voice_state,state)) { xSemaphoreGive(lock); return; }
    strlcpy(voice_state,state,sizeof(voice_state));
    view.mic_ready=!strcmp(state,"ready") || active;
    view.listening=!strcmp(state,"listening");
    if(view.listening) strlcpy(view.message,"Speak near your computer's microphone. Press A+B again to send. B cancels.",sizeof(view.message));
    else if(!strcmp(state,"stopping")) strlcpy(view.message,"Finishing your transcript...",sizeof(view.message));
    else if(voice_active && !active && !busy) view.message[0]=0;
    if(active) { view.page=0; view.choices.count=0; }
    voice_active=active;
    xSemaphoreGive(lock);
}
static void on_buttons(uint16_t b) {
    uint16_t rising=b & ~buttons;
    unsigned action=chromatic_button_actions(&button_state,b,esp_timer_get_time());
    if(action&CH_BUTTON_VOICE) {
        if(chromatic_chat_busy()) chromatic_message("Muse is still working. Wait for its reply before recording.");
        else if(!noise_ctrl_is_connected()) chromatic_message("Muse is offline. Wait for the connection to return.");
        else {
            chromatic_emit("voice_toggle","A+B");
            xSemaphoreTake(lock,portMAX_DELAY); bool ready=view.mic_ready; xSemaphoreGive(lock);
            if(!ready) chromatic_message("Open Odd Retro Muse on your Mac and enable the microphone. Then press A+B to talk.");
        }
    }
    if(action&CH_BUTTON_A) {
        int state, selection; char choice[CHROMATIC_CHOICE_BYTES+1]={0}; bool open_choices=false;
        xSemaphoreTake(lock,portMAX_DELAY); state=view.state; selection=view.selected; bool listening=view.listening;
        if(!listening && !chromatic_chat_busy() && view.message[0] && view.choices.count==CHROMATIC_CHOICE_COUNT) {
            int pages=chromatic_message_pages(view.message);
            if(view.page<pages) { view.page=pages; open_choices=true; }
            else strcpy(choice,view.choices.items[view.choice_selected]);
        }
        xSemaphoreGive(lock);
        if(!listening) {
            if(state==LED_STATE_PAIRING_CONFIRM_REQUIRED || state==LED_STATE_BLE_CONNECTED || state==LED_STATE_BLE_ADVERTISING) {
                if(on_tap) on_tap();
            } else if(open_choices) { /* A opens the options beneath this reply. */ }
            else if(noise_ctrl_is_connected()) chromatic_ask(choice[0]?choice:prompts[selection]);
            else chromatic_message("Pair in the Muse phone app first. Settings > Devices > Developer mode.");
        }
    }
    if(action&CH_BUTTON_B) chromatic_emit("voice_cancel","B");
    xSemaphoreTake(lock,portMAX_DELAY);
    if(action&CH_BUTTON_B) { view.message[0]=0; view.page=0; view.choices.count=0; }
    if(rising & ((1<<4)|(1<<6))) {
        if(view.message[0]) {
            int pages=chromatic_message_pages(view.message);
            if(view.choices.count && view.page>=pages && view.choice_selected>0) view.choice_selected--;
            else if(view.page>0) view.page--;
        }
        else view.selected=(view.selected+3)%4;
    }
    if(rising & ((1<<7)|(1<<5))) {
        if(view.message[0]) {
            int pages=chromatic_message_pages(view.message);
            if(view.choices.count && view.page>=pages) {
                if(view.choice_selected<view.choices.count-1) view.choice_selected++;
            } else if(view.page<pages-1 || view.choices.count) view.page++;
        }
        else view.selected=(view.selected+1)%4;
    }
    xSemaphoreGive(lock);
    buttons=b;
}
static void fpga_task(void *arg) {
    uint8_t msg[14]; size_t n=0, need=0;
    while(1) {
        uint8_t ch;
        if(uart_read_bytes(UART_NUM_1,&ch,1,pdMS_TO_TICKS(50))!=1) continue;
        if(!n) { if(ch==0x8f) { msg[n++]=ch; need=0; } continue; }
        msg[n++]=ch;
        if(n==3) {
            if(msg[2]>10) { n=0; continue; }
            need=4+msg[2];
        }
        if(need && n==need) {
            if(crc(msg,n)==0 && msg[1]==2 && msg[2]>=2)
                on_buttons(((uint16_t)msg[3]<<8)|msg[4]);
            n=0;
        }
    }
}
static void display_task(void *arg) {
    while(1) {
        xSemaphoreTake(lock,portMAX_DELAY);
        view.frame++; view.busy=chromatic_chat_busy();
        static chromatic_view_t snapshot;
        snapshot=view;
        xSemaphoreGive(lock);
        /* Same 1024-byte QSPI bursts as the stock OSD, with a synchronous
         * transfer so DMA never reads a stripe while it is redrawn. All stripes
         * share one state snapshot, preserving a consistent animation frame. */
        for(int row=0;row<144;row+=8) {
            chromatic_render_rows(pixels,&snapshot,row,8);
            for(int offset=0;offset<(int)sizeof(pixels);offset+=1024) {
                int bytes=sizeof(pixels)-offset; if(bytes>1024) bytes=1024;
                spi_transaction_t t={.cmd=0x7ff,.addr=row*160*2+offset,.length=bytes*8,
                    .tx_buffer=(uint8_t*)pixels+offset,.flags=SPI_TRANS_MODE_QIO};
                if(spi_device_transmit(panel,&t)!=ESP_OK) break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void console_task(void *arg) {
    char line[2200]; size_t n=0;
    while(1) {
        int ch=getchar();
        if(ch==EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if(ch=='\r'||ch=='\n') {
            line[n]=0; n=0;
            if(!strncmp(line,"@ask ",5)) {
                cJSON *j=cJSON_Parse(line+5);
                const char *s=cJSON_GetStringValue(cJSON_GetObjectItem(j,"text"));
                if(s) chromatic_ask(s);
                cJSON_Delete(j);
            } else if(!strncmp(line,"@voice ",7)) {
                cJSON *j=cJSON_Parse(line+7);
                const char *state=cJSON_GetStringValue(cJSON_GetObjectItem(j,"state"));
                chromatic_set_voice(state);
                cJSON_Delete(j);
            } else if(!strncmp(line,"@bridge ",8)) {
                cJSON *j=cJSON_Parse(line+8);
                bool ok=chromatic_wireless_pair(cJSON_GetStringValue(cJSON_GetObjectItem(j,"key")));
                printf("@bridge {\"ok\":%s}\n",ok?"true":"false");
                cJSON_Delete(j);
            } else if(!strcmp(line,"@status")) {
                char ip[16]; chromatic_ip(ip,sizeof(ip));
                printf("@status {\"board\":\"ModRetro Chromatic\",\"muse_connected\":%s,\"ip\":\"%s\",\"wireless_configured\":%s}\n",
                       noise_ctrl_is_connected()?"true":"false",ip,chromatic_wireless_configured()?"true":"false");
            }
        } else if(n<sizeof(line)-1) line[n++]=ch;
    }
}
bool chromatic_init(void) {
    if(lock) return true;
    lock=xSemaphoreCreateMutex(); if(!lock) return false;
    strlcpy(view.title,"YOUR POCKET MUSE",sizeof(view.title));
    const uart_config_t u={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1,1024,0,0,NULL,0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1,&u));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,9,10,-1,-1));
    spi_bus_config_t bus={.mosi_io_num=23,.miso_io_num=19,.sclk_io_num=18,
        .quadwp_io_num=22,.quadhd_io_num=21,.max_transfer_sz=1100,
        .data4_io_num=-1,.data5_io_num=-1,.data6_io_num=-1,.data7_io_num=-1,
        .flags=SPICOMMON_BUSFLAG_QUAD|SPICOMMON_BUSFLAG_MASTER};
    spi_device_interface_config_t dev={.clock_speed_hz=40000000,.mode=0,
        .spics_io_num=5,.queue_size=1,.flags=SPI_DEVICE_HALFDUPLEX,
        .cs_ena_pretrans=3,.cs_ena_posttrans=3,.command_bits=11,
        .address_bits=32,.dummy_bits=3};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST,&dev,&panel));
    chromatic_chat_init();
    xTaskCreate(fpga_task,"ch_buttons",3072,NULL,3,NULL);
    xTaskCreate(display_task,"ch_display",4096,NULL,2,NULL);
    xTaskCreate(console_task,"ch_console",6144,NULL,1,NULL);
    ESP_LOGI("link.led","LED status ready: Chromatic FPGA OSD 160x144");
    ESP_LOGI("link.chromatic","Press the side Menu button to show Muse; leave a cartridge inserted.");
    return true;
}
bool led_status_init(void) { return chromatic_init(); }
void led_status_set_state(led_state_t s) {
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY); view.state=s; xSemaphoreGive(lock);
}
void led_status_set_title(const char *s) {
    if(!lock) return;
    xSemaphoreTake(lock,portMAX_DELAY); strlcpy(view.title,s?s:"",sizeof(view.title)); xSemaphoreGive(lock);
}
bool led_status_display_info(int *w,int *h) { *w=160; *h=144; return true; }
int led_status_display_bits(void) { return 16; }
/* Custom text API instead of image decoding to preserve RAM on this device. */
bool led_status_draw_rect(int x,int y,int w,int h,const uint16_t *p) { return false; }
void led_status_draw_done(void) {}
void led_status_show_animation(void) { chromatic_message(""); }
void led_status_set_voice(led_voice_t s) {}
void led_status_set_level(float f) {}
void led_status_show_volume(int p) {}
