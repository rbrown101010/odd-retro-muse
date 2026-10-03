/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Muse chat turns on boards without PSRAM (CONFIG_MUSE_HATCH=n). There's no room
 * for a second TLS and Noise connection or for MP3 decoding, so turns ride
 * Home Link's own session (muse_link_req_*) and replies are text only:
 *   talk    -> POST /chat/stream with the voice note, sent as it's recorded
 *   release -> GET /chat/history?limit=1 marks where the chat ends; the
 *              reply to the POST names the note's message id
 *   reply   -> history rows after the mark, one at a time, until the
 *              assistant's replies to the note; they scroll as captions
 * One row per request: a row is ~2 KB plus three copies of its text, and a
 * reply frame larger than Link's session buffers would end that session. Rows
 * are read in place as they arrive, without cJSON, which would need more RAM
 * than the row itself.
 */
#include "muse_chat.h"
#include "muse_chat_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_link.h"
#include "muse_wifi.h"

static const char *TAG = "muse_chat_link";

#define MIC_RATE 16000
#define STAGE_BYTES 1536                /* PCM per body chunk: 2 KB of base64, 48 ms */
#define CHUNK_BYTES (STAGE_BYTES / 3 * 4)
#define ACK_MAX 2048
#define ROW_MAX 12288                   /* a history page split across frames, gathered */
#define TEXT_MAX 1024                   /* reply text kept for the captions */
#define EV_TEXT 72
#define SEND_WAIT_MS 200               /* the press queues the pre-roll all at once */
#define POLL_US 500000                  /* between history polls while nothing's new */
#define SETTLE_US 3000000               /* quiet after a reply before the turn ends */
#define REPLY_TIMEOUT_US 60000000
#define READ_CHARS_PER_S 14             /* caption scroll, about speaking pace */


/* A reply being received on Link's session task, read by the voice task once done. */
typedef struct {
    uint32_t gen;       /* bumped per request; frames of older ones are dropped */
    int status;         /* HTTP status, -1 if the request died */
    bool done;
    bool overflow;
    size_t len;
    size_t cap;         /* fixed body (the note's ack); 0: history, gathered only if split */
    char *body;
} rx_t;

/* The one row of a history page. */
typedef struct {
    bool ok;            /* the page parsed */
    bool found;         /* it had a row */
    bool ready;         /* display_text_ready */
    uint64_t seq;
    char event[24];
    char msg[80];
    char reply_to[80];
    char text[TEXT_MAX];
} row_t;

enum { RX_NOTE, RX_ROW, RX_COUNT };

typedef enum { T_IDLE, T_TALKING, T_ACK, T_REPLY } phase_t;

typedef struct {
    char text[EV_TEXT];
    muse_hatch_ev_t type;
} ev_t;

static SemaphoreHandle_t s_rx_lock;
static rx_t s_rx[RX_COUNT];
static int64_t s_stream[RX_COUNT];      /* open request per slot, 0 none */
static QueueHandle_t s_events;
static row_t s_row;                     /* written with s_rx[RX_ROW], under s_rx_lock */

/* Voice task only. */
static struct {
    phase_t phase;
    uint8_t *stage;                     /* PCM waiting for the next chunk */
    size_t stage_len;
    char *chunk;
    bool marked;                        /* have the chat's last seq */
    uint64_t after;                     /* history seq read up to */
    char note_id[80];
    int64_t t_end, t_poll, t_reply, t_show;
    bool heard, replied, skipped_big;
    bool after_note;                    /* the walk is past the note's row, before any other user row */
    char error[EV_TEXT];                /* why the turn failed, repeated at the release */
    char text[TEXT_MAX];
    char shown[EV_TEXT];
} s_turn;

/* ---- History pages, read in place ---- */

typedef struct {
    const char *p, *end;
} scan_t;

static void skip_ws(scan_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) {
        s->p++;
    }
}

static bool expect(scan_t *s, char c)
{
    skip_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* Reads the string at s->p into out (may be NULL), cut to fit on a character boundary. */
static bool read_string(scan_t *s, char *out, size_t cap)
{
    if (s->p >= s->end || *s->p != '"') {
        return false;
    }
    size_t n = 0;
    for (s->p++; s->p < s->end && *s->p != '"'; s->p++) {
        char buf[4];
        size_t len = 1;
        buf[0] = *s->p;
        if (*s->p == '\\' && ++s->p < s->end) {
            switch (*s->p) {
            case 'n': buf[0] = '\n'; break;
            case 't': buf[0] = ' '; break;
            case 'r': buf[0] = ' '; break;
            case 'b': case 'f': buf[0] = ' '; break;
            case 'u': {
                if (s->end - s->p < 5) {
                    return false;
                }
                uint32_t c = hex4(s->p + 1);
                s->p += 4;
                if (c >= 0xD800 && c < 0xDC00 && s->end - s->p >= 7 && s->p[1] == '\\' && s->p[2] == 'u') {
                    c = 0x10000 + ((c - 0xD800) << 10) + (hex4(s->p + 3) - 0xDC00);
                    s->p += 6;
                }
                if (c < 0x80) {
                    buf[0] = (char)c;
                } else if (c < 0x800) {
                    buf[0] = (char)(0xC0 | c >> 6);
                    buf[1] = (char)(0x80 | (c & 63));
                    len = 2;
                } else if (c < 0x10000) {
                    buf[0] = (char)(0xE0 | c >> 12);
                    buf[1] = (char)(0x80 | (c >> 6 & 63));
                    buf[2] = (char)(0x80 | (c & 63));
                    len = 3;
                } else {
                    buf[0] = (char)(0xF0 | c >> 18);
                    buf[1] = (char)(0x80 | (c >> 12 & 63));
                    buf[2] = (char)(0x80 | (c >> 6 & 63));
                    buf[3] = (char)(0x80 | (c & 63));
                    len = 4;
                }
                break;
            }
            default: buf[0] = *s->p; break;   /* \" \\ \/ */
            }
        }
        if (out && n + len < cap) {
            memcpy(out + n, buf, len);
            n += len;
        }
    }
    if (out && cap) {
        while (n && (out[n - 1] & 0xC0) == 0x80 && s->p >= s->end) {
            n--;
        }
        out[n] = '\0';
    }
    if (s->p >= s->end) {
        return false;
    }
    s->p++;
    return true;
}

/* Steps over one value: a string, number, literal, or a whole object or array. */
static bool skip_value(scan_t *s)
{
    int depth = 0;
    skip_ws(s);
    while (s->p < s->end) {
        char c = *s->p;
        if (c == '"') {
            if (!read_string(s, NULL, 0)) {
                return false;
            }
        } else if (c == '{' || c == '[') {
            depth++;
            s->p++;
        } else if (c == '}' || c == ']') {
            if (!depth) {
                return true;
            }
            depth--;
            s->p++;
        } else if (c == ',' && !depth) {
            return true;
        } else {
            s->p++;
        }
        if (!depth && (c == '"' || c == '}' || c == ']')) {
            return true;
        }
    }
    return false;
}

/* Reads a string member's value into out; anything else (null) is skipped. */
static bool read_field(scan_t *s, char *out, size_t cap)
{
    skip_ws(s);
    return s->p < s->end && *s->p == '"' ? read_string(s, out, cap) : skip_value(s);
}

static bool parse_row(scan_t *s, row_t *r)
{
    if (!expect(s, '{')) {
        return false;
    }
    if (expect(s, '}')) {
        return true;
    }
    do {
        char key[24];
        skip_ws(s);
        if (!read_string(s, key, sizeof(key)) || !expect(s, ':')) {
            return false;
        }
        skip_ws(s);
        bool ok;
        if (!strcmp(key, "seq")) {
            for (r->seq = 0; s->p < s->end && *s->p >= '0' && *s->p <= '9'; s->p++) {
                r->seq = r->seq * 10 + (*s->p - '0');
            }
            ok = skip_value(s);
        } else if (!strcmp(key, "display_text_ready")) {
            r->ready = s->p < s->end && *s->p == 't';
            ok = skip_value(s);
        } else if (!strcmp(key, "event_name")) {
            ok = read_field(s, r->event, sizeof(r->event));
        } else if (!strcmp(key, "message_id")) {
            ok = read_field(s, r->msg, sizeof(r->msg));
        } else if (!strcmp(key, "reply_to_message_id")) {
            ok = read_field(s, r->reply_to, sizeof(r->reply_to));
        } else if (!strcmp(key, "display_text")) {
            ok = read_field(s, r->text, sizeof(r->text));
        } else {
            ok = skip_value(s);
        }
        if (!ok) {
            return false;
        }
    } while (expect(s, ','));
    return expect(s, '}');
}

static bool parse_object(scan_t *s, row_t *r);

/* {"ok":true,"result":{"chat_events":[row],...}}: fills r from the first row, if any. */
static bool parse_page(const char *p, size_t n, row_t *r)
{
    scan_t s = { p, p + n };
    memset(r, 0, sizeof(*r));
    return parse_object(&s, r);
}

static bool parse_object(scan_t *sp, row_t *r)
{
    scan_t s = *sp;
    if (!expect(&s, '{')) {
        return false;
    }
    if (expect(&s, '}')) {
        *sp = s;
        return true;
    }
    do {
        char key[24];
        skip_ws(&s);
        if (!read_string(&s, key, sizeof(key)) || !expect(&s, ':')) {
            return false;
        }
        if (!strcmp(key, "result")) {
            if (!parse_object(&s, r)) {
                return false;
            }
        } else if (strcmp(key, "chat_events")) {
            if (!skip_value(&s)) {
                return false;
            }
        } else if (expect(&s, '[') && !expect(&s, ']')) {
            if (!parse_row(&s, r)) {
                return false;
            }
            r->found = true;
            while (expect(&s, ',')) {
                if (!skip_value(&s)) {
                    return false;
                }
            }
            if (!expect(&s, ']')) {
                return false;
            }
        }
    } while (expect(&s, ','));
    if (!expect(&s, '}')) {
        return false;
    }
    *sp = s;
    return true;
}

/* A page in one frame is parsed straight from Link's buffer; a split one is gathered first. */
static void row_data(rx_t *rx, const uint8_t *data, size_t len, bool end)
{
    if (rx->status != 200 || rx->overflow) {
        return;
    }
    if (!rx->body && end) {
        s_row.ok = parse_page((const char *)data, len, &s_row);
        return;
    }
    if (len) {
        char *grown = rx->len + len <= ROW_MAX ? realloc(rx->body, rx->len + len) : NULL;
        if (!grown) {
            rx->overflow = true;
            return;
        }
        memcpy(grown + rx->len, data, len);
        rx->body = grown;
        rx->len += len;
    }
    if (end) {
        s_row.ok = parse_page(rx->body, rx->len, &s_row);
    }
}

static void rx_clear(rx_t *rx)
{
    if (!rx->cap) {
        free(rx->body);
        rx->body = NULL;
    }
    rx->status = 0;
    rx->done = rx->overflow = false;
    rx->len = 0;
}

static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    uintptr_t token = (uintptr_t)ctx;
    rx_t *rx = &s_rx[token & 1];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if ((uint32_t)(token >> 1) == rx->gen && !rx->done) {
        if (status) {
            rx->status = status;
        }
        if (!rx->cap) {
            row_data(rx, data, len, end && status >= 0);
        } else if (len && rx->len + len >= rx->cap) {
            rx->overflow = true;
        } else if (len) {
            memcpy(rx->body + rx->len, data, len);
            rx->len += len;
            rx->body[rx->len] = '\0';
        }
        rx->done = end || status < 0;
    }
    xSemaphoreGive(s_rx_lock);
}

/* Starts a request whose reply lands in slot `slot`. False if Link's session is down. */
static bool request(int slot, const char *verb, const char *path, bool json, bool end_body)
{
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              json ? "Content-Type" : NULL, "application/json", NULL };
    rx_t *rx = &s_rx[slot];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    rx->gen++;
    rx_clear(rx);
    if (rx->cap) {
        rx->body[0] = '\0';
    }
    s_row.ok = false;
    uintptr_t token = (uintptr_t)rx->gen << 1 | slot;
    xSemaphoreGive(s_rx_lock);
    s_stream[slot] = muse_link_req_open(verb, path, headers, end_body, on_frame, (void *)token);
    return s_stream[slot] != 0;
}

/* Drops slot `slot`'s request, if any, and whatever of its reply is still coming. */
static void drop(int slot)
{
    if (s_stream[slot]) {
        muse_link_req_cancel(s_stream[slot]);
        s_stream[slot] = 0;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx[slot].gen++;
    rx_clear(&s_rx[slot]);
    xSemaphoreGive(s_rx_lock);
}

/* True once slot `slot`'s reply is complete (or the request died). */
static bool received(int slot)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool done = s_stream[slot] && s_rx[slot].done;
    xSemaphoreGive(s_rx_lock);
    if (done) {
        s_stream[slot] = 0;
    }
    return done;
}

static void emit(muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type };
    strlcpy(ev.text, text ? text : "", sizeof(ev.text));
    xQueueSend(s_events, &ev, 0);
}

static void end_turn(void)
{
    drop(RX_NOTE);
    drop(RX_ROW);
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.phase = T_IDLE;
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    end_turn();
    strlcpy(s_turn.error, why, sizeof(s_turn.error));
    emit(MUSE_HATCH_EV_ERROR, why);
}

/* ---- The note ---- */

/* Sends the staged PCM as one body chunk; whole chunks are a multiple of 3 bytes, so only the last pads. */
static bool send_stage(bool last)
{
    size_t n = muse_hatch_base64(s_turn.stage, s_turn.stage_len, (char *)s_turn.chunk);
    if (last) {
        memcpy(s_turn.chunk + n, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        n += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    s_turn.stage_len = 0;
    return muse_link_req_send(s_stream[RX_NOTE], s_turn.chunk, n, last, SEND_WAIT_MS);
}

/* ---- The reply ---- */

/* The chat's newest row (none yet: the mark) or the next one after it. */
static bool poll_row(void)
{
    char path[64];
    if (s_turn.marked) {
        snprintf(path, sizeof(path), "/chat/history?limit=1&after_seq=%" PRIu64, s_turn.after);
    } else {
        strlcpy(path, "/chat/history?limit=1", sizeof(path));
    }
    return request(RX_ROW, "GET", path, false, true);
}

static void on_ack(void)
{
    rx_t *rx = &s_rx[RX_NOTE];
    if (rx->status != 200) {
        ESP_LOGW(TAG, "chat/stream: %d %.120s", rx->status, rx->body);
        fail(rx->status < 0 ? "LOST CONNECTION TO MUSE" : "MUSE DIDN'T TAKE IT");
        return;
    }
    cJSON *root = cJSON_Parse(rx->body);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "message_id"));
    strlcpy(s_turn.note_id, id ? id : "", sizeof(s_turn.note_id));
    cJSON_Delete(root);
    if (!s_turn.note_id[0]) {
        ESP_LOGW(TAG, "chat/stream ack without a message id: %.120s", rx->body);
        fail("MUSE DIDN'T TAKE IT");
        return;
    }
    ESP_LOGI(TAG, "note %s sent; ack after %.2fs", s_turn.note_id, (esp_timer_get_time() - s_turn.t_end) / 1e6);
    s_turn.phase = T_REPLY;
}

/* Handles the row just read; returns true to move past it. */
static bool on_row(const row_t *r)
{
    if (!strcmp(r->event, "message.user")) {
        s_turn.after_note = !strcmp(r->msg, s_turn.note_id);
    }
    if (s_turn.after_note && !strcmp(r->event, "message.user")) {
        /* The row reads "<transcript>\n[file:audio/wav ...]", or "[Voice note]" before transcription. */
        static char heard[TEXT_MAX];   /* voice task only */
        strlcpy(heard, r->text, sizeof(heard));
        char *att = strstr(heard, "\n[file:");
        if (att) {
            *att = '\0';
        }
        if (heard[0] && strcmp(heard, "[Voice note]") && !s_turn.heard) {
            char line[EV_TEXT];
            muse_hatch_tail_words(heard, line, sizeof(line));
            emit(MUSE_HATCH_EV_HEARD, line);
            s_turn.heard = true;
        }
        return true;
    }
    /* Replies to voice notes may leave reply_to_message_id empty: then it's whatever follows the note. */
    if (strcmp(r->event, "message.assistant")
        || (r->reply_to[0] ? strcmp(r->reply_to, s_turn.note_id) : !s_turn.after_note)) {
        return true;
    }
    if (!r->ready) {
        return false;   /* still being written; read it again */
    }
    if (r->text[0]) {
        size_t len = strlen(s_turn.text);
        snprintf(s_turn.text + len, sizeof(s_turn.text) - len, "%s%s", len ? " " : "", r->text);
        ESP_LOGI(TAG, "reply after %.2fs: %.80s", (esp_timer_get_time() - s_turn.t_end) / 1e6, r->text);
        if (!s_turn.replied) {
            s_turn.t_show = esp_timer_get_time();
        }
        s_turn.replied = true;
        s_turn.t_reply = esp_timer_get_time();
    }
    return true;
}

/* A page is in: note where the chat ends, or read the next row. */
static void on_page(void)
{
    rx_t *rx = &s_rx[RX_ROW];
    int64_t now = esp_timer_get_time();
    s_turn.t_poll = now + POLL_US;
    if (rx->status != 200) {
        ESP_LOGW(TAG, "chat/history: %d", rx->status);
        if (rx->status < 0) {
            fail("LOST CONNECTION TO MUSE");
        }
        return;
    }
    if (!s_row.ok) {
        ESP_LOGW(TAG, "chat/history page unreadable%s", rx->overflow ? ", too long" : "");
        s_turn.skipped_big |= rx->overflow;
        return;
    }
    if (!s_turn.marked) {
        s_turn.marked = true;
        s_turn.after = s_row.found ? s_row.seq : 0;
        s_turn.t_poll = now;
    } else if (s_row.found && s_row.seq > s_turn.after && on_row(&s_row)) {
        s_turn.after = s_row.seq;
        s_turn.t_poll = now;   /* there may be more */
    }
}

/* How far through the reply reading has got. */
static size_t read_at(int64_t now)
{
    return (size_t)((now - s_turn.t_show) * READ_CHARS_PER_S / 1000000);
}

/* Scrolls the reply through the caption at reading pace; true once it's all been shown. */
static bool scroll(int64_t now)
{
    size_t len = strlen(s_turn.text);
    size_t at = read_at(now);
    char line[EV_TEXT];
    if (muse_hatch_caption_at(s_turn.text, at < len ? at : len - 1, line, sizeof(line))
        && strcmp(line, s_turn.shown)) {
        strlcpy(s_turn.shown, line, sizeof(s_turn.shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
    return at >= len;
}

/* Moves the turn along; runs on the voice task each time it asks for events. */
static void pump(void)
{
    int64_t now = esp_timer_get_time();
    if (s_turn.phase == T_ACK && received(RX_NOTE)) {
        on_ack();
    }
    if (s_turn.phase != T_IDLE && received(RX_ROW)) {
        on_page();
    }
    if (s_turn.phase != T_REPLY) {
        if (s_turn.phase == T_ACK && now - s_turn.t_end > REPLY_TIMEOUT_US) {
            fail("NO REPLY FROM MUSE");
        }
        return;
    }
    if (!s_stream[RX_ROW] && now >= s_turn.t_poll && !poll_row()) {
        fail("LOST CONNECTION TO MUSE");
        return;
    }
    if (s_turn.replied) {
        if (scroll(now) && now - s_turn.t_reply > SETTLE_US) {
            ESP_LOGI(TAG, "reply done: %u chars", (unsigned)strlen(s_turn.text));
            end_turn();
            emit(MUSE_HATCH_EV_DONE, NULL);
        }
    } else if (now - s_turn.t_end > REPLY_TIMEOUT_US) {
        fail(s_turn.skipped_big ? "REPLY TOO LONG" : "NO REPLY FROM MUSE");
    }
}

/* ---- Public ---- */

void muse_hatch_start(void)
{
    s_rx_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(8, sizeof(ev_t));
    static char ack[ACK_MAX];
    s_rx[RX_NOTE].body = ack;
    s_rx[RX_NOTE].cap = sizeof(ack);
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!muse_link_hatch_linked()) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, "Pair in the Muse app", sizeof(out->detail));
    } else if (!muse_wifi_connected()) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (muse_link_req_ready()) {
        out->state = MUSE_HATCH_REACHABLE;
        strlcpy(out->detail, "Through Home Link, text replies", sizeof(out->detail));
    } else {
        out->state = MUSE_HATCH_TESTING;
        strlcpy(out->detail, "Connecting...", sizeof(out->detail));
    }
}

void muse_hatch_test(void)
{
}

void muse_hatch_config_changed(void)
{
}

void muse_hatch_set_resting(bool resting)
{
    (void)resting;   /* Link's session does the polling */
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Not set up";
    case MUSE_HATCH_OFFLINE: return "Offline";
    case MUSE_HATCH_UNTESTED: return "Saved";
    case MUSE_HATCH_TESTING: return "Connecting";
    case MUSE_HATCH_REACHABLE: return "Connected";
    case MUSE_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

bool muse_hatch_ready(void)
{
    return s_events && muse_link_hatch_linked() && muse_wifi_connected() && muse_link_req_ready();
}

void muse_hatch_turn_begin(void)
{
    end_turn();
    xQueueReset(s_events);
    memset(&s_turn, 0, sizeof(s_turn));
    s_turn.stage = malloc(STAGE_BYTES);
    s_turn.chunk = malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL));
    if (!s_turn.stage || !s_turn.chunk) {
        fail("OUT OF MEMORY");
        return;
    }
    if (!request(RX_NOTE, "POST", "/chat/stream", true, false)
        || !muse_link_req_send(s_stream[RX_NOTE], MUSE_HATCH_NOTE_HEAD, sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false, SEND_WAIT_MS)) {
        fail("CAN'T REACH MUSE");
        return;
    }
    muse_hatch_wav_header(s_turn.stage, MIC_RATE);
    s_turn.stage_len = MUSE_HATCH_WAV_HEADER;
    s_turn.phase = T_TALKING;
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    const uint8_t *p = (const uint8_t *)pcm;
    size_t n = frames * 2;
    while (s_turn.phase == T_TALKING && n) {
        size_t take = STAGE_BYTES - s_turn.stage_len < n ? STAGE_BYTES - s_turn.stage_len : n;
        memcpy(s_turn.stage + s_turn.stage_len, p, take);
        s_turn.stage_len += take;
        p += take;
        n -= take;
        if (s_turn.stage_len == STAGE_BYTES && !send_stage(false)) {
            fail("CAN'T KEEP UP");
        }
    }
}

void muse_hatch_turn_end(void)
{
    if (s_turn.phase != T_TALKING) {
        /* Failed while recording: that error went to the recording caption. */
        emit(MUSE_HATCH_EV_ERROR, s_turn.error[0] ? s_turn.error : "CAN'T REACH MUSE");
        return;
    }
    if (!send_stage(true)) {
        fail("CAN'T KEEP UP");
        return;
    }
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.t_end = esp_timer_get_time();
    s_turn.phase = T_ACK;
    /*
     * Only now mark where the chat ends: the newest row is often the last
     * reply, and reading it mid-upload leaves no room for TLS to send. Hatch
     * adds this note's row only after transcribing it, so none is missed.
     */
    if (!poll_row()) {
        fail("LOST CONNECTION TO MUSE");
    }
}

void muse_hatch_turn_cancel(void)
{
    end_turn();
    xQueueReset(s_events);
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    pump();
    ev_t ev;
    if (xQueueReceive(s_events, &ev, 0) != pdTRUE) {
        return MUSE_HATCH_EV_NONE;
    }
    strlcpy(text, ev.text, cap);
    return ev.type;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    (void)played;   /* no speech: the reply's page follows the reading pace */
    size_t len = strlen(s_turn.text);
    if (s_turn.phase != T_REPLY || !s_turn.replied || !len) {
        return false;
    }
    size_t at = read_at(esp_timer_get_time());
    return muse_hatch_caption_at(s_turn.text, at < len ? at : len - 1, out, cap);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    (void)pcm;
    (void)frames;
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1);
    }
    return 0;
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}
