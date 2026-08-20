#pragma once
#include "hardware/display.h"

namespace frame_buffer {

    bool ensureFrameSprite();

    LGFX_Sprite get_s_frame();
    
    lgfx::LovyanGFX* get_s_draw();

    void set_s_draw(lgfx::LovyanGFX* new_s_draw);

}