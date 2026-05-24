#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_brookesia.hpp"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "CodexPet"
#include "esp_lib_utils.h"

#include "codex_pet_app.hpp"

#define APP_NAME "Codex Pet"
#define TAG "codex_pet"
#define LOGICAL_WIDTH 240
#define LOGICAL_HEIGHT 240
#define CANVAS_WIDTH 288
#define CANVAS_HEIGHT 288
#define SCREEN_BG_COLOR 0xffffff
#define SKY_BG_COLOR 0x87ceeb

namespace {

enum PetState {
    PET_STATE_IDLE = 0,
    PET_STATE_RUNNING,
    PET_STATE_DONE,
    PET_STATE_OFFLINE,
};

enum PetPattern {
    PET_PATTERN_PURPLE = 0,
    PET_PATTERN_GUITAR,
    PET_PATTERN_BALL,
    PET_PATTERN_COUNT,
};

struct HttpBody {
    char *buffer;
    int buffer_len;
    int used;
};

volatile PetState s_pet_state = PET_STATE_IDLE;
volatile bool s_bridge_online = false;
EventGroupHandle_t s_wifi_events = nullptr;
bool s_wifi_started = false;
bool s_poll_task_started = false;

constexpr int WIFI_CONNECTED_BIT = BIT0;

int scaleAxis(int value, int logical_size, int canvas_size)
{
    int half = logical_size / 2;
    return (value * canvas_size + (value >= 0 ? half : -half)) / logical_size;
}

int scaleX(int value)
{
    return scaleAxis(value, LOGICAL_WIDTH, CANVAS_WIDTH);
}

int scaleY(int value)
{
    return scaleAxis(value, LOGICAL_HEIGHT, CANVAS_HEIGHT);
}

int scaleLen(int value)
{
    if (value <= 0) {
        return value;
    }
    int scaled = (value * CANVAS_WIDTH + LOGICAL_WIDTH / 2) / LOGICAL_WIDTH;
    return scaled > 0 ? scaled : 1;
}

const char *stateText(PetState state)
{
    switch (state) {
    case PET_STATE_RUNNING:
        return "RUNNING";
    case PET_STATE_DONE:
        return "DONE";
    case PET_STATE_OFFLINE:
        return "OFFLINE";
    default:
        return "IDLE";
    }
}

void setPetState(PetState state)
{
    s_pet_state = state;
}

void wifiEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        s_bridge_online = false;
        setPetState(PET_STATE_OFFLINE);
        if (s_wifi_events != nullptr) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d, reconnecting", event ? event->reason : -1);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_events != nullptr) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t initNvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifiInitSta(void)
{
    ESP_RETURN_ON_ERROR(initNvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop failed");
    }

    if (s_wifi_events == nullptr) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events != nullptr, ESP_ERR_NO_MEM, TAG, "wifi event group failed");
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr),
        TAG,
        "wifi handler failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr),
        TAG,
        "ip handler failed"
    );

    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), CONFIG_CODEX_PET_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(
        reinterpret_cast<char *>(wifi_config.sta.password),
        CONFIG_CODEX_PET_WIFI_PASSWORD,
        sizeof(wifi_config.sta.password) - 1
    );
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    if (strlen(CONFIG_CODEX_PET_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "Connecting Wi-Fi SSID '%s'", CONFIG_CODEX_PET_WIFI_SSID);
    return ESP_OK;
}

PetState parseStateResponse(const char *body)
{
    if (strstr(body, "\"running\"") || strstr(body, "running")) {
        return PET_STATE_RUNNING;
    }
    if (strstr(body, "\"done\"") || strstr(body, "done")) {
        return PET_STATE_DONE;
    }
    if (strstr(body, "\"idle\"") || strstr(body, "idle")) {
        return PET_STATE_IDLE;
    }
    return PET_STATE_OFFLINE;
}

esp_err_t httpEventHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == nullptr || evt->data == nullptr || evt->data_len <= 0) {
        return ESP_OK;
    }

    HttpBody *body = static_cast<HttpBody *>(evt->user_data);
    int remaining = body->buffer_len - body->used - 1;
    if (remaining <= 0) {
        return ESP_OK;
    }

    int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
    memcpy(body->buffer + body->used, evt->data, copy_len);
    body->used += copy_len;
    body->buffer[body->used] = '\0';
    return ESP_OK;
}

void statePollTask(void *arg)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    while (true) {
        char body[192] = {};
        HttpBody response = {
            .buffer = body,
            .buffer_len = sizeof(body),
            .used = 0,
        };
        esp_http_client_config_t config = {};
        config.url = CONFIG_CODEX_PET_BRIDGE_URL;
        config.timeout_ms = 1500;
        config.event_handler = httpEventHandler;
        config.user_data = &response;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            s_bridge_online = false;
            setPetState(PET_STATE_OFFLINE);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_CODEX_PET_POLL_INTERVAL_MS));
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200 && response.used > 0) {
                s_bridge_online = true;
                PetState new_state = parseStateResponse(body);
                if (new_state != s_pet_state) {
                    ESP_LOGI(TAG, "Bridge state changed: %s", stateText(new_state));
                }
                setPetState(new_state);
            } else {
                s_bridge_online = false;
                ESP_LOGW(TAG, "Bridge returned HTTP %d with %d bytes", status, response.used);
                setPetState(PET_STATE_OFFLINE);
            }
        } else {
            s_bridge_online = false;
            ESP_LOGW(TAG, "Bridge request failed: %s", esp_err_to_name(err));
            setPetState(PET_STATE_OFFLINE);
        }

        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CODEX_PET_POLL_INTERVAL_MS));
    }
}

void drawRoundRect(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, int radius, lv_opa_t opa)
{
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_hex(color);
    rect.bg_opa = opa;
    rect.radius = scaleLen(radius);
    lv_area_t area = {
        .x1 = scaleX(x1),
        .y1 = scaleY(y1),
        .x2 = scaleX(x2),
        .y2 = scaleY(y2),
    };
    lv_draw_rect(layer, &rect, &area);
}

void drawLineSegment(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, int width)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(color);
    line.width = scaleLen(width);
    line.round_start = 1;
    line.round_end = 1;
    line.p1 = (lv_point_precise_t){scaleX(x1), scaleY(y1)};
    line.p2 = (lv_point_precise_t){scaleX(x2), scaleY(y2)};
    lv_draw_line(layer, &line);
}

void drawTriangle(lv_layer_t *layer, int x1, int y1, int x2, int y2, int x3, int y3, uint32_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = lv_color_hex(color);
    tri.opa = opa;
    tri.p[0] = (lv_point_precise_t){scaleX(x1), scaleY(y1)};
    tri.p[1] = (lv_point_precise_t){scaleX(x2), scaleY(y2)};
    tri.p[2] = (lv_point_precise_t){scaleX(x3), scaleY(y3)};
    lv_draw_triangle(layer, &tri);
}

void drawLabelArea(lv_layer_t *layer, lv_draw_label_dsc_t *label, int x1, int y1, int x2, int y2)
{
    lv_area_t area = {
        .x1 = scaleX(x1),
        .y1 = scaleY(y1),
        .x2 = scaleX(x2),
        .y2 = scaleY(y2),
    };
    lv_draw_label(layer, label, &area);
}

void drawPurplePet(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t body = 0x9f88ed;
    const uint32_t foot = 0x4d3f82;
    const uint32_t ink = 0x221b35;
    const uint32_t white = 0xffffff;
    const uint32_t floor_line = 0xaeb4bf;
    const uint32_t shadow = 0xb7b7b7;

    int bounce = running ? (int)(sinf(frame * 0.42f) * 4.0f) : 0;
    int sway = running ? (int)(sinf(frame * 0.28f) * 3.0f) : 0;
    int left_step = running ? (int)(sinf(frame * 1.48f) * 5.0f) : 0;
    int right_step = running ? (int)(sinf(frame * 1.48f + 3.14159f) * 5.0f) : 0;
    int eye_shift = running ? (int)(sinf(frame * 0.32f) * 2.0f) : 0;

    int body_x = x + 30 + sway;
    int body_y = y + 16 - bounce;
    int body_size = 158;
    int floor_y = y + 202;

    drawLineSegment(layer, 0, floor_y, LOGICAL_WIDTH - 1, floor_y, floor_line, 2);
    drawRoundRect(layer, x + 105, y + 211, x + 150, y + 219, shadow, 24, LV_OPA_70);

    drawRoundRect(
        layer,
        body_x + 42 + left_step,
        body_y + 122 + bounce / 2,
        body_x + 78 + left_step,
        body_y + 184 + bounce / 2,
        foot,
        20,
        LV_OPA_COVER
    );
    drawRoundRect(
        layer,
        body_x + 114 + right_step,
        body_y + 120 + bounce / 2,
        body_x + 150 + right_step,
        body_y + 176 + bounce / 2,
        foot,
        18,
        LV_OPA_COVER
    );

    drawRoundRect(layer, body_x, body_y, body_x + body_size, body_y + body_size, body, 90, LV_OPA_COVER);

    drawRoundRect(layer, body_x + 84, body_y + 44, body_x + 129, body_y + 94, white, 25, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 126, body_y + 38, body_x + 170, body_y + 88, white, 25, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 106 + eye_shift, body_y + 54, body_x + 127 + eye_shift, body_y + 85, 0x000000, 13, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 148 + eye_shift, body_y + 48, body_x + 169 + eye_shift, body_y + 79, 0x000000, 13, LV_OPA_COVER);

    drawLineSegment(layer, body_x + 127, body_y + 105, body_x + 132, body_y + 116, ink, 3);
    drawLineSegment(layer, body_x + 132, body_y + 116, body_x + 138, body_y + 118, ink, 3);
    drawLineSegment(layer, body_x + 138, body_y + 118, body_x + 146, body_y + 110, ink, 3);
}

void drawCatHead(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t fur = 0x222733;
    const uint32_t fur_light = 0xb7bdcc;
    const uint32_t muzzle = 0xe9edf4;
    const uint32_t white = 0xffffff;
    const uint32_t ink = 0x05070a;
    const uint32_t tongue = 0xff7e88;
    int look = running ? (int)(sinf(frame * 0.24f) * 2.0f) : 0;

    drawTriangle(layer, x + 14, y + 28, x + 31, y - 21, x + 45, y + 31, fur, LV_OPA_COVER);
    drawTriangle(layer, x + 106, y + 30, x + 147, y + 1, x + 128, y + 53, fur, LV_OPA_COVER);
    drawTriangle(layer, x + 24, y + 20, x + 32, y - 9, x + 41, y + 26, fur_light, LV_OPA_70);
    drawTriangle(layer, x + 116, y + 28, x + 138, y + 10, x + 126, y + 43, fur_light, LV_OPA_70);

    drawRoundRect(layer, x, y + 18, x + 142, y + 109, fur, 54, LV_OPA_COVER);
    drawRoundRect(layer, x + 27, y + 57, x + 88, y + 109, 0x2e3542, 36, LV_OPA_COVER);
    drawRoundRect(layer, x + 61, y + 72, x + 96, y + 101, muzzle, 16, LV_OPA_COVER);

    drawRoundRect(layer, x + 44, y + 47, x + 78, y + 78, white, 15, LV_OPA_COVER);
    drawRoundRect(layer, x + 85, y + 49, x + 119, y + 80, white, 15, LV_OPA_COVER);
    drawRoundRect(layer, x + 62 + look, y + 59, x + 70 + look, y + 72, ink, 5, LV_OPA_COVER);
    drawRoundRect(layer, x + 101 + look, y + 61, x + 109 + look, y + 74, ink, 5, LV_OPA_COVER);

    drawRoundRect(layer, x + 76, y + 78, x + 86, y + 86, ink, 5, LV_OPA_COVER);
    drawLineSegment(layer, x + 81, y + 85, x + 74, y + 93, ink, 2);
    drawLineSegment(layer, x + 81, y + 85, x + 90, y + 94, ink, 2);
    drawRoundRect(layer, x + 77, y + 92, x + 88, y + 106, tongue, 7, LV_OPA_COVER);

    drawLineSegment(layer, x + 16, y + 72, x - 15, y + 64, ink, 2);
    drawLineSegment(layer, x + 19, y + 84, x - 15, y + 86, ink, 2);
    drawLineSegment(layer, x + 119, y + 73, x + 153, y + 64, ink, 2);
    drawLineSegment(layer, x + 116, y + 85, x + 151, y + 87, ink, 2);
}

void drawCatGuitar(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t fur = 0x222733;
    const uint32_t fur_light = 0xb7bdcc;
    const uint32_t belly = 0xd8dde8;
    const uint32_t paw = 0x141820;
    const uint32_t guitar = 0xf3c545;
    const uint32_t guitar_dark = 0xea7e25;
    const uint32_t guitar_light = 0xffdf72;
    const uint32_t string = 0xfff4cf;
    const uint32_t shadow = 0xb0b7c4;
    int strum = running ? (int)(sinf(frame * 0.95f) * 10.0f) : 0;
    int bob = running ? (int)(sinf(frame * 0.30f) * 2.0f) : 0;

    int cat_x = x + 18;
    int cat_y = y + 18 + bob;
    drawRoundRect(layer, x + 52, y + 209, x + 165, y + 222, shadow, 24, LV_OPA_50);

    drawLineSegment(layer, cat_x + 30, cat_y + 123, cat_x + 4, cat_y + 154, fur, 30);
    drawRoundRect(layer, cat_x - 2, cat_y + 140, cat_x + 45, cat_y + 188, fur, 24, LV_OPA_COVER);
    drawLineSegment(layer, cat_x + 39, cat_y + 132, cat_x + 10, cat_y + 151, fur_light, 5);
    drawRoundRect(layer, cat_x + 39, cat_y + 151, cat_x + 68, cat_y + 178, paw, 14, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 62, cat_y + 163, cat_x + 94, cat_y + 196, paw, 16, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 68, cat_y + 172, cat_x + 91, cat_y + 194, 0x303743, 12, LV_OPA_COVER);

    drawRoundRect(layer, cat_x + 43, cat_y + 83, cat_x + 151, cat_y + 189, fur, 56, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 75, cat_y + 118, cat_x + 137, cat_y + 183, belly, 34, LV_OPA_COVER);

    drawLineSegment(layer, cat_x + 26, cat_y + 118, cat_x + 76, cat_y + 95, fur, 20);
    drawRoundRect(layer, cat_x + 60, cat_y + 84, cat_x + 90, cat_y + 109, paw, 14, LV_OPA_COVER);

    drawRoundRect(layer, cat_x + 118, cat_y + 141, cat_x + 215, cat_y + 219, guitar, 39, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 100, cat_y + 121, cat_x + 159, cat_y + 188, guitar, 34, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 114, cat_y + 136, cat_x + 144, cat_y + 166, 0x8a4a1f, 16, LV_OPA_COVER);
    drawRoundRect(layer, cat_x + 124, cat_y + 148, cat_x + 217, cat_y + 219, guitar_dark, 36, LV_OPA_40);
    drawLineSegment(layer, cat_x + 36, cat_y + 108, cat_x + 133, cat_y + 149, guitar, 25);
    drawLineSegment(layer, cat_x + 34, cat_y + 103, cat_x + 130, cat_y + 144, guitar_light, 18);
    drawRoundRect(layer, cat_x + 30, cat_y + 96, cat_x + 52, cat_y + 124, guitar_light, 10, LV_OPA_COVER);
    drawLineSegment(layer, cat_x + 45, cat_y + 109, cat_x + 151, cat_y + 168, string, 2);
    drawLineSegment(layer, cat_x + 44, cat_y + 116, cat_x + 150, cat_y + 175, string, 2);
    drawLineSegment(layer, cat_x + 43, cat_y + 123, cat_x + 149, cat_y + 182, string, 2);
    drawLineSegment(layer, cat_x + 186, cat_y + 176, cat_x + 205, cat_y + 144, guitar_dark, 7);

    drawLineSegment(layer, cat_x + 132, cat_y + 122, cat_x + 154, cat_y + 167 + strum / 2, paw, 15);
    drawRoundRect(layer, cat_x + 146, cat_y + 160 + strum / 2, cat_x + 165, cat_y + 181 + strum / 2, paw, 10, LV_OPA_COVER);

    drawCatHead(layer, cat_x + 21, cat_y + 0, frame, running);
}

void drawBallRunner(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t fur = 0x222733;
    const uint32_t fur_light = 0xb7bdcc;
    const uint32_t muzzle = 0xe9edf4;
    const uint32_t white = 0xffffff;
    const uint32_t ink = 0x05070a;
    const uint32_t paw = 0x141820;
    const uint32_t ball = 0xe9a72d;
    const uint32_t ball_light = 0xf5be45;
    const uint32_t ball_line = 0xa65d15;
    const uint32_t shadow = 0xaeb4bf;

    int roll = running ? frame * 6 : 0;
    int bounce = running ? (int)(sinf(frame * 0.34f) * 2.0f) : 0;
    int tail_sway = running ? (int)(sinf(frame * 0.42f) * 4.0f) : 0;
    int paw_swing = running ? (int)(sinf(frame * 0.70f) * 2.0f) : 0;
    int look = running ? (int)(sinf(frame * 0.24f) * 2.0f) : 0;

    int ball_x = x + 78;
    int ball_y = y + 111;
    int ball_size = 96;
    int body_x = x + 62;
    int body_y = y + 57 - bounce;
    int head_x = x + 20;
    int head_y = y + 44 - bounce;

    drawRoundRect(layer, x + 70, y + 220, x + 192, y + 231, shadow, 26, LV_OPA_50);

    drawRoundRect(layer, ball_x, ball_y, ball_x + ball_size, ball_y + ball_size, ball, ball_size / 2, LV_OPA_COVER);
    drawRoundRect(layer, ball_x + 7, ball_y + 7, ball_x + 49, ball_y + 51, ball_light, 28, LV_OPA_40);
    drawLineSegment(layer, ball_x + 6 + (roll % 12) / 3, ball_y + 35, ball_x + 86 - (roll % 12) / 4, ball_y + 22, ball_line, 3);
    drawLineSegment(layer, ball_x + 4 + (roll % 12) / 4, ball_y + 64, ball_x + 90 - (roll % 12) / 5, ball_y + 49, ball_line, 3);
    drawLineSegment(layer, ball_x + 40 - (roll % 12) / 2, ball_y + 5, ball_x + 62 + (roll % 12) / 3, ball_y + 91, ball_line, 3);
    drawLineSegment(layer, ball_x + 85, ball_y + 22, ball_x + 76, ball_y + 76, ball_line, 3);
    drawLineSegment(layer, ball_x + 5, ball_y + 35, ball_x + 41, ball_y + 95, ball_line, 2);

    drawLineSegment(layer, body_x + 112, body_y + 26, body_x + 150, body_y - 6 + tail_sway, fur, 22);
    drawLineSegment(layer, body_x + 147, body_y - 7 + tail_sway, body_x + 138, body_y - 28 + tail_sway, fur, 18);
    drawLineSegment(layer, body_x + 138, body_y - 28 + tail_sway, body_x + 151, body_y - 44 + tail_sway, fur, 14);
    drawLineSegment(layer, body_x + 140, body_y - 24 + tail_sway, body_x + 149, body_y - 37 + tail_sway, fur_light, 8);
    drawLineSegment(layer, body_x + 132, body_y - 3 + tail_sway, body_x + 145, body_y - 15 + tail_sway, fur_light, 7);

    drawRoundRect(layer, body_x + 6, body_y + 1, body_x + 119, body_y + 72, fur, 40, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 48, body_y + 4, body_x + 127, body_y + 78, fur, 38, LV_OPA_COVER);
    drawLineSegment(layer, body_x + 16, body_y + 52, body_x + 68, body_y + 82, muzzle, 9);
    drawLineSegment(layer, body_x + 82, body_y + 6, body_x + 103, body_y + 35, fur_light, 4);
    drawLineSegment(layer, body_x + 102, body_y + 9, body_x + 119, body_y + 40, fur_light, 4);
    drawLineSegment(layer, body_x + 77, body_y + 16, body_x + 80, body_y + 46, ink, 2);
    drawLineSegment(layer, body_x + 94, body_y + 20, body_x + 98, body_y + 49, ink, 2);

    drawLineSegment(layer, body_x + 50, body_y + 52, ball_x + 86, ball_y + 22 + paw_swing, fur, 17);
    drawRoundRect(layer, ball_x + 76, ball_y + 17 + paw_swing, ball_x + 97, ball_y + 37 + paw_swing, paw, 10, LV_OPA_COVER);
    drawLineSegment(layer, body_x + 118, body_y + 48, ball_x + 100, ball_y + 72, fur, 18);
    drawRoundRect(layer, ball_x + 91, ball_y + 66, ball_x + 112, ball_y + 89, paw, 10, LV_OPA_COVER);

    drawTriangle(layer, head_x + 12, head_y + 20, head_x + 4, head_y - 21, head_x + 45, head_y + 9, fur, LV_OPA_COVER);
    drawTriangle(layer, head_x + 69, head_y + 15, head_x + 98, head_y - 34, head_x + 104, head_y + 26, fur, LV_OPA_COVER);
    drawTriangle(layer, head_x + 22, head_y + 12, head_x + 13, head_y - 12, head_x + 39, head_y + 9, fur_light, LV_OPA_70);
    drawTriangle(layer, head_x + 78, head_y + 11, head_x + 94, head_y - 17, head_x + 96, head_y + 19, fur_light, LV_OPA_70);

    drawRoundRect(layer, head_x, head_y, head_x + 96, head_y + 65, fur, 34, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 25, head_y + 32, head_x + 71, head_y + 64, muzzle, 18, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 31, head_y + 15, head_x + 51, head_y + 43, white, 10, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 60, head_y + 13, head_x + 82, head_y + 42, white, 10, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 40 + look, head_y + 22, head_x + 47 + look, head_y + 35, ink, 4, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 69 + look, head_y + 20, head_x + 76 + look, head_y + 34, ink, 4, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 49, head_y + 40, head_x + 60, head_y + 48, ink, 5, LV_OPA_COVER);
    drawLineSegment(layer, head_x + 54, head_y + 47, head_x + 44, head_y + 55, ink, 2);
    drawLineSegment(layer, head_x + 54, head_y + 47, head_x + 66, head_y + 55, ink, 2);
    drawLineSegment(layer, head_x + 14, head_y + 37, head_x - 16, head_y + 31, ink, 2);
    drawLineSegment(layer, head_x + 16, head_y + 48, head_x - 15, head_y + 50, ink, 2);
    drawLineSegment(layer, head_x + 80, head_y + 36, head_x + 111, head_y + 30, ink, 2);
    drawLineSegment(layer, head_x + 79, head_y + 47, head_x + 110, head_y + 50, ink, 2);
}

void drawPatternDots(lv_layer_t *layer, int selected)
{
    int start_x = 97;
    for (int i = 0; i < PET_PATTERN_COUNT; ++i) {
        uint32_t color = i == selected ? 0x111111 : 0xc8ccd3;
        int x = start_x + i * 18;
        drawRoundRect(layer, x, 231, x + 8, 239, color, 4, LV_OPA_COVER);
    }
}

} // namespace

namespace esp_brookesia::apps {

CodexPetApp *CodexPetApp::_instance = nullptr;

CodexPetApp *CodexPetApp::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new CodexPetApp(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

CodexPetApp::CodexPetApp(bool use_status_bar, bool use_navigation_bar):
    App(APP_NAME, nullptr, true, use_status_bar, use_navigation_bar),
    _draw_buf(nullptr),
    _canvas(nullptr),
    _pattern_index(PET_PATTERN_PURPLE)
{
}

CodexPetApp::~CodexPetApp()
{
    if (_draw_buf != nullptr) {
        lv_draw_buf_destroy(_draw_buf);
        _draw_buf = nullptr;
    }
}

bool CodexPetApp::init(void)
{
    if (strlen(CONFIG_CODEX_PET_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "CONFIG_CODEX_PET_WIFI_SSID is empty");
        setPetState(PET_STATE_OFFLINE);
        return true;
    }
    if (strlen(CONFIG_CODEX_PET_BRIDGE_URL) == 0) {
        ESP_LOGW(TAG, "CONFIG_CODEX_PET_BRIDGE_URL is empty");
        setPetState(PET_STATE_OFFLINE);
        return true;
    }

    if (!s_wifi_started) {
        esp_err_t err = wifiInitSta();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
            setPetState(PET_STATE_OFFLINE);
            return true;
        }
        s_wifi_started = true;
    }

    if (!s_poll_task_started) {
        BaseType_t ok = xTaskCreate(statePollTask, "codex_pet_poll", 8192, nullptr, 4, nullptr);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "state poll task create failed");
            setPetState(PET_STATE_OFFLINE);
        } else {
            s_poll_task_started = true;
        }
    }

    return true;
}

bool CodexPetApp::run(void)
{
    lv_obj_t *screen = lv_screen_active();
    ESP_UTILS_CHECK_NULL_RETURN(screen, false, "Invalid active screen");

    if (_draw_buf == nullptr) {
        _draw_buf = lv_draw_buf_create(CANVAS_WIDTH, CANVAS_HEIGHT, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        ESP_UTILS_CHECK_NULL_RETURN(_draw_buf, false, "Create canvas draw buffer failed");
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(SCREEN_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    _canvas = lv_canvas_create(screen);
    ESP_UTILS_CHECK_NULL_RETURN(_canvas, false, "Create canvas failed");
    lv_obj_set_size(_canvas, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_center(_canvas);
    lv_canvas_set_draw_buf(_canvas, _draw_buf);
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(_canvas, gestureCallback, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(screen, gestureCallback, LV_EVENT_GESTURE, this);

    lv_timer_create(timerCallback, 70, this);
    drawFrame();

    return true;
}

bool CodexPetApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool CodexPetApp::cleanResource(void)
{
    _canvas = nullptr;
    return true;
}

void CodexPetApp::timerCallback(lv_timer_t *timer)
{
    CodexPetApp *app = static_cast<CodexPetApp *>(lv_timer_get_user_data(timer));
    if (app != nullptr) {
        app->drawFrame();
    }
}

void CodexPetApp::gestureCallback(lv_event_t *event)
{
    CodexPetApp *app = static_cast<CodexPetApp *>(lv_event_get_user_data(event));
    if (app == nullptr) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        app->_pattern_index = (app->_pattern_index + 1) % PET_PATTERN_COUNT;
    } else if (dir == LV_DIR_RIGHT) {
        app->_pattern_index = (app->_pattern_index + PET_PATTERN_COUNT - 1) % PET_PATTERN_COUNT;
    } else {
        return;
    }
    ESP_LOGI(TAG, "Pattern changed: %d", app->_pattern_index);
    app->drawFrame();
}

void CodexPetApp::drawFrame(void)
{
    static int frame = 0;
    if (_canvas == nullptr) {
        return;
    }

    PetState state = s_pet_state;
    bool running = state == PET_STATE_RUNNING;
    int bob = running ? (int)(sinf(frame * 0.45f) * 2.0f) : 0;
    int cat_x = running ? 0 + (int)(sinf(frame * 0.18f) * 2.0f) : 0;

    lv_layer_t layer;
    lv_canvas_init_layer(_canvas, &layer);
    uint32_t bg = _pattern_index == PET_PATTERN_PURPLE ? SCREEN_BG_COLOR : SKY_BG_COLOR;
    lv_obj_t *screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    }
    lv_canvas_fill_bg(_canvas, lv_color_hex(bg), LV_OPA_COVER);

    if (_pattern_index == PET_PATTERN_BALL) {
        drawBallRunner(&layer, cat_x, 14 + bob, frame, running);
    } else if (_pattern_index == PET_PATTERN_GUITAR) {
        drawCatGuitar(&layer, cat_x, 5 + bob, frame, running);
    } else {
        drawPurplePet(&layer, cat_x, 16 + bob, frame, running);
    }
    drawPatternDots(&layer, _pattern_index);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = lv_color_hex(0x111111);
    label.font = &lv_font_montserrat_24;
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = stateText(state);
    drawLabelArea(&layer, &label, 0, 3, LOGICAL_WIDTH, 31);

    lv_canvas_finish_layer(_canvas, &layer);
    frame++;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, CodexPetApp, APP_NAME, []()
{
    return std::shared_ptr<CodexPetApp>(CodexPetApp::requestInstance(), [](CodexPetApp *p) {});
})

} // namespace esp_brookesia::apps
