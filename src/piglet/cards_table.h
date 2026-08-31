#pragma once
// Farm table — unlock lv 45. Jump → «Дуэль» (best of 3).
#include <Arduino.h>
#include <M5Unified.h>
#include <stdint.h>

namespace CardsTable {

void begin();
bool unlocked();
bool isActive();
void end();
void update();
void draw(M5Canvas& canvas, int16_t yOffset);
void drawActive(M5Canvas& canvas);
void scroll(int8_t dx);

}  // namespace CardsTable
