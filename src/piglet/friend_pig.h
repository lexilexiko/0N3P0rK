#pragma once
// Second pig (lv 40+). Own hearts / zombie / dialogue — not tied to player skin.
#include <Arduino.h>
#include <M5Unified.h>
#include <stdint.h>

namespace FriendPig {

void begin();
bool unlocked();
bool enabled();
void setEnabled(bool on);

void update();
void draw(M5Canvas& canvas, int16_t yOffset);
void scroll(int8_t dx);

int getFeetX();
bool isActive();
bool isFallen();
bool isZombie();              // friend undead — independent of player
void onWolfBitten();          // own damage; 0 hearts → she becomes zombie

}  // namespace FriendPig
