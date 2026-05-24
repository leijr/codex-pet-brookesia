#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class CodexPetApp: public systems::phone::App {
public:
    static CodexPetApp *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~CodexPetApp() override;

protected:
    CodexPetApp(bool use_status_bar, bool use_navigation_bar);

    bool init(void) override;
    bool run(void) override;
    bool back(void) override;
    bool cleanResource(void) override;

private:
    void drawFrame(void);
    static void timerCallback(lv_timer_t *timer);
    static void gestureCallback(lv_event_t *event);

    static CodexPetApp *_instance;
    lv_draw_buf_t *_draw_buf;
    lv_obj_t *_canvas;
    int _pattern_index;
};

} // namespace esp_brookesia::apps
