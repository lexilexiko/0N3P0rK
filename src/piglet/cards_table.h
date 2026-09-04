#pragma once
// Farm cards table (lv 45+). Stand at table center, press G to play — not jump.
#include <Arduino.h>
#include <M5Unified.h>
#include <stdint.h>

namespace CardsTable {

void begin();
bool unlocked();
void update();
void draw(M5Canvas& canvas, int16_t yOffset);
void scroll(int8_t dx);

bool atCenter();
bool isActive();
void drawActive(M5Canvas& canvas);
void close();

}  // namespace CardsTable
