#pragma once
// Second pig on the farm (unlock lv 40). Own AI; wolf can bite her too.
// Toggle: SCENE → FRIEND
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
bool isFallen();              // on ground after wolf bite (own state)
void onWolfBitten();          // flinch + fall + flee — does NOT stun player

}  // namespace FriendPig
