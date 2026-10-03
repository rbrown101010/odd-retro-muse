/* Typed Muse turns over the SDK's existing encrypted control session.
 * Send using the supported gadget route from linux/src/musegadget/link_client.py.
 * Replies return through the advertised display.message device command.
 */
#include "chromatic.h"
#include "noise_control.h"
#include "identity.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static QueueHandle_t asks;
static SemaphoreHandle_t rx_lock;
static atomic_bool busy;
static uint32_t generation;
static struct { int status; bool done, overflow; size_t len; char data[4096]; } rx;
static char active_turn[32];
static bool reply_received;
static int last_status;
typedef struct { char text[1024]; } ask_t;
void chromatic_emit(const char *kind, const char *s) {
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"type",kind); cJSON_AddStringToObject(j,"text",s);
    char *out=cJSON_PrintUnformatted(j);
    if(out) { printf("@muse %s\n",out); free(out); }
    cJSON_Delete(j);
    chromatic_wireless_publish(kind,s);
}
#define emit chromatic_emit
static void received(void *ctx,int status,const uint8_t *p,size_t n,bool end) {
    xSemaphoreTake(rx_lock,portMAX_DELAY);
    if((uintptr_t)ctx==generation && !rx.done) {
        if(status) rx.status=status;
        if(n>=sizeof(rx.data)-rx.len) rx.overflow=true;
        else if(n && !rx.overflow) { memcpy(rx.data+rx.len,p,n); rx.len+=n; rx.data[rx.len]=0; }
        if(end || status<0) rx.done=true;
    }
    xSemaphoreGive(rx_lock);
}
/* One request at a time; callbacks from cancelled requests cannot overwrite
 * later replies. The caller owns the parsed object returned here. */
static cJSON *request(const char *verb,const char *path,const char *body) {
    last_status=0;
    xSemaphoreTake(rx_lock,portMAX_DELAY);
    generation++; if(!generation) generation++;
    memset(&rx,0,sizeof(rx));
    uint32_t gen=generation;
    xSemaphoreGive(rx_lock);
    char id[40]; snprintf(id,sizeof(id),"chromatic-%08" PRIx32,esp_random());
    const char *headers[]={"x-request-id",id,"x-app-id","musegadget",
        body?"Content-Type":NULL,"application/json",NULL};
    int64_t stream=noise_ctrl_req_open(verb,path,headers,!body,received,(void*)(uintptr_t)gen);
    if(!stream) return NULL;
    if(body && !noise_ctrl_req_send(stream,body,strlen(body),true,1000)) {
        noise_ctrl_req_cancel(stream); return NULL;
    }
    int64_t deadline=esp_timer_get_time()+20000000;
    while(esp_timer_get_time()<deadline) {
        xSemaphoreTake(rx_lock,portMAX_DELAY);
        bool done=rx.done;
        last_status=rx.status;
        cJSON *j=done && rx.status>=200 && rx.status<300 && !rx.overflow ? cJSON_Parse(rx.data) : NULL;
        if(done) generation++;
        xSemaphoreGive(rx_lock);
        if(done) return j;
        if(!noise_ctrl_is_connected()) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreTake(rx_lock,portMAX_DELAY); generation++; xSemaphoreGive(rx_lock);
    noise_ctrl_req_cancel(stream); return NULL;
}
static cJSON *result(cJSON *j) {
    cJSON *r=cJSON_GetObjectItem(j,"result"); return cJSON_IsObject(r)?r:j;
}
static const char *field(cJSON *j,const char *key) {
    const char *s=cJSON_GetStringValue(cJSON_GetObjectItem(j,key)); return s?s:"";
}
bool chromatic_deliver_message(const char *s,const char *turn_id) {
    if(!rx_lock || !s || !*s || strlen(s)>CHROMATIC_REPLY_BYTES) return false;
    xSemaphoreTake(rx_lock,portMAX_DELAY);
    bool reply=active_turn[0]!=0;
    bool valid=reply ? turn_id && !strcmp(turn_id,active_turn) : !turn_id || !*turn_id;
    if(valid && reply) reply_received=true;
    xSemaphoreGive(rx_lock);
    if(!valid) return false;
    chromatic_message(s); emit(reply?"reply":"push",s);
    return true;
}
static void worker(void *arg) {
    ask_t ask;
    while(xQueueReceive(asks,&ask,portMAX_DELAY)) {
        if(!noise_ctrl_is_connected()) { chromatic_message("Muse is offline. Pair in the Muse phone app and connect Wi-Fi."); emit("error","Muse is offline"); atomic_store(&busy,false); continue; }
        char turn[32]; snprintf(turn,sizeof(turn),"chromatic-%08" PRIx32,esp_random());
        xSemaphoreTake(rx_lock,portMAX_DELAY);
        strlcpy(active_turn,turn,sizeof(active_turn)); reply_received=false;
        xSemaphoreGive(rx_lock);
        chromatic_message("Sending to Muse..."); emit("busy","Muse is thinking");
        char message[1700];
        snprintf(message,sizeof(message),"%s\n\n[From the Chromatic handheld. Deliver your reply to this device using its display.message command, with text in plain ASCII up to 2400 bytes; give a complete useful answer, concise when appropriate and turn_id exactly %s. Reply via the device command so the user can read it on the handheld.]",ask.text,turn);
        cJSON *body=cJSON_CreateObject(); cJSON_AddStringToObject(body,"message",message);
        cJSON_AddStringToObject(body,"device_id",identity_node_id());
        char *json=cJSON_PrintUnformatted(body); cJSON_Delete(body);
        cJSON *ack=json?request("POST","/chat/stream",json):NULL; free(json);
        char note[96]; strlcpy(note,field(result(ack),"message_id"),sizeof(note)); cJSON_Delete(ack);
        xSemaphoreTake(rx_lock,portMAX_DELAY); bool responded=reply_received; xSemaphoreGive(rx_lock);
        if(!note[0] && !responded) {
            xSemaphoreTake(rx_lock,portMAX_DELAY); active_turn[0]=0; xSemaphoreGive(rx_lock);
            char error[160]; snprintf(error,sizeof(error),"Message not confirmed (HTTP %d). Check Muse before retrying.",last_status);
            chromatic_message(error); emit("error",error); atomic_store(&busy,false); continue;
        }
        if(!responded) chromatic_message("Sent to Muse. Waiting for screen reply...");
        emit("sent",ask.text);
        bool found=false;
        int64_t deadline=esp_timer_get_time()+120000000;
        while(esp_timer_get_time()<deadline && noise_ctrl_is_connected()) {
            xSemaphoreTake(rx_lock,portMAX_DELAY); found=reply_received; xSemaphoreGive(rx_lock);
            if(found) break;
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        xSemaphoreTake(rx_lock,portMAX_DELAY); active_turn[0]=0; xSemaphoreGive(rx_lock);
        if(!found) { chromatic_message("No reply arrived yet. Open your Muse chat to check it; your message may still be running."); emit("error","Reply timed out; check Muse chat"); }
        atomic_store(&busy,false);
    }
}
bool chromatic_chat_busy(void) { return atomic_load(&busy); }
bool chromatic_ask(const char *s) {
    if(!asks || !s || !*s || strlen(s)>=1024) return false;
    if(atomic_exchange(&busy,true)) { emit("error","Muse is already working"); return false; }
    ask_t ask; strlcpy(ask.text,s,sizeof(ask.text));
    if(xQueueSend(asks,&ask,0)!=pdTRUE) { atomic_store(&busy,false); return false; }
    return true;
}
void chromatic_chat_init(void) {
    rx_lock=xSemaphoreCreateMutex(); asks=xQueueCreate(1,sizeof(ask_t));
    if(!rx_lock || !asks) abort();
    if(xTaskCreate(worker,"ch_chat",8192,NULL,2,NULL)!=pdPASS) abort();
}
