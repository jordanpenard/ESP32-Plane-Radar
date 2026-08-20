#include "hardware/display.h"
#include "ui/frame_buffer.h"

#include "hardware/display_font.h"

LGFX tft;

void displayInit() {
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
  while(!frame_buffer::ensureFrameSprite()) {
    delay(10);
  }
}
