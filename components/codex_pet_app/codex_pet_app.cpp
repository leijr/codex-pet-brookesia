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
#define SCREEN_BG_COLOR 0xfff7ff

namespace {

enum PetState {
    PET_STATE_IDLE = 0,
    PET_STATE_RUNNING,
    PET_STATE_DONE,
    PET_STATE_OFFLINE,
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

void drawCat(lv_layer_t *layer, int x, int y, int frame, bool running)
{
    const uint32_t fur = 0x202733;
    const uint32_t fur_light = 0x5c6678;
    const uint32_t fur_dark = 0x10141b;
    const uint32_t ink = 0x0e1117;
    const uint32_t white = 0xf6f7f8;
    const uint32_t blush = 0xff9aa8;
    const uint32_t ball = 0xf59d20;
    const uint32_t ball_light = 0xffca55;
    const uint32_t ball_line = 0xaa5d09;
    int roll = running ? (frame * 5) % 36 : 0;
    int lean = running ? (int)(sinf(frame * 0.42f) * 4.0f) : 0;
    int paw = running ? (int)(sinf(frame * 0.68f) * 4.0f) : 0;
    int tail_wave = running ? (int)(sinf(frame * 0.58f) * 9.0f) : 0;
    int ear_tip = running ? (int)(sinf(frame * 0.8f) * 2.0f) : 0;

    int ball_x1 = x + 55;
    int ball_y1 = y + 77;
    int ball_x2 = x + 151;
    int ball_y2 = y + 173;
    int head_x = x + 15 + lean;
    int head_y = y + 19 - lean / 2;
    int body_x = x + 87 + lean;
    int body_y = y + 34 - lean / 2;

    drawRoundRect(layer, x + 30, y + 171, x + 171, y + 183, 0x111111, 20, LV_OPA_20);

    drawRoundRect(layer, ball_x1, ball_y1, ball_x2, ball_y2, ball, 50, LV_OPA_COVER);
    drawRoundRect(layer, x + 75, y + 88, x + 112, y + 123, ball_light, 20, LV_OPA_30);
    drawLineSegment(layer, x + 62 + roll / 3, y + 103, x + 143 - roll / 5, y + 93, ball_line, 4);
    drawLineSegment(layer, x + 59 + roll / 4, y + 134, x + 145 - roll / 6, y + 118, ball_line, 4);
    drawLineSegment(layer, x + 72 + roll / 2, y + 165, x + 132 - roll / 3, y + 145, ball_line, 4);
    drawLineSegment(layer, x + 92 - roll / 2, y + 80, x + 123 + roll / 3, y + 169, ball_line, 4);
    drawLineSegment(layer, x + 50 + roll / 2, y + 118, x + 103 + roll / 3, y + 174, ball_line, 3);
    drawLineSegment(layer, x + 125 - roll / 3, y + 82, x + 153 - roll / 3, y + 139, ball_line, 3);

    drawRoundRect(layer, body_x, body_y, x + 174 + lean, y + 121, fur, 38, LV_OPA_COVER);
    drawLineSegment(layer, x + 122 + lean, y + 42, x + 155 + lean, y + 82, fur_light, 5);
    drawLineSegment(layer, x + 139 + lean, y + 43, x + 166 + lean, y + 85, fur_light, 4);
    drawLineSegment(layer, x + 162 + lean, y + 83, x + 193, y + 64 + tail_wave, fur, 16);
    drawLineSegment(layer, x + 193, y + 64 + tail_wave, x + 207, y + 39 + tail_wave / 2, fur, 15);
    drawRoundRect(layer, x + 197, y + 27 + tail_wave / 2, x + 222, y + 51 + tail_wave / 2, fur_dark, 14, LV_OPA_COVER);
    drawLineSegment(layer, x + 202, y + 33 + tail_wave / 2, x + 214, y + 28 + tail_wave / 2, fur_light, 4);

    lv_draw_triangle_dsc_t ear;
    lv_draw_triangle_dsc_init(&ear);
    ear.color = lv_color_hex(fur);
    ear.opa = LV_OPA_COVER;
    ear.p[0] = (lv_point_precise_t){head_x + 12, head_y + 16};
    ear.p[1] = (lv_point_precise_t){head_x + 1, head_y - 15 - ear_tip};
    ear.p[2] = (lv_point_precise_t){head_x + 34, head_y + 11};
    lv_draw_triangle(layer, &ear);
    ear.p[0] = (lv_point_precise_t){head_x + 56, head_y + 10};
    ear.p[1] = (lv_point_precise_t){head_x + 73, head_y - 17 + ear_tip};
    ear.p[2] = (lv_point_precise_t){head_x + 83, head_y + 20};
    lv_draw_triangle(layer, &ear);

    ear.color = lv_color_hex(0xe2e6ef);
    ear.p[0] = (lv_point_precise_t){head_x + 13, head_y + 10};
    ear.p[1] = (lv_point_precise_t){head_x + 8, head_y - 3 - ear_tip};
    ear.p[2] = (lv_point_precise_t){head_x + 29, head_y + 10};
    lv_draw_triangle(layer, &ear);
    ear.p[0] = (lv_point_precise_t){head_x + 62, head_y + 9};
    ear.p[1] = (lv_point_precise_t){head_x + 71, head_y - 2 + ear_tip};
    ear.p[2] = (lv_point_precise_t){head_x + 75, head_y + 16};
    lv_draw_triangle(layer, &ear);

    drawRoundRect(layer, head_x, head_y + 2, head_x + 84, head_y + 79, fur, 32, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 10, head_y + 12, head_x + 77, head_y + 69, 0x2c3543, 28, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 46, head_y + 48, head_x + 70, head_y + 63, white, 9, LV_OPA_COVER);

    drawRoundRect(layer, head_x + 24, head_y + 27, head_x + 43, head_y + 54, white, 9, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 50, head_y + 25, head_x + 69, head_y + 52, white, 9, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 34, head_y + 38, head_x + 40, head_y + 49, ink, 4, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 55, head_y + 36, head_x + 61, head_y + 47, ink, 4, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 47, head_y + 55, head_x + 54, head_y + 61, blush, 4, LV_OPA_COVER);
    drawRoundRect(layer, head_x + 49, head_y + 62, head_x + 62, head_y + 67, 0xf7b7c2, 5, LV_OPA_COVER);

    drawLineSegment(layer, head_x + 9, head_y + 52, head_x - 14, head_y + 47, ink, 2);
    drawLineSegment(layer, head_x + 10, head_y + 60, head_x - 12, head_y + 63, ink, 2);
    drawLineSegment(layer, head_x + 75, head_y + 51, head_x + 99, head_y + 43, ink, 2);
    drawLineSegment(layer, head_x + 76, head_y + 59, head_x + 101, head_y + 62, ink, 2);

    drawLineSegment(layer, x + 96, y + 111, x + 137, y + 112, white, 7);
    drawLineSegment(layer, x + 98 + lean, y + 80, x + 92, y + 132 + paw, fur_dark, 15);
    drawRoundRect(layer, x + 84, y + 127 + paw, x + 105, y + 146 + paw, fur_dark, 11, LV_OPA_COVER);
    drawLineSegment(layer, x + 128 + lean, y + 82, x + 123, y + 126 - paw, fur_dark, 13);
    drawRoundRect(layer, x + 114, y + 121 - paw, x + 134, y + 139 - paw, fur_dark, 10, LV_OPA_COVER);
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
    _canvas(nullptr)
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

void CodexPetApp::drawFrame(void)
{
    static int frame = 0;
    if (_canvas == nullptr) {
        return;
    }

    PetState state = s_pet_state;
    bool running = state == PET_STATE_RUNNING;
    int bob = running ? (int)(sinf(frame * 0.45f) * 3.0f) : 0;
    int cat_x = running ? 16 + (int)(sinf(frame * 0.18f) * 2.0f) : 16;

    lv_layer_t layer;
    lv_canvas_init_layer(_canvas, &layer);
    lv_canvas_fill_bg(_canvas, lv_color_hex(SCREEN_BG_COLOR), LV_OPA_COVER);

    drawCat(&layer, cat_x, 52 + bob, frame, running);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.color = lv_color_hex(0x111111);
    label.font = &lv_font_montserrat_24;
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = stateText(state);
    lv_area_t label_area = {.x1 = 0, .y1 = 6, .x2 = CANVAS_WIDTH, .y2 = 34};
    lv_draw_label(&layer, &label, &label_area);

    lv_canvas_finish_layer(_canvas, &layer);
    frame++;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, CodexPetApp, APP_NAME, []()
{
    return std::shared_ptr<CodexPetApp>(CodexPetApp::requestInstance(), [](CodexPetApp *p) {});
})

} // namespace esp_brookesia::apps
