// OnePork-style settings: PIG scene sections / SYSTEM / RADIO / BLE
// ^/v move   ENT = toggle or enter value   then ^/v change   ESC back
#pragma once

#include <M5Unified.h>

enum class SettingsPage : uint8_t {
    SCENE = 0,
    SYSTEM = 1,
    RADIO = 2,
    BLE = 3,
    CONNECT = 4,
    KEYS = 5,
    STATUS = 6,
    RADIO_EDIT = 7, // dynamic knobs for current method
    FLORA = 8,
    SCENE_TREES = 9,
    SCENE_EFFECTS = 10,
    SCENE_OTHER = 11
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
