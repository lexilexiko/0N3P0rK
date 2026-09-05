// Cardputer built-in WS2812 RGB (GPIO21) — status LED like Bruce.
#pragma once

#include <stdint.h>

namespace Led {

void begin();
void update();   // call from main loop — non-blocking blink/status
void off();
void setRgb(uint8_t r, uint8_t g, uint8_t b);
void pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t onMs = 120);

// Apply brightness from Config (0..100).
void applyBrightness();

}  // namespace Led
