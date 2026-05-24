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
#define CANVAS_WIDTH 240
#define CANVAS_HEIGHT 240
#define SCREEN_BG_COLOR 0xffffff

namespace {

enum PetState {
    PET_STATE_IDLE = 0,
    PET_STATE_RUNNING,
    PET_STATE_DONE,
    PET_STATE_OFFLINE,
};

enum PetPattern {
    PET_PATTERN_GUITAR = 0,
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
    rect.radius = radius;
    lv_area_t area = {.x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2};
    lv_draw_rect(layer, &rect, &area);
}

void drawLineSegment(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, int width)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(color);
    line.width = width;
    line.round_start = 1;
    line.round_end = 1;
    line.p1 = (lv_point_precise_t){x1, y1};
    line.p2 = (lv_point_precise_t){x2, y2};
    lv_draw_line(layer, &line);
}

void drawGuitarPet(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t body = 0x9f88ed;
    const uint32_t foot = 0x4d3f82;
    const uint32_t ink = 0x221b35;
    const uint32_t white = 0xffffff;
    const uint32_t guitar = 0xd88937;
    const uint32_t guitar_dark = 0x7b4a22;
    const uint32_t guitar_light = 0xf4bc60;
    const uint32_t floor_line = 0xaeb4bf;
    const uint32_t shadow = 0xb7b7b7;

    int bounce = running ? (int)(sinf(frame * 0.34f) * 3.0f) : 0;
    int sway = running ? (int)(sinf(frame * 0.22f) * 2.0f) : 0;
    int strum = running ? (int)(sinf(frame * 0.92f) * 12.0f) : 0;
    int left_step = running ? (int)(sinf(frame * 0.44f) * 2.0f) : 0;
    int right_step = running ? (int)(sinf(frame * 0.44f + 3.14159f) * 2.0f) : 0;
    int eye_shift = running ? (int)(sinf(frame * 0.32f) * 2.0f) : 0;

    int body_x = x + 30 + sway;
    int body_y = y + 16 - bounce;
    int body_size = 158;
    int floor_y = y + 202;

    drawLineSegment(layer, 0, floor_y, CANVAS_WIDTH - 1, floor_y, floor_line, 2);
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

    drawLineSegment(layer, body_x + 148, body_y + 116, body_x + 208, body_y + 74, guitar_dark, 10);
    drawLineSegment(layer, body_x + 146, body_y + 113, body_x + 206, body_y + 71, guitar_light, 3);
    drawRoundRect(layer, body_x + 198, body_y + 62, body_x + 225, body_y + 80, guitar_dark, 8, LV_OPA_COVER);

    drawRoundRect(layer, body_x, body_y, body_x + body_size, body_y + body_size, body, 90, LV_OPA_COVER);

    drawRoundRect(layer, body_x + 84, body_y + 44, body_x + 129, body_y + 94, white, 25, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 126, body_y + 38, body_x + 170, body_y + 88, white, 25, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 106 + eye_shift, body_y + 54, body_x + 127 + eye_shift, body_y + 85, 0x000000, 13, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 148 + eye_shift, body_y + 48, body_x + 169 + eye_shift, body_y + 79, 0x000000, 13, LV_OPA_COVER);

    drawLineSegment(layer, body_x + 127, body_y + 105, body_x + 132, body_y + 116, ink, 3);
    drawLineSegment(layer, body_x + 132, body_y + 116, body_x + 138, body_y + 118, ink, 3);
    drawLineSegment(layer, body_x + 138, body_y + 118, body_x + 146, body_y + 110, ink, 3);

    drawRoundRect(layer, body_x + 76, body_y + 118, body_x + 150, body_y + 176, guitar, 30, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 89, body_y + 128, body_x + 136, body_y + 165, guitar_light, 24, LV_OPA_40);
    drawRoundRect(layer, body_x + 107, body_y + 135, body_x + 125, body_y + 153, 0x2b1a13, 10, LV_OPA_COVER);
    drawLineSegment(layer, body_x + 80, body_y + 135, body_x + 210, body_y + 75, 0xf8e6bd, 2);
    drawLineSegment(layer, body_x + 81, body_y + 144, body_x + 210, body_y + 78, 0xf8e6bd, 2);
    drawLineSegment(layer, body_x + 82, body_y + 153, body_x + 211, body_y + 81, 0xf8e6bd, 2);

    drawLineSegment(layer, body_x + 45, body_y + 112, body_x + 92, body_y + 140 + strum / 3, foot, 13);
    drawRoundRect(layer, body_x + 88, body_y + 134 + strum / 3, body_x + 109, body_y + 154 + strum / 3, foot, 11, LV_OPA_COVER);
    drawLineSegment(layer, body_x + 140, body_y + 108, body_x + 178, body_y + 93, foot, 12);
    drawRoundRect(layer, body_x + 173, body_y + 86, body_x + 192, body_y + 105, foot, 10, LV_OPA_COVER);
}

void drawBallRunner(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t body = 0x9f88ed;
    const uint32_t body_dark = 0x7e6ccc;
    const uint32_t foot = 0x4d3f82;
    const uint32_t ink = 0x221b35;
    const uint32_t white = 0xffffff;
    const uint32_t ball = 0xf4a629;
    const uint32_t ball_line = 0xa65d15;
    const uint32_t floor_line = 0xaeb4bf;
    const uint32_t shadow = 0xb7b7b7;

    int roll = running ? frame * 6 : 0;
    int bounce = running ? (int)(sinf(frame * 0.44f) * 4.0f) : 0;
    int lean = running ? (int)(sinf(frame * 0.38f) * 3.0f) : 0;
    int step = running ? (int)(sinf(frame * 1.36f) * 7.0f) : 0;
    int eye_shift = running ? (int)(sinf(frame * 0.28f) * 2.0f) : 0;

    int floor_y = y + 205;
    int ball_x = x + 70;
    int ball_y = y + 118;
    int body_x = x + 57 + lean;
    int body_y = y + 30 - bounce;

    drawLineSegment(layer, 0, floor_y, CANVAS_WIDTH - 1, floor_y, floor_line, 2);
    drawRoundRect(layer, x + 91, y + 216, x + 150, y + 224, shadow, 24, LV_OPA_60);

    drawRoundRect(layer, ball_x, ball_y, ball_x + 96, ball_y + 96, ball, 48, LV_OPA_COVER);
    drawLineSegment(layer, ball_x + 12 + (roll % 18) / 3, ball_y + 34, ball_x + 82 - (roll % 18) / 5, ball_y + 24, ball_line, 4);
    drawLineSegment(layer, ball_x + 8 + (roll % 18) / 4, ball_y + 63, ball_x + 87 - (roll % 18) / 6, ball_y + 47, ball_line, 4);
    drawLineSegment(layer, ball_x + 34 - (roll % 18) / 2, ball_y + 4, ball_x + 58 + (roll % 18) / 3, ball_y + 92, ball_line, 4);

    drawLineSegment(layer, body_x + 47, body_y + 115, ball_x + 34 + step, ball_y + 21, foot, 10);
    drawLineSegment(layer, body_x + 85, body_y + 114, ball_x + 65 - step, ball_y + 19, foot, 10);
    drawRoundRect(layer, ball_x + 23 + step, ball_y + 11, ball_x + 47 + step, ball_y + 28, foot, 10, LV_OPA_COVER);
    drawRoundRect(layer, ball_x + 54 - step, ball_y + 10, ball_x + 78 - step, ball_y + 27, foot, 10, LV_OPA_COVER);

    drawRoundRect(layer, body_x, body_y, body_x + 116, body_y + 116, body, 60, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 5, body_y + 82, body_x + 72, body_y + 116, body_dark, 42, LV_OPA_30);

    drawRoundRect(layer, body_x + 61, body_y + 35, body_x + 94, body_y + 73, white, 20, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 92, body_y + 30, body_x + 124, body_y + 67, white, 19, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 77 + eye_shift, body_y + 43, body_x + 93 + eye_shift, body_y + 66, 0x000000, 10, LV_OPA_COVER);
    drawRoundRect(layer, body_x + 107 + eye_shift, body_y + 38, body_x + 123 + eye_shift, body_y + 61, 0x000000, 10, LV_OPA_COVER);

    drawLineSegment(layer, body_x + 83, body_y + 82, body_x + 88, body_y + 92, ink, 3);
    drawLineSegment(layer, body_x + 88, body_y + 92, body_x + 94, body_y + 94, ink, 3);
    drawLineSegment(layer, body_x + 94, body_y + 94, body_x + 101, body_y + 87, ink, 3);
}

void drawPatternDots(lv_layer_t *layer, int selected)
{
    for (int i = 0; i < PET_PATTERN_COUNT; ++i) {
        uint32_t color = i == selected ? 0x111111 : 0xc8ccd3;
        int x = 106 + i * 18;
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
    _pattern_index(PET_PATTERN_GUITAR)
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
    lv_canvas_fill_bg(_canvas, lv_color_hex(SCREEN_BG_COLOR), LV_OPA_COVER);

    if (_pattern_index == PET_PATTERN_BALL) {
        drawBallRunner(&layer, cat_x, 14 + bob, frame, running);
    } else {
        drawGuitarPet(&layer, cat_x, 16 + bob, frame, running);
    }
    drawPatternDots(&layer, _pattern_index);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = lv_color_hex(0x111111);
    label.font = &lv_font_montserrat_24;
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = stateText(state);
    lv_area_t label_area = {.x1 = 0, .y1 = -6, .x2 = CANVAS_WIDTH, .y2 = 22};
    lv_draw_label(&layer, &label, &label_area);

    lv_canvas_finish_layer(_canvas, &layer);
    frame++;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, CodexPetApp, APP_NAME, []()
{
    return std::shared_ptr<CodexPetApp>(CodexPetApp::requestInstance(), [](CodexPetApp *p) {});
})

} // namespace esp_brookesia::apps
