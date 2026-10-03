/*
 * Copyright (c) 2026 muse-gadget-xiaozhi contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The muse_hatch_* API (muse_chat.h) backed by the xiaozhi.me voice service
 * (github.com/78/xiaozhi-esp32) instead of Muse's Hatch VM, so the Muse UI,
 * avatar and push-to-talk loop run unchanged:
 *
 *   OTA check   POST <ota url> -> {"websocket":{url,token,version}} or an
 *               {"activation":{code}} to bind the device at xiaozhi.me first
 *   session     WebSocket, "hello" both ways, then per press:
 *               listen start (manual) -> 60 ms Opus frames -> listen stop
 *   reply       stt (transcript), tts start / sentence_start (text) / stop,
 *               Opus frames decoded straight to 16 kHz
 *
 * Every incoming frame, text or audio, goes through one message buffer, so a
 * sentence's caption is stamped with where its speech starts in the reply.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "muse_audio.h"
#include "muse_chat_priv.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_wifi.h"

static const char *TAG = "xiaozhi";

#define DEFAULT_OTA_URL CONFIG_MUSE_XIAOZHI_OTA_URL
#define LANGUAGE CONFIG_MUSE_XIAOZHI_LANGUAGE
#define FRAME_MS 60
#define ENC_FRAMES (MUSE_AUDIO_RATE * FRAME_MS / 1000)    /* 960 */
#define DEC_FRAMES_MAX (MUSE_AUDIO_RATE * 120 / 1000)     /* longest Opus frame */
#define IN_BYTES (256 * 1024)          /* ~8 s of speech while the session opens */
#define OUT_BYTES (128 * 1024)         /* ~4 s of reply ahead of the speaker */
#define RX_BYTES (96 * 1024)
#define FRAG_MAX 8192
#define EV_TEXT 512
#define REPLY_MAX 4096
#define SENTENCES_MAX 48
#define HELLO_WAIT_MS 10000
#define REPLY_WAIT_MS 30000            /* release -> first word of the reply */
#define SPEECH_CHARS_PER_S 15          /* caption pace before a sentence's end is known */

/* ---- What the voice task and the UI see ---- */

typedef enum { CMD_BEGIN, CMD_END, CMD_CANCEL, CMD_CONNECT, CMD_FORGET } cmd_kind_t;
typedef struct {
    cmd_kind_t kind;
    uint32_t gen;
} cmd_t;
typedef struct {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
} ev_t;

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static MessageBufferHandle_t s_rx;
static _Atomic uint32_t s_gen;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_hatch_state_t s_result = MUSE_HATCH_UNTESTED;
static char s_detail[48];
static char s_code[16];                 /* activation code while the device isn't bound */
static volatile bool s_have_config;

/* Reply text and where each sentence's speech starts, for captions. */
static SemaphoreHandle_t s_cap_lock;
static char *s_reply;
static char *s_sentence;                /* caption scratch, under s_cap_lock */
static struct {
    uint32_t start;                     /* reply frames decoded before it */
    uint16_t off;                       /* into s_reply */
} s_sent[SENTENCES_MAX];
static int s_nsent;
static uint32_t s_decoded;              /* reply frames this turn */
static bool s_reply_done;

/* ---- Session task state ---- */

static char s_ota_url[MUSE_HOST_MAX + 1];
static char s_ws_url[160], s_ws_token[160];
static int s_ws_version = 1;
static char s_device_id[18], s_client_id[37];
static char s_session_id[64];
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_up;
static bool s_hello;
static void *s_enc, *s_dec;
static int16_t *s_pcm;                  /* ENC_FRAMES in, DEC_FRAMES_MAX out */
static uint8_t *s_pkt;                  /* encoder output, rx item scratch */
static uint8_t *s_frag;                 /* a frame split over several ws events */
static int s_frag_len;

typedef enum { T_IDLE, T_RECORDING, T_ENDING, T_WAITING, T_SPEAKING } phase_t;
static struct {
    phase_t phase;
    uint32_t gen;
    bool listening;                     /* "listen start" sent */
    int64_t deadline_us;
} s_turn;

static int64_t s_next_ota_us;
static int s_ota_backoff_s = 5;

static void report(muse_hatch_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_result = state;
    strlcpy(s_detail, detail, sizeof(s_detail));
    portEXIT_CRITICAL(&s_lock);
}

static void post_ev(muse_hatch_ev_t type, uint32_t gen, const char *text)
{
    static ev_t ev;   /* session task only */
    ev.type = type;
    ev.gen = gen;
    strlcpy(ev.text, text ? text : "", sizeof(ev.text));
    if (xQueueSend(s_events, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %d", type);
    }
}

static bool current(void)
{
    return s_turn.phase != T_IDLE && s_turn.gen == atomic_load(&s_gen);
}

/* ---- OTA check: where the session lives, or the activation code ---- */

static void ota_url(char out[MUSE_HOST_MAX + 1])
{
    muse_settings_hatch_host(out);
    if (strncmp(out, "http", 4) != 0) {   /* unset, or a Muse host left in NVS */
        strlcpy(out, DEFAULT_OTA_URL, MUSE_HOST_MAX + 1);
    }
}

/* POSTs body to url; returns the HTTP status (or -1) and the reply in *out (malloc'd). */
static int http_post(const char *url, const char *body, char **out)
{
    *out = NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return -1;
    }
    char ua[64];
    snprintf(ua, sizeof(ua), "muse-gadget-xiaozhi/%s", esp_app_get_description()->version);
    esp_http_client_set_header(h, "Activation-Version", "1");
    esp_http_client_set_header(h, "Device-Id", s_device_id);
    esp_http_client_set_header(h, "Client-Id", s_client_id);
    esp_http_client_set_header(h, "User-Agent", ua);
    esp_http_client_set_header(h, "Accept-Language", LANGUAGE);
    esp_http_client_set_header(h, "Content-Type", "application/json");

    int len = strlen(body), status = -1;
    if (esp_http_client_open(h, len) == ESP_OK && esp_http_client_write(h, body, len) == len &&
        esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
        size_t cap = 4096, n = 0;
        char *buf = heap_caps_malloc(cap, MUSE_BIG_CAPS);
        int r;
        while (buf && n < cap - 1 && (r = esp_http_client_read(h, buf + n, cap - 1 - n)) > 0) {
            n += r;
        }
        if (buf) {
            buf[n] = 0;
            *out = buf;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

static void copy_str(const cJSON *obj, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) {
        strlcpy(out, v->valuestring, cap);
    }
}

static void ota_check(void)
{
    ota_url(s_ota_url);
    const esp_app_desc_t *app = esp_app_get_description();
    char body[512];
    snprintf(body, sizeof(body),
             "{\"version\":2,\"language\":\"%s\",\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"chip_model_name\":\"%s\",\"application\":{\"name\":\"muse-gadget-xiaozhi\",\"version\":\"%s\","
             "\"idf_version\":\"%s\"},\"ota\":{\"label\":\"ota_0\"},"
             "\"board\":{\"type\":\"%s\",\"name\":\"muse-gadget-xiaozhi\"}}",
             LANGUAGE, s_device_id, s_client_id, CONFIG_IDF_TARGET, app->version, app->idf_ver,
             CONFIG_MUSE_BOARD_ID);
    if (!s_have_config && !s_code[0]) {
        report(MUSE_HATCH_TESTING, "Checking xiaozhi...");
    }
    char *reply;
    int status = http_post(s_ota_url, body, &reply);
    cJSON *root = status == 200 && reply ? cJSON_Parse(reply) : NULL;
    free(reply);
    if (!root) {
        char detail[48];
        snprintf(detail, sizeof(detail), status < 0 ? "Server unreachable" : "Server said %d", status);
        ESP_LOGW(TAG, "OTA check %s: %s", s_ota_url, detail);
        report(MUSE_HATCH_UNREACHABLE, detail);
        s_next_ota_us = esp_timer_get_time() + (int64_t)s_ota_backoff_s * 1000000;
        s_ota_backoff_s = s_ota_backoff_s < 300 ? s_ota_backoff_s * 2 : 300;
        return;
    }
    s_ota_backoff_s = 5;

    const cJSON *act = cJSON_GetObjectItem(root, "activation");
    const cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    char code[16] = "";
    if (cJSON_IsObject(act)) {
        copy_str(act, "code", code, sizeof(code));
        if (cJSON_IsString(cJSON_GetObjectItem(act, "challenge"))) {
            /* No efuse serial to sign with: an empty body just asks if it's bound yet. */
            char url[MUSE_HOST_MAX + 16], *r;
            snprintf(url, sizeof(url), "%s%sactivate", s_ota_url,
                     s_ota_url[strlen(s_ota_url) - 1] == '/' ? "" : "/");
            http_post(url, "{}", &r);
            free(r);
        }
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_code, code, sizeof(s_code));
    portEXIT_CRITICAL(&s_lock);

    if (code[0]) {
        ESP_LOGI(TAG, "not bound yet: activation code %s", code);
        s_have_config = false;
        s_next_ota_us = esp_timer_get_time() + 5 * 1000000;   /* until it's entered at xiaozhi.me */
    } else if (cJSON_IsObject(ws)) {
        copy_str(ws, "url", s_ws_url, sizeof(s_ws_url));
        copy_str(ws, "token", s_ws_token, sizeof(s_ws_token));
        const cJSON *v = cJSON_GetObjectItem(ws, "version");
        s_ws_version = cJSON_IsNumber(v) && v->valueint ? v->valueint : 1;
        s_have_config = s_ws_url[0] != 0;
        ESP_LOGI(TAG, "session at %s (protocol v%d)", s_ws_url, s_ws_version);
        report(MUSE_HATCH_UNTESTED, "");
    } else {
        ESP_LOGW(TAG, "OTA reply has no websocket section");
        report(MUSE_HATCH_UNREACHABLE, "No WebSocket offered");
        s_next_ota_us = esp_timer_get_time() + 60 * 1000000;
    }
    cJSON_Delete(root);
}

/* ---- WebSocket session ---- */

/* Runs on the client's task: copies each whole frame, tagged 'T'ext or
 * 'B'inary, into s_rx; 'X' says the session went away. */
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_up = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (s_ws_up) {
            s_ws_up = false;
            uint8_t x = 'X';
            xMessageBufferSend(s_rx, &x, 1, 0);
        }
        break;
    case WEBSOCKET_EVENT_DATA: {
        uint8_t kind = d->op_code == 0x1 ? 'T' : d->op_code == 0x2 ? 'B' : 0;
        if (d->payload_offset == 0) {
            s_frag_len = 0;
            if (!kind) {
                break;   /* ping, pong, close */
            }
            s_frag[0] = kind;
            s_frag_len = 1;
        }
        if (!s_frag_len || s_frag_len + d->data_len >= FRAG_MAX) {
            s_frag_len = 0;
            break;
        }
        memcpy(s_frag + s_frag_len, d->data_ptr, d->data_len);
        s_frag_len += d->data_len;
        if (d->payload_offset + d->data_len >= d->payload_len) {
            if (xMessageBufferSend(s_rx, s_frag, s_frag_len, pdMS_TO_TICKS(200)) == 0) {
                ESP_LOGW(TAG, "rx backlog full, dropped a frame");
            }
            s_frag_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

static void ws_close(void)
{
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_up = false;
    s_hello = false;
}

static bool ws_send_text(const char *json)
{
    return s_ws && s_ws_up && esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(2000)) >= 0;
}

/* {"session_id":"..","type":..} with the rest of the members after it. */
static void send_msg(const char *members)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"session_id\":\"%s\",%s}", s_session_id, members);
    ws_send_text(buf);
}

/* One Opus packet, framed for the protocol version the server asked for. */
static bool ws_send_audio(const uint8_t *pkt, int len)
{
    static uint8_t frame[16 + 1500];
    int head = 0;
    if (s_ws_version == 2) {
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
        memset(frame, 0, 16);
        frame[1] = 2;   /* version; type 0 = Opus */
        frame[8] = ts >> 24, frame[9] = ts >> 16, frame[10] = ts >> 8, frame[11] = ts;
        frame[12] = len >> 24, frame[13] = len >> 16, frame[14] = len >> 8, frame[15] = len;
        head = 16;
    } else if (s_ws_version == 3) {
        frame[0] = 0, frame[1] = 0, frame[2] = len >> 8, frame[3] = len;
        head = 4;
    }
    if (len > (int)sizeof(frame) - head) {
        return false;
    }
    memcpy(frame + head, pkt, len);
    return s_ws && s_ws_up &&
           esp_websocket_client_send_bin(s_ws, (const char *)frame, head + len, pdMS_TO_TICKS(2000)) >= 0;
}

static void handle_rx(uint8_t *item, size_t len);

/* Opens the session and waits for the server's hello. */
static bool ws_open(void)
{
    if (s_ws && s_ws_up && s_hello) {
        return true;
    }
    ws_close();
    xMessageBufferReset(s_rx);
    report(MUSE_HATCH_TESTING, "Connecting...");

    char headers[384];
    snprintf(headers, sizeof(headers), "Authorization: %s%s\r\nProtocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             strchr(s_ws_token, ' ') ? "" : "Bearer ", s_ws_token, s_ws_version, s_device_id, s_client_id);
    esp_websocket_client_config_t cfg = {
        .uri = s_ws_url,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .buffer_size = 2048,
        .task_stack = 6144,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 30,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        report(MUSE_HATCH_UNREACHABLE, "Out of memory");
        return false;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ws_close();
        report(MUSE_HATCH_UNREACHABLE, "Can't start session");
        return false;
    }
    int64_t until = esp_timer_get_time() + HELLO_WAIT_MS * 1000LL;
    while (!s_ws_up && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_ws_up) {
        ws_close();
        report(MUSE_HATCH_UNREACHABLE, "Session refused");
        s_have_config = false;   /* the token may be stale: ask the OTA check again */
        s_next_ota_us = 0;
        return false;
    }
    ws_send_text("{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},\"transport\":\"websocket\","
                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
    while (!s_hello && s_ws_up && esp_timer_get_time() < until) {
        size_t n = xMessageBufferReceive(s_rx, s_pkt, FRAG_MAX, pdMS_TO_TICKS(50));
        if (n) {
            handle_rx(s_pkt, n);
        }
    }
    if (!s_hello) {
        ws_close();
        report(MUSE_HATCH_UNREACHABLE, "No hello from server");
        return false;
    }
    report(MUSE_HATCH_REACHABLE, "Connected");
    return true;
}

/* ---- Speech out, reply in ---- */

static void reset_reply(void)
{
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_reply[0] = 0;
    s_nsent = 0;
    s_decoded = 0;
    s_reply_done = false;
    xSemaphoreGive(s_cap_lock);
}

static void add_sentence(const char *text)
{
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    size_t at = strlen(s_reply);
    if (s_nsent < SENTENCES_MAX && at + 2 < REPLY_MAX) {
        if (at) {
            s_reply[at++] = ' ';
        }
        s_sent[s_nsent].start = s_decoded;
        s_sent[s_nsent].off = at;
        s_nsent++;
        strlcpy(s_reply + at, text, REPLY_MAX - at);
    }
    xSemaphoreGive(s_cap_lock);
}

static void play_packet(const uint8_t *pkt, int len)
{
    if (!s_dec || (s_turn.phase != T_WAITING && s_turn.phase != T_SPEAKING) || !current()) {
        return;
    }
    esp_audio_dec_in_raw_t in = { .buffer = (uint8_t *)pkt, .len = len };
    esp_audio_dec_out_frame_t out = { .buffer = (uint8_t *)s_pcm, .len = DEC_FRAMES_MAX * sizeof(int16_t) };
    esp_audio_dec_info_t info;
    if (esp_opus_dec_decode(s_dec, &in, &out, &info) != ESP_AUDIO_ERR_OK || !out.decoded_size) {
        return;
    }
    size_t bytes = out.decoded_size, sent = 0;
    while (sent < bytes && current()) {   /* the speaker paces us; a cancel lets go */
        sent += xStreamBufferSend(s_out, (uint8_t *)s_pcm + sent, bytes - sent, pdMS_TO_TICKS(100));
    }
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    s_decoded += bytes / sizeof(int16_t);
    xSemaphoreGive(s_cap_lock);
}

/* Minimal MCP device: no tools, but answers so the server doesn't wait on us. */
static void handle_mcp(const cJSON *payload)
{
    const cJSON *method = cJSON_GetObjectItem(payload, "method");
    const cJSON *id = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsString(method) || !cJSON_IsNumber(id) || !strncmp(method->valuestring, "notifications", 13)) {
        return;
    }
    char result[192];
    if (!strcmp(method->valuestring, "initialize")) {
        snprintf(result, sizeof(result),
                 "\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                 "\"serverInfo\":{\"name\":\"muse-gadget-xiaozhi\",\"version\":\"%s\"}}",
                 esp_app_get_description()->version);
    } else if (!strcmp(method->valuestring, "tools/list")) {
        strlcpy(result, "\"result\":{\"tools\":[]}", sizeof(result));
    } else {
        strlcpy(result, "\"error\":{\"message\":\"Method not supported\"}", sizeof(result));
    }
    char members[320];
    snprintf(members, sizeof(members), "\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,%s}",
             id->valueint, result);
    send_msg(members);
}

static void handle_json(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;
    const cJSON *jtext = cJSON_GetObjectItem(root, "text");
    const char *str = cJSON_IsString(jtext) ? jtext->valuestring : "";

    if (!strcmp(t, "hello")) {
        const cJSON *tr = cJSON_GetObjectItem(root, "transport");
        if (cJSON_IsString(tr) && !strcmp(tr->valuestring, "websocket")) {
            s_session_id[0] = 0;
            copy_str(root, "session_id", s_session_id, sizeof(s_session_id));
            ESP_LOGI(TAG, "hello: session %s", s_session_id);
            s_hello = true;
        }
    } else if (!strcmp(t, "stt")) {
        ESP_LOGI(TAG, "heard: %s", str);
        if (current()) {
            post_ev(MUSE_HATCH_EV_HEARD, s_turn.gen, str);
        }
    } else if (!strcmp(t, "tts")) {
        const cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *state = cJSON_IsString(st) ? st->valuestring : "";
        ESP_LOGI(TAG, "tts %s%s%s", state, str[0] ? ": " : "", str);
        if (!current()) {
            /* a cancelled turn's tail */
        } else if (!strcmp(state, "start")) {
            s_turn.phase = T_SPEAKING;
            esp_opus_dec_reset(s_dec);
        } else if (!strcmp(state, "sentence_start") && str[0]) {
            add_sentence(str);
            post_ev(MUSE_HATCH_EV_REPLY, s_turn.gen, s_reply);
        } else if (!strcmp(state, "stop")) {
            xSemaphoreTake(s_cap_lock, portMAX_DELAY);
            s_reply_done = true;
            xSemaphoreGive(s_cap_lock);
            post_ev(MUSE_HATCH_EV_DONE, s_turn.gen, s_reply);
            s_turn.phase = T_IDLE;
        }
    } else if (!strcmp(t, "mcp")) {
        handle_mcp(cJSON_GetObjectItem(root, "payload"));
    }
    /* "llm" carries an emotion; the avatar keeps its own moods. */
    cJSON_Delete(root);
}

/* item: a tag byte, then the frame; it lives in s_pkt (FRAG_MAX), so it can be
 * shifted and terminated in place. */
static void handle_rx(uint8_t *item, size_t len)
{
    switch (item[0]) {
    case 'T':
        memmove(item, item + 1, len - 1);
        item[len - 1] = 0;
        handle_json((char *)item);
        break;
    case 'B': {
        const uint8_t *p = item + 1;
        int n = len - 1;
        if (s_ws_version == 2 && n >= 16) {
            p += 16, n -= 16;
        } else if (s_ws_version == 3 && n >= 4) {
            p += 4, n -= 4;
        }
        play_packet(p, n);
        break;
    }
    case 'X':
        ESP_LOGI(TAG, "session closed");
        s_hello = false;
        if (s_result == MUSE_HATCH_REACHABLE) {
            report(MUSE_HATCH_UNTESTED, "");
        }
        if (current()) {
            if (s_turn.phase == T_SPEAKING) {
                post_ev(MUSE_HATCH_EV_DONE, s_turn.gen, s_reply);
            } else {
                post_ev(MUSE_HATCH_EV_ERROR, s_turn.gen, "XIAOZHI HUNG UP");
            }
            s_turn.phase = T_IDLE;
        }
        break;
    default:
        break;
    }
}

/* Encodes and sends whole 60 ms frames of the press's speech; at the end, the
 * zero-padded rest and "listen stop". */
static void pump_mic(void)
{
    if (!s_turn.listening) {
        if (!ws_open()) {
            post_ev(MUSE_HATCH_EV_ERROR, s_turn.gen, "CAN'T REACH XIAOZHI");
            s_turn.phase = T_IDLE;
            return;
        }
        send_msg("\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"");
        s_turn.listening = true;
    }
    const size_t frame_bytes = ENC_FRAMES * sizeof(int16_t);
    for (;;) {
        size_t avail = xStreamBufferBytesAvailable(s_in);
        if (avail < frame_bytes && !(s_turn.phase == T_ENDING && avail)) {
            break;
        }
        size_t got = xStreamBufferReceive(s_in, s_pcm, frame_bytes, 0);
        memset((uint8_t *)s_pcm + got, 0, frame_bytes - got);
        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)s_pcm, .len = frame_bytes };
        esp_audio_enc_out_frame_t out = { .buffer = s_pkt, .len = 1500 };
        if (esp_opus_enc_process(s_enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes) {
            ws_send_audio(s_pkt, out.encoded_bytes);
        }
    }
    if (s_turn.phase == T_ENDING && xStreamBufferIsEmpty(s_in)) {
        send_msg("\"type\":\"listen\",\"state\":\"stop\"");
        post_ev(MUSE_HATCH_EV_SENT, s_turn.gen, "");
        s_turn.phase = T_WAITING;
        s_turn.deadline_us = esp_timer_get_time() + REPLY_WAIT_MS * 1000LL;
    }
}

static void handle_cmd(const cmd_t *c)
{
    switch (c->kind) {
    case CMD_BEGIN:
        if (s_turn.phase == T_SPEAKING || s_turn.phase == T_WAITING) {
            send_msg("\"type\":\"abort\"");
        }
        reset_reply();
        s_turn.phase = T_RECORDING;
        s_turn.gen = c->gen;
        s_turn.listening = false;
        esp_opus_enc_reset(s_enc);
        break;
    case CMD_END:
        if (s_turn.gen == c->gen && s_turn.phase == T_RECORDING) {
            s_turn.phase = T_ENDING;
        }
        break;
    case CMD_CANCEL:
        if (s_turn.gen == c->gen && s_turn.phase != T_IDLE) {
            if (s_turn.listening) {
                send_msg("\"type\":\"abort\"");
            }
            s_turn.phase = T_IDLE;
        }
        break;
    case CMD_CONNECT:
        if (s_have_config) {
            ws_open();
        } else {
            s_next_ota_us = 0;
        }
        break;
    case CMD_FORGET:
        ws_close();
        s_have_config = false;
        s_next_ota_us = 0;
        break;
    }
}

static void xiaozhi_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t c;
        bool busy = s_turn.phase == T_RECORDING || s_turn.phase == T_ENDING;
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(busy ? 5 : 20)) == pdTRUE) {
            handle_cmd(&c);
        }
        if (!muse_wifi_connected()) {
            if (s_ws) {
                ws_close();
            }
            if (current()) {
                post_ev(MUSE_HATCH_EV_ERROR, s_turn.gen, "NO WI-FI");
                s_turn.phase = T_IDLE;
            }
            continue;
        }
        if ((!s_have_config || s_code[0]) && esp_timer_get_time() >= s_next_ota_us) {
            ota_check();
        }
        if (current() && (s_turn.phase == T_RECORDING || s_turn.phase == T_ENDING) && s_have_config) {
            pump_mic();
        }
        size_t n;
        while ((n = xMessageBufferReceive(s_rx, s_pkt, FRAG_MAX, 0)) > 0) {
            handle_rx(s_pkt, n);
            if (uxQueueMessagesWaiting(s_cmds)) {
                break;   /* a cancel shouldn't wait behind a backlog of speech */
            }
        }
        if (current() && s_turn.phase == T_WAITING && esp_timer_get_time() > s_turn.deadline_us) {
            post_ev(MUSE_HATCH_EV_ERROR, s_turn.gen, "NO REPLY");
            s_turn.phase = T_IDLE;
        }
    }
}

/* ---- Identity: Device-Id is the MAC, Client-Id a UUID kept in NVS ---- */

static void load_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);

    nvs_handle_t nvs;
    size_t len = sizeof(s_client_id);
    if (nvs_open("xiaozhi", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_str(nvs, "uuid", s_client_id, &len) != ESP_OK || strlen(s_client_id) != 36) {
        uint8_t u[16];
        esp_fill_random(u, sizeof(u));
        u[6] = (u[6] & 0x0F) | 0x40;
        u[8] = (u[8] & 0x3F) | 0x80;
        snprintf(s_client_id, sizeof(s_client_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3],
                 u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        nvs_set_str(nvs, "uuid", s_client_id);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

/* ---- muse_hatch_* ---- */

void muse_hatch_start(void)
{
    if (s_cmds) {
        return;
    }
    load_identity();   /* NVS writes need an internal stack: here, not on the PSRAM one */
    s_events = xQueueCreateWithCaps(8, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    s_in = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_rx = xMessageBufferCreateWithCaps(RX_BYTES, MALLOC_CAP_SPIRAM);
    s_cap_lock = xSemaphoreCreateMutex();
    s_reply = heap_caps_calloc(1, REPLY_MAX, MUSE_BIG_CAPS);
    s_sentence = heap_caps_malloc(REPLY_MAX, MUSE_BIG_CAPS);
    s_pcm = heap_caps_malloc(DEC_FRAMES_MAX * sizeof(int16_t), MUSE_BIG_CAPS);
    s_pkt = heap_caps_malloc(FRAG_MAX, MUSE_BIG_CAPS);
    s_frag = heap_caps_malloc(FRAG_MAX, MUSE_BIG_CAPS);

    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = MUSE_AUDIO_RATE;
    enc.channel = 1;
    enc.bitrate = ESP_OPUS_BITRATE_AUTO;
    enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc.complexity = 0;
    enc.enable_dtx = true;
    enc.enable_vbr = true;
    if (esp_opus_enc_open(&enc, sizeof(enc), &s_enc) != ESP_AUDIO_ERR_OK) {
        s_enc = NULL;
    }
    /* Opus decodes to any rate: ask for the speaker's, whatever the server encoded at. */
    esp_opus_dec_cfg_t dec = {
        .sample_rate = MUSE_AUDIO_RATE,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&dec, sizeof(dec), &s_dec) != ESP_AUDIO_ERR_OK) {
        s_dec = NULL;
    }

    if (!s_events || !s_in || !s_out || !s_rx || !s_cap_lock || !s_reply || !s_sentence || !s_pcm || !s_pkt ||
        !s_frag || !s_enc || !s_dec) {
        ESP_LOGE(TAG, "start failed: out of memory or codec");
        return;
    }
    s_cmds = xQueueCreate(16, sizeof(cmd_t));
    if (!s_cmds || xTaskCreatePinnedToCoreWithCaps(xiaozhi_task, "xiaozhi", 32 * 1024, NULL, 5, NULL, 0,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed: task");
        s_cmds = NULL;
        return;
    }
    ESP_LOGI(TAG, "device %s", s_device_id);
}

static void post(cmd_kind_t kind, uint32_t gen)
{
    cmd_t c = { kind, gen };
    if (s_cmds) {
        xQueueSend(s_cmds, &c, pdMS_TO_TICKS(100));
    }
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    bool code = s_code[0] != 0;
    out->state = s_result;
    if (code) {
        snprintf(out->detail, sizeof(out->detail), "Code %s at xiaozhi.me", s_code);
    } else {
        strlcpy(out->detail, s_detail, sizeof(out->detail));
    }
    portEXIT_CRITICAL(&s_lock);
    if (code) {
        out->state = MUSE_HATCH_NOT_SET;
    } else if (!muse_wifi_connected() && out->state != MUSE_HATCH_TESTING) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (out->state == MUSE_HATCH_UNTESTED && !out->detail[0]) {
        strlcpy(out->detail, "Connects when you talk", sizeof(out->detail));
    }
}

bool muse_hatch_activation_code(char *out, size_t cap)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_code, cap);
    portEXIT_CRITICAL(&s_lock);
    return out[0] != 0;
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Activate";
    case MUSE_HATCH_OFFLINE: return "Offline";
    case MUSE_HATCH_UNTESTED: return "Ready";
    case MUSE_HATCH_TESTING: return "Connecting";
    case MUSE_HATCH_REACHABLE: return "Connected";
    case MUSE_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

void muse_hatch_test(void)
{
    if (muse_wifi_connected()) {
        report(MUSE_HATCH_TESTING, "Connecting...");
        post(CMD_CONNECT, 0);
    }
}

void muse_hatch_config_changed(void)
{
    report(MUSE_HATCH_UNTESTED, "");
    post(CMD_FORGET, 0);
}

void muse_hatch_set_resting(bool resting)
{
    (void)resting;   /* the server closes an idle session itself */
}

bool muse_hatch_ready(void)
{
    return s_cmds && s_have_config && !s_code[0] && muse_wifi_connected();
}

void muse_hatch_turn_begin(void)
{
    uint32_t gen = atomic_fetch_add(&s_gen, 1) + 1;
    xStreamBufferReset(s_in);
    xStreamBufferReset(s_out);
    post(CMD_BEGIN, gen);
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        ESP_LOGW(TAG, "mic backlog full, dropped audio");
    }
}

size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferSend(s_in, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

void muse_hatch_turn_end(void)
{
    post(CMD_END, atomic_load(&s_gen));
}

void muse_hatch_turn_cancel(void)
{
    uint32_t gen = atomic_fetch_add(&s_gen, 1);
    post(CMD_CANCEL, gen);
    if (s_out) {
        xStreamBufferReset(s_out);
    }
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    static ev_t ev;   /* voice task only */
    while (s_events && xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == atomic_load(&s_gen)) {
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

/* The page of the sentence being spoken, at the point its speech has reached:
 * a sentence runs from where its audio starts to where the next one's does. */
bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    if (!s_cap_lock) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(s_cap_lock, portMAX_DELAY);
    int i = s_nsent - 1;
    while (i > 0 && s_sent[i].start > played) {
        i--;
    }
    if (i >= 0) {
        bool last = i + 1 == s_nsent;
        size_t len = last ? strlen(s_reply + s_sent[i].off) : (size_t)(s_sent[i + 1].off - 1 - s_sent[i].off);
        uint32_t frames = !last ? s_sent[i + 1].start - s_sent[i].start
                          : s_reply_done ? s_decoded - s_sent[i].start
                                         : (uint32_t)(len * MUSE_AUDIO_RATE / SPEECH_CHARS_PER_S);
        size_t at = 0;
        if (played > s_sent[i].start && frames) {
            at = (size_t)((uint64_t)(played - s_sent[i].start) * len / frames);
        }
        if (at >= len) {
            at = len ? len - 1 : 0;
        }
        memcpy(s_sentence, s_reply + s_sent[i].off, len);
        s_sentence[len] = 0;
        ok = muse_hatch_caption_at(s_sentence, at, out, cap);
    }
    xSemaphoreGive(s_cap_lock);
    return ok;
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}

/* Typed turns: xiaozhi's session takes speech only. */
void muse_hatch_text_turn(char *text)
{
    free(text);
    muse_hatch_console("error", "TYPED CHAT NOT SUPPORTED BY XIAOZHI", NULL);
}

void muse_hatch_text_cancel(void)
{
}

/* muse_chat_priv.h */
bool muse_hatch_configured(void)
{
    return s_have_config && !s_code[0];
}

void muse_hatch_report(muse_hatch_state_t state, const char *detail)
{
    report(state, detail);
}

void muse_hatch_chat_connect(void)
{
    post(CMD_CONNECT, 0);
}

void muse_hatch_chat_forget(void)
{
    post(CMD_FORGET, 0);
}
