#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*chromatic_button_cb)(void);
bool chromatic_init(void);
void chromatic_bind_pairing(chromatic_button_cb tap, chromatic_button_cb twice,
                            chromatic_button_cb hold);
void chromatic_message(const char *text);
void chromatic_chat_init(void);
bool chromatic_ask(const char *text);
bool chromatic_chat_busy(void);
#define CHROMATIC_CHOICE_COUNT 20
#define CHROMATIC_CHOICE_BYTES 44
typedef struct { int count; char items[CHROMATIC_CHOICE_COUNT][CHROMATIC_CHOICE_BYTES+1]; } chromatic_suggestions_t;
void chromatic_suggestions_default(chromatic_suggestions_t *out);
bool chromatic_suggestions_parse(const char *encoded, chromatic_suggestions_t *out);
char *chromatic_suggestions_json(const chromatic_suggestions_t *choices);
void chromatic_reply(const char *text, const chromatic_suggestions_t *choices);
bool chromatic_deliver_message(const char *text, const char *turn_id, const chromatic_suggestions_t *choices);
void chromatic_emit(const char *kind, const char *text);
void chromatic_set_voice(const char *state);
void chromatic_wireless_init(void);
bool chromatic_wireless_pair(const char *hex_key);
bool chromatic_wireless_configured(void);
void chromatic_wireless_publish(const char *kind, const char *text);
void chromatic_ip(char *out, size_t size);

#define CHROMATIC_REPLY_BYTES 2400
#define CHROMATIC_TEXT_COLUMNS 25
#define CHROMATIC_REPLY_ROWS 11
int chromatic_message_pages(const char *text);

/* Portable renderer also used by the host preview. */
typedef struct {
    int state, selected, frame, page, choice_selected;
    chromatic_suggestions_t choices;
    bool busy, mic_ready, listening;
    char title[27];
    char message[CHROMATIC_REPLY_BYTES+1];
} chromatic_view_t;
void chromatic_render(uint16_t *pixels, const chromatic_view_t *view);
void chromatic_render_rows(uint16_t *pixels, const chromatic_view_t *view,
                           int first_row, int row_count);
