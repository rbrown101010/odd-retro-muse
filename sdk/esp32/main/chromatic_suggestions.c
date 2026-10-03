/* Twenty compact follow-ups delivered by Muse with each screen reply. */
#include "chromatic.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

void chromatic_suggestions_default(chromatic_suggestions_t *out) {
    static const char *items[CHROMATIC_CHOICE_COUNT]={
        "Tell me more about the main point.", "Give me a shorter version.",
        "What should I do first?", "Turn this into a step-by-step plan.",
        "Make a checklist I can follow.", "Give me a concrete example.",
        "Explain it in simpler words.", "What is the biggest risk here?",
        "What am I overlooking?", "Suggest a different approach.",
        "Compare my best options.", "Help me make the decision.",
        "Draft a message about this.", "Turn this into a creative idea.",
        "How could I show this in a video?", "What can I finish in ten minutes?",
        "What is the best next action for me?", "Connect this to my current projects.",
        "What question should I ask next?", "Surprise me with a useful next step."
    };
    memset(out,0,sizeof(*out)); out->count=CHROMATIC_CHOICE_COUNT;
    for(int i=0;i<CHROMATIC_CHOICE_COUNT;i++) strcpy(out->items[i],items[i]);
}
bool chromatic_suggestions_parse(const char *encoded,chromatic_suggestions_t *out) {
    memset(out,0,sizeof(*out));
    if(!encoded || strlen(encoded)>2400) return false;
    cJSON *j=cJSON_Parse(encoded);
    bool good=cJSON_IsArray(j) && cJSON_GetArraySize(j)==CHROMATIC_CHOICE_COUNT;
    for(int i=0;good && i<CHROMATIC_CHOICE_COUNT;i++) {
        const char *s=cJSON_GetStringValue(cJSON_GetArrayItem(j,i));
        if(!s) { good=false; break; }
        while(*s==' ') s++;
        size_t n=strlen(s); while(n && s[n-1]==' ') n--;
        if(!n || n>CHROMATIC_CHOICE_BYTES) { good=false; break; }
        for(size_t k=0;k<n;k++) {
            unsigned char ch=(unsigned char)s[k];
            if(ch<32 || ch>126) { good=false; break; }
            out->items[i][k]=ch=='"'?'\'':ch=='\\'?'/':ch;
        }
        out->items[i][n]=0;
        for(int k=0;good && k<i;k++) if(!strcasecmp(out->items[i],out->items[k])) good=false;
    }
    cJSON_Delete(j);
    if(!good) { memset(out,0,sizeof(*out)); return false; }
    out->count=CHROMATIC_CHOICE_COUNT; return true;
}
char *chromatic_suggestions_json(const chromatic_suggestions_t *choices) {
    cJSON *j=cJSON_CreateArray();
    for(int i=0;i<choices->count && i<CHROMATIC_CHOICE_COUNT;i++)
        cJSON_AddItemToArray(j,cJSON_CreateString(choices->items[i]));
    char *json=cJSON_PrintUnformatted(j); cJSON_Delete(j); return json;
}
