#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// Own-PC second screen. CONNECT → DESK is the only key: it types a visible
// PowerShell script over USB, installs the packages, and starts the picture.

namespace DeskMode {

void start();
void stop();
// Write /0N3P0rK/on3desk.py on the SD card. ip may be null.
void publishScript(const char* ip);
void update();
void draw(M5Canvas& canvas);
// Live picture is painted on the panel itself (16-bit), not the 8-bit menu canvas.
bool paintDirect();
bool isRunning();
void getStatusLine(char* out, size_t n);

}  // namespace DeskMode
