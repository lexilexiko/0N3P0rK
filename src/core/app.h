#pragma once

#include <stdint.h>

enum class AppMode : uint8_t {
    FARM = 0,
    MENU,
    ATTACK,
    LOOT,
    WIFI,
    PIG,
    TUNE,
    EVILPIG,
    PIGPASS,
    BLE,
    IR,
    SPECTRUM,
    USBSD,
    FILEMGR,
    XFER,
    BADUSB
};

namespace App {

void begin();
void loop();

AppMode mode();
void setMode(AppMode m);
const char* modeName();

bool windowHidden();
void setWindowHidden(bool hid);
bool overlayMode();

}  // namespace App
