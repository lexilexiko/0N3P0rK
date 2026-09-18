#pragma once

#include <stdint.h>

// Standalone screen dump. Porkchop path: panel RGB → 24-bit BMP on SD.
// Bind the key in SETTINGS → KEYS → SNAP (default unbound).
namespace Screenshot {

static const uint8_t HOTKEY_SLOT = 17;  // Config::hotkeys().key[]

void poll();   // edge-trigger the bound key; safe to call every App::loop
bool take();   // write /0N3P0rK/screenshots/screenshotNNN.bmp
bool busy();

} // namespace Screenshot
