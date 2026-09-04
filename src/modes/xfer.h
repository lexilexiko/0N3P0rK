#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// WiFi AP file transfer — DOS / Commander style web UI (lab tool).

namespace XferMode {

void start();
void stop();
void update();
void draw(M5Canvas& canvas);
bool isRunning();
void getStatusLine(char* out, size_t n);

}  // namespace XferMode
