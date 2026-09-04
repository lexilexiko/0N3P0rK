#pragma once
// Farm cards table (lv 45+). Stand in CENTER + G to play. Jump never starts game.
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

// Full-screen / overlay card session (stub until real game ships)
bool isActive();
void drawActive(M5Canvas& canvas);
void close();  // exit overlay

}  // namespace CardsTable
