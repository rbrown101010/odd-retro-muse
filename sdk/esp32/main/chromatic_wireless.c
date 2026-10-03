/* Local computer bridge over AES-256-GCM authenticated UDP. The master key is
 * provisioned once over physical USB, never advertised or logged. Each new
 * connection receives a fresh device-generated session challenge; ordered
 * requests and cached duplicate responses prevent replay and double asks.
 * Muse's existing encrypted cloud connection is unchanged. */
#include "chromatic.h"
#include "noise_control.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 39077
#define CAP 3072
#define EVENTS 8
static SemaphoreHandle_t key_lock, events_lock;
static uint8_t key[32];
static bool configured;
static unsigned key_version;
static struct { uint32_t id; char *json; } events[EVENTS];
static unsigned count;
static uint32_t next_event;
static uint8_t packet[CAP+33], plain[CAP+1];
static char boot[33];

static void random_hex(char out[33]) {
    uint8_t bytes[16]; esp_fill_random(bytes,sizeof(bytes));
    for(int i=0;i<16;i++) sprintf(out+i*2,"%02x",bytes[i]);
}
void chromatic_ip(char *out,size_t size) {
    if(!key_lock) { snprintf(out,size,"0.0.0.0"); return; }
    esp_netif_ip_info_t info;
    esp_netif_t *iface=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(iface && esp_netif_get_ip_info(iface,&info)==ESP_OK)
        snprintf(out,size,IPSTR,IP2STR(&info.ip));
    else snprintf(out,size,"0.0.0.0");
}
bool chromatic_wireless_configured(void) {
    if(!key_lock) return false;
    xSemaphoreTake(key_lock,portMAX_DELAY); bool yes=configured; xSemaphoreGive(key_lock); return yes;
}
bool chromatic_wireless_pair(const char *hex) {
    if(!key_lock || !hex || strlen(hex)!=64) return false;
    uint8_t next[32];
    for(int i=0;i<32;i++) {
        if(!isxdigit((unsigned char)hex[i*2]) || !isxdigit((unsigned char)hex[i*2+1])) return false;
        char b[3]={hex[i*2],hex[i*2+1],0}, *end;
        unsigned long v=strtoul(b,&end,16); if(*end) return false; next[i]=v;
    }
    nvs_handle_t h;
    if(nvs_open("chromatic_net",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_blob(h,"key",next,sizeof(next));
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    if(err!=ESP_OK) return false;
    xSemaphoreTake(key_lock,portMAX_DELAY);
    memcpy(key,next,32); configured=true; key_version++;
    xSemaphoreGive(key_lock); return true;
}
static void clear_events(void) {
    xSemaphoreTake(events_lock,portMAX_DELAY);
    for(unsigned i=0;i<count;i++) free(events[i].json);
    count=0; xSemaphoreGive(events_lock);
}
void chromatic_wireless_publish(const char *kind,const char *text) {
    if(!events_lock || !chromatic_wireless_configured()) return;
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"type",kind); cJSON_AddStringToObject(j,"text",text);
    xSemaphoreTake(events_lock,portMAX_DELAY);
    cJSON_AddNumberToObject(j,"id",++next_event);
    char *s=cJSON_PrintUnformatted(j);
    if(s) {
        if(count==EVENTS) { free(events[0].json); memmove(events,events+1,(EVENTS-1)*sizeof(*events)); count--; }
        events[count].id=next_event; events[count++].json=s;
    }
    xSemaphoreGive(events_lock); cJSON_Delete(j);
}
static const char *str(cJSON *j,const char *k) {
    const char *s=cJSON_GetStringValue(cJSON_GetObjectItem(j,k)); return s?s:"";
}
static cJSON *make_status(void) {
    cJSON *j=cJSON_CreateObject(); char ip[16]; chromatic_ip(ip,sizeof(ip));
    cJSON_AddBoolToObject(j,"muse_connected",noise_ctrl_is_connected());
    cJSON_AddBoolToObject(j,"busy",chromatic_chat_busy());
    cJSON_AddStringToObject(j,"boot",boot); cJSON_AddStringToObject(j,"ip",ip);
    cJSON_AddNumberToObject(j,"free_heap",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return j;
}
static void add_event(cJSON *response,cJSON *request) {
    uint32_t ack=0;
    cJSON *n=cJSON_GetObjectItem(request,"ack");
    if(!strcmp(str(request,"boot"),boot) && cJSON_IsNumber(n) && n->valuedouble>=0 && n->valuedouble<=UINT32_MAX)
        ack=(uint32_t)n->valuedouble;
    xSemaphoreTake(events_lock,portMAX_DELAY);
    while(count && events[0].id<=ack) {
        free(events[0].json); memmove(events,events+1,(--count)*sizeof(*events));
    }
    if(count) { cJSON *event=cJSON_Parse(events[0].json); if(event) cJSON_AddItemToObject(response,"event",event); }
    xSemaphoreGive(events_lock);
}
static bool wire_crypt(bool decrypt,const uint8_t master[32],size_t n,const char *message,int fd,struct sockaddr_in *peer) {
    if(decrypt && (n<33 || memcmp(packet,"CMW1C",5))) return false;
    if(!decrypt && strlen(message)>CAP) return false;
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_ENCRYPT|PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes,PSA_ALG_GCM);
    psa_set_key_type(&attributes,PSA_KEY_TYPE_AES); psa_set_key_bits(&attributes,256);
    mbedtls_svc_key_id_t id=0;
    psa_status_t rc=psa_import_key(&attributes,master,32,&id);
    size_t written=0;
    if(decrypt) {
        if(rc==PSA_SUCCESS) rc=psa_aead_decrypt(id,PSA_ALG_GCM,packet+5,12,packet,5,
                                             packet+17,n-17,plain,CAP,&written);
        plain[written]=0;
    } else {
        size_t len=strlen(message);
        memcpy(packet,"CMW1D",5); esp_fill_random(packet+5,12);
        if(rc==PSA_SUCCESS) rc=psa_aead_encrypt(id,PSA_ALG_GCM,packet+5,12,packet,5,
                                             (const uint8_t*)message,len,packet+17,CAP+16,&written);
        if(rc==PSA_SUCCESS) sendto(fd,packet,written+17,0,(struct sockaddr*)peer,sizeof(*peer));
    }
    if(id) psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    return rc==PSA_SUCCESS;
}
static void network_task(void *arg) {
    char session[33]={0}, last_id[33]={0};
    uint32_t last_seq=0; unsigned version=0;
    char *cached=NULL; int64_t last_seen=0;
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_IP);
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};
    struct timeval timeout={.tv_sec=0,.tv_usec=500000};
    if(fd<0) { vTaskDelete(NULL); return; }
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(fd<0 || bind(fd,(struct sockaddr*)&local,sizeof(local))<0) { if(fd>=0) close(fd); vTaskDelete(NULL); return; }
    while(1) {
        if(session[0] && esp_timer_get_time()-last_seen>10000000) {
            session[0]=0; free(cached); cached=NULL; chromatic_set_voice("off");
        }
        struct sockaddr_in peer; socklen_t peer_len=sizeof(peer);
        int n=recvfrom(fd,packet,sizeof(packet),0,(struct sockaddr*)&peer,&peer_len);
        if(n<33) continue;
        uint8_t master[32]; bool ready; unsigned v;
        xSemaphoreTake(key_lock,portMAX_DELAY); memcpy(master,key,32); ready=configured; v=key_version; xSemaphoreGive(key_lock);
        if(v!=version) { version=v; session[0]=0; last_seq=0; free(cached); cached=NULL; }
        if(!ready || !wire_crypt(true,master,n,NULL,fd,&peer)) continue;
        cJSON *request=cJSON_Parse((char*)plain); if(!cJSON_IsObject(request)) { cJSON_Delete(request); continue; }
        const char *id=str(request,"id"), *op=str(request,"op");
        if(strlen(id)!=32) { cJSON_Delete(request); continue; }
        bool hello=!strcmp(op,"hello"), valid=false, duplicate=false;
        cJSON *seqj=cJSON_GetObjectItem(request,"seq");
        uint32_t seq=cJSON_IsNumber(seqj) && seqj->valuedouble>0 && seqj->valuedouble<UINT32_MAX ? (uint32_t)seqj->valuedouble : 0;
        if(hello) {
            duplicate=cached && !strcmp(id,last_id) && last_seq==0;
            if(!duplicate) { random_hex(session); last_seq=0; clear_events(); }
            valid=true;
        } else if(session[0] && !strcmp(str(request,"session"),session)) {
            duplicate=seq && seq==last_seq && cached && !strcmp(id,last_id);
            valid=seq>last_seq;
        }
        if(duplicate) { wire_crypt(false,master,0,cached,fd,&peer); cJSON_Delete(request); continue; }
        cJSON *response=cJSON_CreateObject(); cJSON_AddStringToObject(response,"id",id);
        bool ok=valid;
        const char *error="Session expired";
        if(valid) {
            last_seen=esp_timer_get_time();
            if(hello) cJSON_AddStringToObject(response,"session",session);
            else {
                last_seq=seq;
                if(!strcmp(op,"ask")) { ok=chromatic_ask(str(request,"text")); error="Muse is busy or the question is invalid"; }
                else if(!strcmp(op,"poll")) {
                    const char *state=str(request,"state");
                    if(!strcmp(state,"off") || !strcmp(state,"ready") || !strcmp(state,"listening") || !strcmp(state,"stopping")) chromatic_set_voice(state);
                } else { ok=false; error="Unknown wireless command"; }
            }
            cJSON_AddItemToObject(response,"status",make_status()); add_event(response,request);
        }
        cJSON_AddBoolToObject(response,"ok",ok);
        if(!ok) cJSON_AddStringToObject(response,"error",error);
        char *out=cJSON_PrintUnformatted(response); cJSON_Delete(response);
        if(out) {
            wire_crypt(false,master,0,out,fd,&peer);
            if(valid) { free(cached); cached=out; strlcpy(last_id,id,sizeof(last_id)); }
            else free(out);
        }
        cJSON_Delete(request);
    }
}
void chromatic_wireless_init(void) {
    key_lock=xSemaphoreCreateMutex(); events_lock=xSemaphoreCreateMutex();
    if(!key_lock || !events_lock) abort();
    if(psa_crypto_init()!=PSA_SUCCESS) abort();
    random_hex(boot);
    nvs_handle_t h; size_t size=sizeof(key);
    if(nvs_open("chromatic_net",NVS_READONLY,&h)==ESP_OK) {
        configured=nvs_get_blob(h,"key",key,&size)==ESP_OK && size==sizeof(key);
        nvs_close(h); if(configured) key_version=1;
    }
    if(xTaskCreate(network_task,"ch_wireless",6144,NULL,2,NULL)!=pdPASS) abort();
}
