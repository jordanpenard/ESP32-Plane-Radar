#include "ui/frame_buffer.h"

namespace frame_buffer {

    lgfx::LovyanGFX* s_draw = &tft;
    LGFX_Sprite s_frame(&tft);
    bool s_frame_ready = false;

    bool ensureFrameSprite() {
        if (s_frame_ready) {
            return true;
        }
        s_frame.setColorDepth(config::kDisplayColorDepth);
        #ifdef BOARD_HAS_PSRAM
        s_frame.setPsram(true);
        #endif
        if (!s_frame.createSprite(config::kDisplayWidth, config::kDisplayHeight)) {
            Serial.println("frame sprite alloc failed");
            return false;
        }
        s_frame_ready = true;
        return true;
    }

    LGFX_Sprite get_s_frame() {
        if (s_frame_ready) {
            return s_frame;
        } else {
            return NULL;
        }
    }

    lgfx::LovyanGFX* get_s_draw() {
        return s_draw;
    }

    void set_s_draw(lgfx::LovyanGFX* new_s_draw) {
        s_draw = new_s_draw;
    }

}
