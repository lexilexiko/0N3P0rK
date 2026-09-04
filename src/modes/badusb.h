#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// BadUSB / BadBLE — DuckyScript from SD /0N3P0rK/badusb/*.txt
// Cardputer (ESP32-S3): native USB HID. BLE uses ESP32-BLE-Keyboard.

namespace BadUsbMode {

void start();
void stop();
void update();
void draw(M5Canvas& canvas);
bool isRunning();
void getStatusLine(char* out, size_t n);

}  // namespace BadUsbMode
