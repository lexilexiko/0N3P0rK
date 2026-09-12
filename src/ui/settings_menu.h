// OnePork-style settings: SCENE / RADIO / BLE
// ;/. move   ENT = toggle or enter value   then ;/. change   ` back
#pragma once

#include <M5Unified.h>

enum class SettingsPage : uint8_t {
    SCENE = 0,
    SYSTEM = 1,
    RADIO = 2,
    BLE = 3,
    CONNECT = 4,
    KEYS = 5,
    STATUS = 6
};

namespace SettingsMenu {

void show(SettingsPage page);
void hide();
bool isActive();
bool isTyping();
void update();
void draw(M5Canvas& canvas);
const char* bottomHint();
SettingsPage page();

}  // namespace SettingsMenu
