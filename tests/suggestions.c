#include "chromatic.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *make(int count,int duplicate,int long_item) {
    cJSON *j=cJSON_CreateArray();
    for(int i=0;i<count;i++) {
        char s[100]; snprintf(s,sizeof(s),"Request number %d",duplicate?0:i);
        if(long_item && i==0) { memset(s,'x',45);s[45]=0; }
        cJSON_AddItemToArray(j,cJSON_CreateString(s));
    }
    char *out=cJSON_PrintUnformatted(j);cJSON_Delete(j);return out;
}
int main(void) {
    chromatic_suggestions_t choices,parsed;
    chromatic_suggestions_default(&choices);
    assert(choices.count==20);
    char *s=chromatic_suggestions_json(&choices);
    assert(chromatic_suggestions_parse(s,&parsed));
    assert(!memcmp(&parsed,&choices,sizeof(choices)));free(s);
    for(int count=19;count<=21;count++) {
        s=make(count,0,0);assert(chromatic_suggestions_parse(s,&parsed)==(count==20));free(s);
    }
    s=make(20,1,0);assert(!chromatic_suggestions_parse(s,&parsed));assert(parsed.count==0);free(s);
    s=make(20,0,1);assert(!chromatic_suggestions_parse(s,&parsed));free(s);
    assert(!chromatic_suggestions_parse(NULL,&parsed));
    assert(!chromatic_suggestions_parse("[null]",&parsed));
    s=make(20,0,0);cJSON *j=cJSON_Parse(s);free(s);
    cJSON_ReplaceItemInArray(j,0,cJSON_CreateString("Use the \\\"quoted\\\" path\\now"));
    s=cJSON_PrintUnformatted(j);assert(chromatic_suggestions_parse(s,&parsed));free(s);
    assert(strchr(parsed.items[0],'"')==NULL && strchr(parsed.items[0],'\\')==NULL);
    cJSON_ReplaceItemInArray(j,0,cJSON_CreateString("\xc3\xa9"));
    s=cJSON_PrintUnformatted(j);assert(!chromatic_suggestions_parse(s,&parsed));free(s);cJSON_Delete(j);
    // A maximum-size options event, nested as a JSON string in a wireless
    // response, must fit the encrypted transport without truncating items.
    memset(&choices,0,sizeof(choices));choices.count=20;
    for(int i=0;i<20;i++) { memset(choices.items[i],'X',44);choices.items[i][0]='A'+i; }
    s=chromatic_suggestions_json(&choices);assert(chromatic_suggestions_parse(s,&parsed));
    j=cJSON_CreateObject();cJSON_AddStringToObject(j,"text",s);free(s);
    s=cJSON_PrintUnformatted(j);assert(strlen(s)<1400);free(s);cJSON_Delete(j);
    puts("Twenty-options validation, distinctness, ASCII, bounds, and wireless serialization passed");
}
